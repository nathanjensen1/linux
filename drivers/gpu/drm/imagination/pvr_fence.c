// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_context.h"
#include "pvr_device.h"
#include "pvr_drv.h"
#include "pvr_fence.h"
#include "pvr_gem.h"
#include "pvr_power.h"
#include "pvr_rogue_cr_defs.h"
#include "pvr_rogue_fwif.h"

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define PVR_IMPORTED_TIMELINE_NAME "imported"

/**
 * pvr_fence_create() - Create a PowerVR fence
 * @context: Target PowerVR fence context
 * @pvr_ctx: Associated PowerVR context. May be %NULL.
 *
 * The fence will be created with two references; one for the caller, one for the fence worker. The
 * caller's reference (and any other references subsequently taken) should be released with
 * dma_fence_put(). If the fence will not be signaled (e.g. on an error path) then the fence worker
 * reference should be manually dropped via pvr_fence_deactivate_and_put().
 *
 * Returns:
 *  * Pointer to the new fence on success,
 *  * -%ENOMEM on out of memory, or
 *  * Any error returned by pvr_gem_create_and_map_fw_object().
 */
struct dma_fence *
pvr_fence_create(struct pvr_fence_context *context, struct pvr_context *pvr_ctx)
{
	struct pvr_device *pvr_dev = context->pvr_dev;
	struct pvr_fence *pvr_fence;
	unsigned long flags;
	int err;

	pvr_fence = kzalloc(sizeof(*pvr_fence), GFP_KERNEL);
	if (!pvr_fence) {
		err = -ENOMEM;
		goto err_out;
	}

	pvr_fence->context = context;
	pvr_fence->sync_checkpoint =
		pvr_gem_create_and_map_fw_object(pvr_dev, sizeof(*pvr_fence->sync_checkpoint),
						 PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
						 DRM_PVR_BO_CREATE_ZEROED,
						 &pvr_fence->sync_checkpoint_fw_obj);
	if (IS_ERR(pvr_fence->sync_checkpoint)) {
		err = PTR_ERR(pvr_fence->sync_checkpoint);
		goto err_free;
	}
	pvr_fence->pvr_ctx = pvr_context_get(pvr_ctx);

	pvr_fence->sync_checkpoint->state = PVR_SYNC_CHECKPOINT_ACTIVE;

	INIT_LIST_HEAD(&pvr_fence->dep_list);
	INIT_LIST_HEAD(&pvr_fence->dep_head);

	dma_fence_init(&pvr_fence->base, &pvr_fence_ops, &context->fence_spinlock,
		       context->fence_context, atomic_inc_return(&context->fence_id));

	/*
	 * The initial reference on this fence will be passed to the fence list as soon as the
	 * fence is added to that list. Take another reference before then to hand back to the
	 * caller.
	 */
	dma_fence_get(&pvr_fence->base);

	spin_lock_irqsave(&pvr_dev->fence_list_spinlock, flags);
	list_add_tail(&pvr_fence->head, &pvr_dev->fence_list);
	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

	return from_pvr_fence(pvr_fence);

err_free:
	kfree(pvr_fence);

err_out:
	return ERR_PTR(err);
}

static void
pvr_fence_release_dep_fences(struct pvr_fence *pvr_fence)
{
	struct pvr_device *pvr_dev = pvr_fence->context->pvr_dev;
	struct pvr_fence *dep_fence;
	struct pvr_fence *tmp;
	unsigned long flags;

	LIST_HEAD(dep_fence_list);

	spin_lock_irqsave(&pvr_dev->fence_list_spinlock, flags);

	list_for_each_entry_safe(dep_fence, tmp, &pvr_fence->dep_list, dep_head)
		list_move_tail(&dep_fence->dep_head, &dep_fence_list);

	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

	list_for_each_entry_safe(dep_fence, tmp, &dep_fence_list, dep_head) {
		list_del_init(&dep_fence->dep_head);
		dma_fence_put(&dep_fence->base);
	}
}

static void
pvr_fence_destroy(struct pvr_fence *pvr_fence)
{
	struct pvr_device *pvr_dev = pvr_fence->context->pvr_dev;
	unsigned long flags;

	spin_lock_irqsave(&pvr_dev->fence_list_spinlock, flags);
	list_del(&pvr_fence->head);
	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

	pvr_fence_release_dep_fences(pvr_fence);

	pvr_fw_object_vunmap(pvr_fence->sync_checkpoint_fw_obj, pvr_fence->sync_checkpoint, false);
	pvr_fw_object_release(pvr_fence->sync_checkpoint_fw_obj);

	if (pvr_fence->pvr_ctx)
		pvr_context_put(pvr_fence->pvr_ctx);

	BUILD_BUG_ON(offsetof(typeof(*pvr_fence), base));
	dma_fence_free(&pvr_fence->base);
}

