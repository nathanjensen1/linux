/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_FENCE_H__
#define __PVR_FENCE_H__

#include "pvr_device.h"

#include <linux/bits.h>
#include <linux/dma-fence.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/* Forward declaration from pvr_gem.h. */
struct pvr_fw_object;

/* Forward declarations from pvr_rogue_fwif_shared.h. */
struct rogue_fwif_sync_checkpoint;
struct rogue_fwif_ufo;

/**
 * &struct pvr_fence_context - PowerVR fence context
 */
struct pvr_fence_context {
	/** @pvr_dev: Owning PowerVR device. */
	struct pvr_device *pvr_dev;

	/** @fence_spinlock: Lock used by pvr_fence. */
	spinlock_t fence_spinlock;

	/** @fence_id: Next ID to be assigned when creating fences. */
	atomic_t fence_id;

	/** @fence_context: Device fence context. */
	u64 fence_context;

	/** @timeline_name: Name of timeline this fence context represents. */
	char timeline_name[32];
};

/**
 * &struct pvr_fence - PowerVR fence structure
 */
struct pvr_fence {
	/** @base: Base DMA fence backing this pvr_fence. */
	struct dma_fence base;

	/** @fence_context: Owning fence context. */
	struct pvr_fence_context *context;

	/** @head: List head for this fence. */
	struct list_head head;

	/**
	 * @sync_checkpoint_fw_obj: FW object representing the sync checkpoint structure for this
	 *                          fence.
	 */
	struct pvr_fw_object *sync_checkpoint_fw_obj;

	/** @sync_checkpoint: CPU mapping of sync checkpoint structure for this fence. */
	struct rogue_fwif_sync_checkpoint *sync_checkpoint;

	/**
	 * @dep_list: List of fences this fence depends on. All fences in this list will be
	 * released when this fence is signalled or destroyed.
	 */
	struct list_head dep_list;

	/** @dep_head: Dependency list head for this fence. */
	struct list_head dep_head;
};

extern const struct dma_fence_ops pvr_fence_ops;

void
pvr_fence_device_init(struct pvr_device *pvr_dev);
void
pvr_fence_context_init(struct pvr_device *pvr_dev, struct pvr_fence_context *context,
		       const char *name);
struct dma_fence *
pvr_fence_create(struct pvr_fence_context *context);
int
pvr_fence_to_ufo(struct dma_fence *fence, struct rogue_fwif_ufo *ufo);

static __always_inline struct dma_fence *
from_pvr_fence(struct pvr_fence *pvr_fence)
{
	return &pvr_fence->base;
}

static __always_inline struct pvr_fence *
to_pvr_fence(struct dma_fence *fence)
{
	if (fence->ops == &pvr_fence_ops)
		return container_of(fence, struct pvr_fence, base);

	return NULL;
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
static __always_inline int
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

	list_add_tail(&pvr_dep_fence->dep_head, &pvr_fence->dep_list);

	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

	return 0;

err_unlock:
	spin_unlock_irqrestore(&pvr_dev->fence_list_spinlock, flags);

err_out:
	return err;
}

#endif /* __PVR_FENCE_H__ */