static const char *
pvr_fence_get_driver_name(struct dma_fence *fence)
{
	return PVR_DRIVER_NAME;
}

static const char *
pvr_fence_get_timeline_name(struct dma_fence *fence)
{
	struct pvr_fence *pvr_fence = to_pvr_fence(fence);

	return (const char *)pvr_fence->context->timeline_name;
}

static bool
pvr_fence_is_signaled(struct dma_fence *fence)
{
	struct pvr_fence *pvr_fence = to_pvr_fence(fence);

	return (pvr_fence->sync_checkpoint->state == PVR_SYNC_CHECKPOINT_ERRORED) ||
	       (pvr_fence->sync_checkpoint->state == PVR_SYNC_CHECKPOINT_SIGNALED);
}

static void
pvr_fence_release(struct dma_fence *fence)
{
	struct pvr_fence *pvr_fence = to_pvr_fence(fence);

	pvr_fence_destroy(pvr_fence);
}

const struct dma_fence_ops pvr_fence_ops = {
	.get_driver_name = pvr_fence_get_driver_name,
	.get_timeline_name = pvr_fence_get_timeline_name,
	.release = pvr_fence_release,
	.signaled = pvr_fence_is_signaled,
};

/**
 * pvr_fence_process_worker() - Process any completed fences
 * @pvr_dev: Target PowerVR device.
 */
static void
pvr_fence_process_worker(struct work_struct *work)
{
	struct pvr_device *pvr_dev = container_of(work, struct pvr_device, fence_work);
	struct pvr_fence *pvr_fence;
	struct pvr_fence *tmp;
	unsigned long flags;

	LIST_HEAD(signaled_list);

	spin_lock_irqsave(&pvr_dev->fence_list_spinlock, flags);

	/* Move any signaled fences to the signaled list for further processing. */
	list_for_each_entry_safe(pvr_fence, tmp, &pvr_dev->fence_list, head) {
		if (pvr_fence_is_signaled(&pvr_fence->base))
			list_move_tail(&pvr_fence->head, &signaled_list);
	}

	/* Finished with device fence list, can now drop lock. */
	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

	list_for_each_entry_safe(pvr_fence, tmp, &signaled_list, head) {
		list_del_init(&pvr_fence->head);

		/* Signal fence and drop our reference. */
		dma_fence_signal(&pvr_fence->base);
		pvr_fence_release_dep_fences(pvr_fence);
		dma_fence_put(&pvr_fence->base);
	}
}

/**
 * pvr_fence_device_init() - Initialise fence handling for PowerVR device
 * @pvr_dev: Target PowerVR device.
 */
void
pvr_fence_device_init(struct pvr_device *pvr_dev)
{
	spin_lock_init(&pvr_dev->fence_list_spinlock);
	INIT_LIST_HEAD(&pvr_dev->fence_list);
	INIT_LIST_HEAD(&pvr_dev->imported_fence_list);
	INIT_WORK(&pvr_dev->fence_work, pvr_fence_process_worker);
}

/**
 * pvr_fence_context_init() - Initialise fence context
 * @pvr_dev: Target PowerVR device.
 * @context: Pointer to fence context to initialise.
 * @name: Name of timeline this fence context represents.
 */
void
pvr_fence_context_init(struct pvr_device *pvr_dev, struct pvr_fence_context *context,
		       const char *name)
{
	context->pvr_dev = pvr_dev;
	spin_lock_init(&context->fence_spinlock);
	atomic_set(&context->fence_id, 0);
	context->fence_context = dma_fence_context_alloc(1);
	strncpy(context->timeline_name, name, sizeof(context->timeline_name) - 1);
}

/**
 * pvr_fence_to_ufo() - Create a UFO representation of a pvr_fence, for use by firmware
 * @fence: Pointer to fence to convert.
 * @ufo: Location to write UFO representation.
 *
 * Returns:
 *  * 0 on success,
 *  * -%EINVAL if provided fence is not a &struct pvr_fence, or
 *  * -%ENOMEM if provided fence is not mapped to firmware.
 */
int
pvr_fence_to_ufo(struct dma_fence *fence, struct rogue_fwif_ufo *ufo)
{
	struct pvr_fence *pvr_fence = to_pvr_fence(fence);

	if (unlikely(!pvr_fence))
		return -EINVAL;

	pvr_gem_get_fw_addr(pvr_fence->sync_checkpoint_fw_obj, &ufo->addr);

	ufo->addr |= ROGUE_FWIF_UFO_ADDR_IS_SYNC_CHECKPOINT;
	ufo->value = PVR_SYNC_CHECKPOINT_ACTIVE;

	return 0;
}

static const char *
pvr_fence_imported_get_timeline_name(struct dma_fence *fence)
{
	return PVR_IMPORTED_TIMELINE_NAME;
}

static void
pvr_fence_imported_release(struct dma_fence *fence)
{
	struct pvr_fence *pvr_fence = to_pvr_fence(fence);

	if (pvr_fence) {
		dma_fence_put(pvr_fence->imported_fence);
		pvr_fence_destroy(pvr_fence);
	}
}

const struct dma_fence_ops pvr_fence_imported_ops = {
	.get_driver_name = pvr_fence_get_driver_name,
	.get_timeline_name = pvr_fence_imported_get_timeline_name,
	.release = pvr_fence_imported_release,
	.signaled = pvr_fence_is_signaled,
};

static void pvr_fence_imported_signal_worker(struct work_struct *work)
{
	struct pvr_fence *pvr_fence = container_of(work, struct pvr_fence, signal_work);
	struct pvr_device *pvr_dev = pvr_fence->context->pvr_dev;
	unsigned long flags;

	spin_lock_irqsave(&pvr_dev->fence_list_spinlock, flags);
	list_del_init(&pvr_fence->head);
	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

	pvr_fence->sync_checkpoint->state = PVR_SYNC_CHECKPOINT_SIGNALED;

	/* Signal fence and drop our reference. */
	dma_fence_signal(&pvr_fence->base);
	pvr_fence_release_dep_fences(pvr_fence);
	dma_fence_put(&pvr_fence->base);

	/* Sent uncounted kick to FW. */
	mutex_lock(&pvr_dev->power_lock);

	if (!pvr_power_set_state(pvr_dev, PVR_POWER_STATE_ON)) {
		pvr_fw_mts_schedule(pvr_dev, (PVR_FWIF_DM_GP & ~ROGUE_CR_MTS_SCHEDULE_DM_CLRMSK) |
					     ROGUE_CR_MTS_SCHEDULE_TASK_NON_COUNTED);
	}

	mutex_unlock(&pvr_dev->power_lock);
}

static void pvr_fence_imported_signal(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct pvr_fence *pvr_fence = container_of(cb, struct pvr_fence, cb);
	struct pvr_device *pvr_dev = pvr_fence->context->pvr_dev;

	/* Callback might be called from atomic context, so handle signal in workqueue. */
	queue_work(pvr_dev->irq_wq, &pvr_fence->signal_work);
}

/**
 * pvr_fence_import() - Create a PowerVR fence from an existing dma_fence
 * @context: Target PowerVR fence context.
 * @imported_fence: Existing fence to create the new fence from.
 *
 * The fence will be created with two references; one for the caller, one for the fence worker. The
 * caller's reference (and any other references subsequently taken) should be released with
 * dma_fence_put(). If the fence is not subsequently used (e.g. on an error path) then the fence
 * worker reference should be manually dropped via pvr_fence_deactivate_and_put(); this prevents any
 * race conditions due to uncertainty about whether the fence will be signaled or not.
 *
 * Returns:
 *  * Pointer to the new fence on success,
 *  * -%ENOMEM on out of memory, or
 *  * Any error returned by pvr_gem_create_and_map_fw_object().
 */
struct dma_fence *
pvr_fence_import(struct pvr_fence_context *context, struct dma_fence *imported_fence)
{
	struct pvr_device *pvr_dev = context->pvr_dev;
	struct pvr_fence *pvr_fence;
	unsigned long flags;
	int err;

	pvr_fence = kzalloc(sizeof(*pvr_fence), GFP_KERNEL);
	if (!pvr_fence) {
		err = -ENOMEM;
		goto err_out;
	}

	pvr_fence->flags = PVR_FENCE_FLAGS_IMPORTED;
	pvr_fence->imported_fence = imported_fence;
	pvr_fence->context = context;
	pvr_fence->sync_checkpoint =
		pvr_gem_create_and_map_fw_object(pvr_dev, sizeof(*pvr_fence->sync_checkpoint),
						 PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
						 DRM_PVR_BO_CREATE_ZEROED,
						 &pvr_fence->sync_checkpoint_fw_obj);
	if (IS_ERR(pvr_fence->sync_checkpoint)) {
		err = PTR_ERR(pvr_fence->sync_checkpoint);
		goto err_free;
	}

	pvr_fence->sync_checkpoint->state = PVR_SYNC_CHECKPOINT_ACTIVE;

	INIT_LIST_HEAD(&pvr_fence->dep_list);
	INIT_LIST_HEAD(&pvr_fence->dep_head);
	INIT_WORK(&pvr_fence->signal_work, pvr_fence_imported_signal_worker);

	dma_fence_init(&pvr_fence->base, &pvr_fence_imported_ops, &context->fence_spinlock,
		       context->fence_context, atomic_inc_return(&context->fence_id));

	/*
	 * The initial reference on this fence will be passed to the fence list as soon as the
	 * fence is added to that list. Take another reference before then to hand back to the
	 * caller.
	 */
	dma_fence_get(&pvr_fence->base);

	spin_lock_irqsave(&pvr_dev->fence_list_spinlock, flags);
	list_add_tail(&pvr_fence->head, &pvr_dev->imported_fence_list);
	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

	err = dma_fence_add_callback(imported_fence, &pvr_fence->cb, pvr_fence_imported_signal);
	if (err == -ENOENT) {
		/* Fence has already signaled. Call callback directly. */
		pvr_fence_imported_signal(&pvr_fence->base, &pvr_fence->cb);
	} else if (err) {
		goto err_fw_obj_release;
	}

	return from_pvr_fence(pvr_fence);

err_fw_obj_release:
	pvr_fw_object_vunmap(pvr_fence->sync_checkpoint_fw_obj, pvr_fence->sync_checkpoint, false);
	pvr_fw_object_release(pvr_fence->sync_checkpoint_fw_obj);

err_free:
	kfree(pvr_fence);

err_out:
	return ERR_PTR(err);
}

/**
 * pvr_fence_deactivate_and_put() - Deactivate a fence and drop the fence worker's reference
 * @fence: Pointer to fence to deactivate.
 *
 * As it is possible that the fence has already signaled and fence worker reference has been
 * dropped, the caller should hold an additional reference on the fence.
 */
void
pvr_fence_deactivate_and_put(struct dma_fence *fence)
{
	struct pvr_fence *pvr_fence = to_pvr_fence(fence);
	struct pvr_device *pvr_dev;
	unsigned long flags;

	if (!pvr_fence)
		return;

	pvr_dev = pvr_fence->context->pvr_dev;

	/* Remove from the global fence list. */
	spin_lock_irqsave(&pvr_dev->fence_list_spinlock, flags);
	list_del_init(&pvr_fence->head);
	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

	if (pvr_fence->flags & PVR_FENCE_FLAGS_IMPORTED) {
		if (!dma_fence_remove_callback(pvr_fence->imported_fence, &pvr_fence->cb)) {
			/*
			 * Parent fence has already signaled. Flush the signal work; the
			 * reference will be released by the worker.
			 */
			flush_work(&pvr_fence->signal_work);
		} else {
			/* Parent fence has not signaled, we can drop the reference now. */
			dma_fence_put(fence);
		}
	} else {
		/*
		 * For native fences, this function should only be called if it is known
		 * that the fence will never be signaled (e.g. the update command for this
		 * fence was never submitted). Just drop the reference.
		 */
		dma_fence_put(fence);
	}
}

/**
 * pvr_fence_add_fence_dependency() - Add dependency to pvr_fence
 * @fence: Target fence.
 * @dep_fence: Dependency to add.
 *
 * Dependency will be released when target fence is signalled or destroyed.
 *
 * Returns:
 *  * 0 on success,
 *  * -%EINVAL if provided fences are not pvr_fences, or
 *  * -%EINVAL if dependency is already attached to a &struct pvr_fence.
 */
int
pvr_fence_add_fence_dependency(struct dma_fence *fence, struct dma_fence *dep_fence)
{
	struct pvr_fence *pvr_dep_fence = to_pvr_fence(dep_fence);
	struct pvr_fence *pvr_fence = to_pvr_fence(fence);
	struct pvr_device *pvr_dev;
	unsigned long flags;
	int err;

	if (!pvr_fence || !pvr_dep_fence) {
		err = -EINVAL;
		goto err_out;
	}

	pvr_dev = pvr_fence->context->pvr_dev;
	spin_lock_irqsave(&pvr_dev->fence_list_spinlock, flags);

	if (!list_empty(&pvr_dep_fence->dep_head)) {
		err = -EINVAL;
		goto err_unlock;
	}

	dma_fence_get(dep_fence);
	list_add_tail(&pvr_dep_fence->dep_head, &pvr_fence->dep_list);

	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

	return 0;

err_unlock:
	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

err_out:
	return err;
}
