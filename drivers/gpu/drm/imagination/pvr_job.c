// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_context.h"
#include "pvr_device.h"
#include "pvr_fence.h"
#include "pvr_gem.h"
#include "pvr_hwrt.h"
#include "pvr_job.h"
#include "pvr_rogue_fwif.h"
#include "pvr_rogue_fwif_client.h"
#include "pvr_stream.h"
#include "pvr_stream_defs.h"

#include <drm/drm_gem.h>
#include <drm/drm_syncobj.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/ww_mutex.h>
#include <linux/xarray.h>
#include <uapi/drm/pvr_drm.h>

static struct pvr_job *pvr_create_job(struct pvr_device *pvr_dev, enum pvr_job_type job_type)
{
	struct pvr_job *job = kzalloc(sizeof(*job), GFP_KERNEL);
	int err;

	if (!job) {
		err = -ENOMEM;
		goto err_out;
	}

	job->pvr_dev = pvr_dev;
	job->type = job_type;

	switch (job_type) {
	case PVR_JOB_TYPE_GEOMETRY:
		job->fw_ccb_cmd_type = ROGUE_FWIF_CCB_CMD_TYPE_GEOM;
		break;

	case PVR_JOB_TYPE_FRAGMENT:
		job->fw_ccb_cmd_type = ROGUE_FWIF_CCB_CMD_TYPE_FRAG;
		break;

	case PVR_JOB_TYPE_COMPUTE:
		job->fw_ccb_cmd_type = ROGUE_FWIF_CCB_CMD_TYPE_CDM;
		break;

	case PVR_JOB_TYPE_TRANSFER:
		job->fw_ccb_cmd_type = ROGUE_FWIF_CCB_CMD_TYPE_TQ_3D;
		break;

	default:
		err = -EINVAL;
		goto err_free;
	}

	xa_init_flags(&job->deps.non_native, XA_FLAGS_ALLOC);
	xa_init_flags(&job->deps.native, XA_FLAGS_ALLOC);
	kref_init(&job->ref_count);

	err = xa_alloc(&pvr_dev->job_ids, &job->id, job, xa_limit_32b, GFP_KERNEL);
	if (err)
		goto err_free;

	return job;

err_free:
	kfree(job);

err_out:
	return ERR_PTR(err);
}

static void release_fences(struct xarray *in_fences)
{
	struct dma_fence *fence;
	unsigned long id;

	xa_for_each(in_fences, id, fence)
		dma_fence_put(fence);

	xa_destroy(in_fences);
}

static void pvr_job_release(struct kref *kref)
{
	struct pvr_job *job = container_of(kref, struct pvr_job, ref_count);

	xa_erase(&job->pvr_dev->job_ids, job->id);

	pvr_hwrt_data_put(job->hwrt);
	pvr_context_put(job->ctx);

	if (job->deps.cur) {
		dma_fence_remove_callback(job->deps.cur, &job->deps.cb);
		dma_fence_put(job->deps.cur);
	}

	release_fences(&job->deps.non_native);
	release_fences(&job->deps.native);

	dma_fence_put(job->done_fence);
	kfree(job->cmd);
	kfree(job);
}

static void pvr_job_dep_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct pvr_job *job = container_of(cb, struct pvr_job, deps.cb);
	struct pvr_context *ctx = job->ctx;

	pvr_context_pending_job_event(ctx);
}

bool pvr_job_non_native_deps_done(struct pvr_job *job)
{
	/* The current fence is a native fence, meaning all non-native deps are done. */
	if (job->deps.cur && to_pvr_context_queue_fence(job->deps.cur))
		return true;

	/* No more non-native deps to wait on. */
	if (!job->deps.cur && xa_empty(&job->deps.non_native))
		return true;

	/* Current non-native fence is still unsignaled. */
	if (job->deps.cur && !dma_fence_is_signaled(job->deps.cur))
		return false;

	dma_fence_put(job->deps.cur);
	job->deps.cur = NULL;

	while (!xa_empty(&job->deps.non_native)) {
		struct dma_fence *next_dep;
		int ret;

		next_dep = xa_erase(&job->deps.non_native, job->deps.next_index++);
		ret = dma_fence_add_callback(next_dep, &job->deps.cb, pvr_job_dep_cb);
		if (!ret) {
			job->deps.cur = next_dep;
			break;
		}

		WARN_ON(ret != -ENOENT);
		dma_fence_put(next_dep);
	}

	/* Reset the index so it can be used to iterate over the native array. */
	if (xa_empty(&job->deps.non_native))
		job->deps.next_index = 0;

	return !job->deps.cur;
}

/**
 * pvr_job_evict_signaled_native_deps() - Evict signaled native deps
 * @job: Job to operate on.
 */
void pvr_job_evict_signaled_native_deps(struct pvr_job *job)
{
	struct dma_fence *fence = NULL;
	unsigned long index;

	if (job->deps.cur && dma_fence_is_signaled(job->deps.cur)) {
		if (!WARN_ON(!to_pvr_context_queue_fence(job->deps.cur)))
			job->deps.native_count--;

		dma_fence_put(job->deps.cur);
		job->deps.cur = NULL;
	}

	xa_for_each_start(&job->deps.native, index, fence, job->deps.next_index) {
		if (dma_fence_is_signaled(fence)) {
			xa_erase(&job->deps.native, index);
			dma_fence_put(fence);
			job->deps.native_count--;
		}
	}
}

/**
 * pvr_job_wait_first_non_signaled_native_dep() - Register a fence callback on the first
 *						  non-signaled native dep
 * @job: Job to operate on.
 *
 * Returns:
 *  * 0 on success,
 *  * or -ENOENT if there's no fence to wait on.
 */
int pvr_job_wait_first_non_signaled_native_dep(struct pvr_job *job)
{
	struct dma_fence *fence;
	unsigned long index;

	if (job->deps.cur)
		return 0;

	xa_for_each_start(&job->deps.native, index, fence, job->deps.next_index) {
		int err;

		xa_erase(&job->deps.native, index);
		err = dma_fence_add_callback(fence, &job->deps.cb, pvr_job_dep_cb);
		if (!err) {
			job->deps.cur = fence;
			job->deps.next_index = index + 1;
			return 0;
		}

		WARN_ON(err != -ENOENT);
		job->deps.native_count--;
		dma_fence_put(fence);
	}

	return -ENOENT;
}

struct pvr_cccb *
get_cccb(struct pvr_job *job)
{
	switch (job->type) {
	case PVR_JOB_TYPE_GEOMETRY:
		return &container_of(job->ctx, struct pvr_context_render, base)->ctx_geom.cccb;

	case PVR_JOB_TYPE_FRAGMENT:
		return &container_of(job->ctx, struct pvr_context_render, base)->ctx_frag.cccb;

	case PVR_JOB_TYPE_COMPUTE:
		return &container_of(job->ctx, struct pvr_context_compute, base)->cccb;

	case PVR_JOB_TYPE_TRANSFER:
		return &container_of(job->ctx, struct pvr_context_transfer, base)->cccb;

	default:
		return NULL;
	}
}

struct pvr_context_queue *
get_ctx_queue(struct pvr_job *job)
{
	switch (job->type) {
	case PVR_JOB_TYPE_GEOMETRY:
		return &container_of(job->ctx, struct pvr_context_render, base)->ctx_geom.queue;

	case PVR_JOB_TYPE_FRAGMENT:
		return &container_of(job->ctx, struct pvr_context_render, base)->ctx_frag.queue;

	case PVR_JOB_TYPE_COMPUTE:
		return &container_of(job->ctx, struct pvr_context_compute, base)->queue;

	case PVR_JOB_TYPE_TRANSFER:
		return &container_of(job->ctx, struct pvr_context_transfer, base)->queue;

	default:
		return NULL;
	}
}

static u32 get_ctx_fw_addr(struct pvr_job *job)
{
	struct pvr_fw_object *ctx_fw_obj = NULL;
	u32 ctx_fw_addr;

	switch (job->type) {
	case PVR_JOB_TYPE_GEOMETRY:
	case PVR_JOB_TYPE_FRAGMENT:
		ctx_fw_obj = container_of(job->ctx, struct pvr_context_render, base)->fw_obj;
		break;

	case PVR_JOB_TYPE_COMPUTE:
		ctx_fw_obj = container_of(job->ctx, struct pvr_context_compute, base)->fw_obj;
		break;

	case PVR_JOB_TYPE_TRANSFER:
		ctx_fw_obj = container_of(job->ctx, struct pvr_context_transfer, base)->fw_obj;
		break;

	default:
		WARN_ON(1);
		return 0;
	}

	pvr_gem_get_fw_addr(ctx_fw_obj, &ctx_fw_addr);

	if (job->type == PVR_JOB_TYPE_FRAGMENT)
		ctx_fw_addr += offsetof(struct rogue_fwif_fwrendercontext, frag_context);

	return ctx_fw_addr;
}

/**
 * pvr_job_fits_in_cccb() - Check if a job fits in CCCB
 * @job: Job to check.
 *
 * Returns:
 *  * 0 on success,
 *  * or -E2BIG if the CCCB is too small to ever hold the commands for this job,
 *  * or -ENOMEM if the CCCB doesn't have enough memory to hold the commands for
 *    this job at the moment.
 */
int pvr_job_fits_in_cccb(struct pvr_job *job)
{
	/* One UFO for job done signaling, and one per remaining native fence. */
	u32 ufo_op_count = 1 + job->deps.native_count;

	u32 size = (pvr_cccb_get_size_of_cmd_with_hdr(sizeof(struct rogue_fwif_ufo)) *
		    ufo_op_count) +
		   pvr_cccb_get_size_of_cmd_with_hdr(job->cmd_len);
	struct pvr_cccb *cccb = get_cccb(job);

	return pvr_cccb_check_command_space(cccb, size);
}

void pvr_job_submit(struct pvr_job *job)
{
	struct pvr_context_queue *queue = get_ctx_queue(job);
	struct pvr_device *pvr_dev = job->pvr_dev;
	struct pvr_context_queue_fence *qfence;
	struct pvr_cccb *cccb = get_cccb(job);
	struct rogue_fwif_ufo queue_ufo;
	u32 ctx_fw_addr = get_ctx_fw_addr(job);
	struct dma_fence *fence;
	unsigned long index;
	int err;

	if (WARN_ON(!queue || !cccb))
		return;

	pvr_cccb_lock(cccb);

	spin_lock(&pvr_dev->active_contexts.lock);
	if (list_empty(&job->ctx->active_node))
		list_add_tail(&job->ctx->active_node, &pvr_dev->active_contexts.list);
	spin_lock(&queue->jobs.lock);
	list_move_tail(&job->node, &queue->jobs.in_flight);
	spin_unlock(&queue->jobs.lock);
	spin_unlock(&pvr_dev->active_contexts.lock);

	qfence = to_pvr_context_queue_fence(job->deps.cur);
	WARN_ON(job->deps.cur && !qfence);
	if (qfence) {
		pvr_gem_get_fw_addr(qfence->ctx->timeline_ufo.fw_obj, &queue_ufo.addr);
		queue_ufo.value = fence->seqno;
		err = pvr_cccb_write_command_with_header(cccb, ROGUE_FWIF_CCB_CMD_TYPE_FENCE,
							 sizeof(queue_ufo), &queue_ufo, 0, 0);
		if (WARN_ON(err))
			goto err_cccb_unlock_rollback;
	}

	xa_for_each(&job->deps.native, index, fence) {
		qfence = to_pvr_context_queue_fence(fence);
		if (WARN_ON(!qfence))
			continue;

		pvr_gem_get_fw_addr(qfence->ctx->timeline_ufo.fw_obj, &queue_ufo.addr);
		queue_ufo.value = fence->seqno;
		err = pvr_cccb_write_command_with_header(cccb, ROGUE_FWIF_CCB_CMD_TYPE_FENCE,
							 sizeof(queue_ufo), &queue_ufo, 0, 0);
		if (WARN_ON(err))
			goto err_cccb_unlock_rollback;
	}

	/* Submit job to FW */
	err = pvr_cccb_write_command_with_header(cccb, job->fw_ccb_cmd_type, job->cmd_len, job->cmd,
						 job->id, job->id);
	if (WARN_ON(err))
		goto err_cccb_unlock_rollback;

	pvr_gem_get_fw_addr(queue->fence_ctx->timeline_ufo.fw_obj, &queue_ufo.addr);
	queue_ufo.value = job->done_fence->seqno;
	err = pvr_cccb_write_command_with_header(cccb, ROGUE_FWIF_CCB_CMD_TYPE_UPDATE,
						 sizeof(queue_ufo), &queue_ufo, 0, 0);
	if (WARN_ON(err))
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_unlock_send_kccb_kick(pvr_dev, cccb, ctx_fw_addr, job->hwrt);
	if (WARN_ON(err))
		goto err_cccb_unlock_rollback;

	return;

err_cccb_unlock_rollback:
	spin_lock(&pvr_dev->active_contexts.lock);
	spin_lock(&queue->jobs.lock);
	list_move(&job->node, &queue->jobs.pending);
	spin_unlock(&queue->jobs.lock);
	if (!pvr_context_has_in_flight_jobs(job->ctx))
		list_del_init(&job->ctx->active_node);
	spin_unlock(&pvr_dev->active_contexts.lock);
	pvr_cccb_unlock_rollback(cccb);
}

static void pvr_job_push(struct pvr_job *job)
{
	struct pvr_context_queue *queue = get_ctx_queue(job);

	pvr_job_get(job);

	spin_lock(&queue->jobs.lock);
	list_add_tail(&job->node, &queue->jobs.pending);
	spin_unlock(&queue->jobs.lock);

	pvr_context_pending_job_event(job->ctx);
}

/**
 * pvr_job_put() - Release reference on job
 * @job: Target job.
 */
void
pvr_job_put(struct pvr_job *job)
{
	if (job)
		kref_put(&job->ref_count, pvr_job_release);
}

/**
 * fence_array_add() - Adds the fence to an array of fences to be waited on,
 *                     deduplicating fences from the same context.
 * @fence_array: array of dma_fence * for the job to block on.
 * @fence: the dma_fence to add to the list of dependencies.
 *
 * This functions consumes the reference for @fence both on success and error
 * cases.
 *
 * Returns:
 *  * 0 on success, or an error on failing to expand the array.
 */
static int
fence_array_add(struct xarray *fence_array, struct dma_fence *fence)
{
	struct dma_fence *entry;
	unsigned long index;
	u32 id = 0;
	int ret;

	if (!fence)
		return 0;

	/* Deduplicate if we already depend on a fence from the same context.
	 * This lets the size of the array of deps scale with the number of
	 * engines involved, rather than the number of BOs.
	 */
	xa_for_each(fence_array, index, entry) {
		if (entry->context != fence->context)
			continue;

		if (dma_fence_is_later(fence, entry)) {
			dma_fence_put(entry);
			xa_store(fence_array, index, fence, GFP_KERNEL);
		} else {
			dma_fence_put(fence);
		}
		return 0;
	}

	ret = xa_alloc(fence_array, &id, fence, xa_limit_32b, GFP_KERNEL);
	if (ret != 0)
		dma_fence_put(fence);

	return ret;
}

static u32 *
get_syncobj_handles(u32 num_in_syncobj_handles, u64 in_syncobj_handles_p)
{
	const void __user *uptr = u64_to_user_ptr(in_syncobj_handles_p);
	u32 *in_syncobj_handles;
	int err;

	in_syncobj_handles = kcalloc(num_in_syncobj_handles, sizeof(*in_syncobj_handles),
				     GFP_KERNEL);
	if (!in_syncobj_handles) {
		err = -ENOMEM;
		goto err_out;
	}

	if (copy_from_user(in_syncobj_handles, uptr,
			   sizeof(*in_syncobj_handles) * num_in_syncobj_handles)) {
		err = -EFAULT;
		goto err_free_memory;
	}

	return in_syncobj_handles;

err_free_memory:
	kfree(in_syncobj_handles);

err_out:
	return ERR_PTR(err);
}

static int
pvr_job_add_deps(struct pvr_file *pvr_file, struct pvr_job *job,
		 u32 num_handles, u32 __user *user_handles)
{
	struct dma_fence *fence;
	unsigned long index;
	u32 *handles;
	int err = 0;

	if (!num_handles)
		return 0;

	handles = kvmalloc_array(num_handles, sizeof(*user_handles), GFP_KERNEL);
	if (!handles)
		return -ENOMEM;

	if (copy_from_user(handles, user_handles, sizeof(*handles) * num_handles)) {
		err = -EFAULT;
		goto out_free_handles;
	}

	for (u32 i = 0; i < num_handles; i++) {
		struct dma_fence *unwrapped_fence;
		struct dma_fence_unwrap iter;
		u32 native_fence_count = 0;

		err = drm_syncobj_find_fence(pvr_file->file, handles[i], 0, 0, &fence);
		if (err)
			goto out_free_handles;

		dma_fence_unwrap_for_each(unwrapped_fence, &iter, fence) {
			if (to_pvr_context_queue_fence(unwrapped_fence))
				native_fence_count++;
		}

		if (!native_fence_count) {
			/* No need to unwrap the fence if it's fully non-native. */
			err = fence_array_add(&job->deps.non_native, fence);
			if (err)
				goto out_free_handles;
		} else {
			dma_fence_unwrap_for_each(unwrapped_fence, &iter, fence) {
				/* There's no dma_fence_unwrap_stop() helper cleaning up the refs
				 * owned by dma_fence_unwrap(), so let's just iterate over all
				 * entries without doing anything when something failed.
				 */
				if (err)
					continue;

				dma_fence_get(unwrapped_fence);
				if (to_pvr_context_queue_fence(unwrapped_fence)) {
					err = fence_array_add(&job->deps.native, unwrapped_fence);
				} else {
					err = fence_array_add(&job->deps.non_native,
							      unwrapped_fence);
				}
				dma_fence_put(unwrapped_fence);
			}

			if (err)
				goto out_free_handles;
		}
	}

	xa_for_each(&job->deps.native, index, fence)
		job->deps.native_count++;

out_free_handles:
	kvfree(handles);
	return err;
}

static int pvr_fw_cmd_init(struct pvr_device *pvr_dev, struct pvr_job *job,
			   const struct pvr_stream_cmd_defs *stream_def,
			   u64 stream_userptr, u32 stream_len)
{
	void *stream;
	int err;

	stream = kzalloc(stream_len, GFP_KERNEL);
	if (!stream) {
		err = -ENOMEM;
		goto err_out;
	}

	if (copy_from_user(stream, u64_to_user_ptr(stream_userptr), stream_len)) {
		err = -EFAULT;
		goto err_free_stream;
	}

	err = pvr_stream_process(pvr_dev, stream_def, stream, stream_len, job);

err_free_stream:
	kfree(stream);

err_out:
	return err;
}

static u32
convert_geom_flags(u32 in_flags)
{
	u32 out_flags = 0;

	if (in_flags & DRM_PVR_SUBMIT_JOB_GEOM_CMD_FIRST)
		out_flags |= ROGUE_GEOM_FLAGS_FIRSTKICK;
	if (in_flags & DRM_PVR_SUBMIT_JOB_GEOM_CMD_LAST)
		out_flags |= ROGUE_GEOM_FLAGS_LASTKICK;
	if (in_flags & DRM_PVR_SUBMIT_JOB_GEOM_CMD_SINGLE_CORE)
		out_flags |= ROGUE_GEOM_FLAGS_SINGLE_CORE;

	return out_flags;
}

static u32
convert_frag_flags(u32 in_flags)
{
	u32 out_flags = 0;

	if (in_flags & DRM_PVR_SUBMIT_JOB_FRAG_CMD_SINGLE_CORE)
		out_flags |= ROGUE_FRAG_FLAGS_SINGLE_CORE;
	if (in_flags & DRM_PVR_SUBMIT_JOB_FRAG_CMD_DEPTHBUFFER)
		out_flags |= ROGUE_FRAG_FLAGS_DEPTHBUFFER;
	if (in_flags & DRM_PVR_SUBMIT_JOB_FRAG_CMD_STENCILBUFFER)
		out_flags |= ROGUE_FRAG_FLAGS_STENCILBUFFER;
	if (in_flags & DRM_PVR_SUBMIT_JOB_FRAG_CMD_PREVENT_CDM_OVERLAP)
		out_flags |= ROGUE_FRAG_FLAGS_PREVENT_CDM_OVERLAP;
	if (in_flags & DRM_PVR_SUBMIT_JOB_FRAG_CMD_GET_VIS_RESULTS)
		out_flags |= ROGUE_FRAG_FLAGS_GET_VIS_RESULTS;

	return out_flags;
}

static struct pvr_job *
pvr_create_geom_job(struct pvr_device *pvr_dev,
		    struct pvr_file *pvr_file,
		    struct pvr_hwrt_data *hwrt,
		    struct pvr_context_render *ctx_render,
		    struct drm_pvr_ioctl_submit_job_args *args,
		    struct drm_pvr_job_render_args *render_args)
{
	struct rogue_fwif_cmd_geom *cmd_geom;
	struct pvr_context_queue *queue;
	struct pvr_job *job;
	int err;

	job = pvr_create_job(pvr_dev, PVR_JOB_TYPE_GEOMETRY);
	if (IS_ERR(job))
		return job;

	err = pvr_fw_cmd_init(pvr_dev, job, &pvr_cmd_geom_stream, render_args->geom_cmd_stream,
			      render_args->geom_cmd_stream_len);
	if (err)
		goto err_put_job;

	cmd_geom = job->cmd;
	cmd_geom->cmd_shared.cmn.frame_num = 0;
	cmd_geom->flags = convert_geom_flags(render_args->geom_flags);
	pvr_gem_get_fw_addr(hwrt->fw_obj, &cmd_geom->cmd_shared.hwrt_data_fw_addr);

	job->hwrt = pvr_hwrt_data_get(hwrt);
	job->ctx = pvr_context_get(from_pvr_context_render(ctx_render));

	/* Check if the job will ever fit in the CCCB. */
	err = pvr_job_fits_in_cccb(job);
	if (err == -E2BIG)
		goto err_put_job;

	err = pvr_job_add_deps(pvr_file, job, args->num_in_syncobj_handles,
			       u64_to_user_ptr(args->in_syncobj_handles));
	if (err)
		goto err_put_job;

	queue = get_ctx_queue(job);
	if (!queue) {
		err = -EINVAL;
		goto err_put_job;
	}

	job->done_fence = pvr_context_queue_fence_create(queue);
	if (IS_ERR(job->done_fence)) {
		err = PTR_ERR(job->done_fence);
		job->done_fence = NULL;
		goto err_put_job;
	}

	return job;

err_put_job:
	pvr_job_put(job);
	return ERR_PTR(err);
}

static struct pvr_job *
pvr_create_frag_job(struct pvr_device *pvr_dev,
		    struct pvr_file *pvr_file,
		    struct pvr_hwrt_data *hwrt,
		    struct pvr_context_render *ctx_render,
		    struct drm_pvr_ioctl_submit_job_args *args,
		    struct drm_pvr_job_render_args *render_args,
		    struct dma_fence *geom_done_fence)
{
	struct rogue_fwif_cmd_frag *cmd_frag;
	struct pvr_context_queue *queue;
	struct pvr_job *job;
	int err;

	job = pvr_create_job(pvr_dev, PVR_JOB_TYPE_FRAGMENT);
	if (IS_ERR(job))
		return job;

	err = pvr_fw_cmd_init(pvr_dev, job, &pvr_cmd_frag_stream, render_args->frag_cmd_stream,
			      render_args->frag_cmd_stream_len);
	if (err)
		goto err_put_job;

	cmd_frag = job->cmd;
	cmd_frag->cmd_shared.cmn.frame_num = 0;
	cmd_frag->flags = convert_frag_flags(render_args->frag_flags);
	pvr_gem_get_fw_addr(hwrt->fw_obj, &cmd_frag->cmd_shared.hwrt_data_fw_addr);

	job->hwrt = pvr_hwrt_data_get(hwrt);
	job->ctx = pvr_context_get(from_pvr_context_render(ctx_render));

	/* Check if the job will ever fit in the CCCB. */
	err = pvr_job_fits_in_cccb(job);
	if (err == -E2BIG)
		goto err_put_job;

	err = pvr_job_add_deps(pvr_file, job, render_args->num_in_syncobj_handles_frag,
			       u64_to_user_ptr(render_args->in_syncobj_handles_frag));
	if (err)
		goto err_put_job;

	/* Add the geometry job done_fence to the native fence array. */
	if (geom_done_fence &&
	    !WARN_ON(!pvr_context_queue_fence_ctx_from_fence(geom_done_fence))) {
		err = fence_array_add(&job->deps.native, dma_fence_get(geom_done_fence));
	}

	queue = get_ctx_queue(job);
	if (!queue) {
		err = -EINVAL;
		goto err_put_job;
	}

	job->done_fence = pvr_context_queue_fence_create(queue);
	if (IS_ERR(job->done_fence)) {
		err = PTR_ERR(job->done_fence);
		job->done_fence = NULL;
		goto err_put_job;
	}

	return job;

err_put_job:
	pvr_job_put(job);
	return ERR_PTR(err);
}

static int
pvr_process_job_render(struct pvr_device *pvr_dev,
		       struct pvr_file *pvr_file,
		       struct drm_pvr_ioctl_submit_job_args *args,
		       struct drm_pvr_job_render_args *render_args)
{
	struct drm_syncobj *geom_out_syncobj = NULL, *frag_out_syncobj = NULL;
	struct pvr_job *geom_job = NULL, *frag_job = NULL;
	struct pvr_context_render *ctx_render;
	struct pvr_hwrt_data *hwrt = NULL;
	struct pvr_context *ctx = NULL;
	int err;

	if (render_args->_padding_54)
		return -EINVAL;

	/* Verify that at least one command is provided. */
	if (!render_args->geom_cmd_stream && !render_args->frag_cmd_stream)
		return -EINVAL;

	if ((render_args->geom_flags & ~DRM_PVR_SUBMIT_JOB_GEOM_CMD_FLAGS_MASK) ||
	    (render_args->frag_flags & ~DRM_PVR_SUBMIT_JOB_FRAG_CMD_FLAGS_MASK))
		return -EINVAL;

	hwrt = pvr_hwrt_data_lookup(pvr_file, render_args->hwrt_data_set_handle,
				    render_args->hwrt_data_index);
	if (!hwrt)
		return -EINVAL;

	ctx = pvr_context_lookup(pvr_file, args->context_handle);
	if (!ctx) {
		err = -EINVAL;
		goto out;
	}

	/* to_pvr_context_render() will validate the context type. */
	ctx_render = to_pvr_context_render(ctx);
	if (!ctx_render) {
		err = -EINVAL;
		goto out;
	}

	if (render_args->out_syncobj_geom) {
		geom_out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
						    render_args->out_syncobj_geom);
		if (!geom_out_syncobj) {
			err = -ENOENT;
			goto out;
		}
	}

	if (render_args->out_syncobj_frag) {
		frag_out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
						    render_args->out_syncobj_frag);
		if (!geom_out_syncobj) {
			err = -ENOENT;
			goto out;
		}
	}

	if (render_args->geom_cmd_stream) {
		geom_job = pvr_create_geom_job(pvr_dev, pvr_file, hwrt, ctx_render, args,
					       render_args);
		if (IS_ERR(geom_job)) {
			err = PTR_ERR(geom_job);
			geom_job = NULL;
			goto out;
		}
	}

	if (render_args->frag_cmd_stream) {
		frag_job = pvr_create_frag_job(pvr_dev, pvr_file, hwrt, ctx_render, args,
					       render_args,
					       geom_job ? geom_job->done_fence : NULL);
		if (IS_ERR(frag_job)) {
			err = PTR_ERR(frag_job);
			frag_job = NULL;
			goto out;
		}
	}

	if (geom_job) {
		if (geom_out_syncobj)
			drm_syncobj_replace_fence(geom_out_syncobj, geom_job->done_fence);

		pvr_job_push(geom_job);
	}

	if (frag_job) {
		if (frag_out_syncobj)
			drm_syncobj_replace_fence(frag_out_syncobj, frag_job->done_fence);

		pvr_job_push(frag_job);
	}

	err = 0;

out:
	if (frag_out_syncobj)
		drm_syncobj_put(frag_out_syncobj);

	if (geom_out_syncobj)
		drm_syncobj_put(geom_out_syncobj);

	pvr_job_put(geom_job);
	pvr_job_put(frag_job);
	pvr_context_put(ctx);
	pvr_hwrt_data_put(hwrt);
	return err;
}

static u32
convert_compute_flags(u32 in_flags)
{
	u32 out_flags = 0;

	if (in_flags & DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP)
		out_flags |= ROGUE_COMPUTE_FLAG_PREVENT_ALL_OVERLAP;
	if (in_flags & DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE)
		out_flags |= ROGUE_COMPUTE_FLAG_SINGLE_CORE;

	return out_flags;
}

static int
pvr_process_job_compute(struct pvr_device *pvr_dev,
			struct pvr_file *pvr_file,
			struct drm_pvr_ioctl_submit_job_args *args,
			struct drm_pvr_job_compute_args *compute_args)
{
	struct rogue_fwif_cmd_compute *cmd_compute;
	struct pvr_context_compute *ctx_compute;
	struct drm_syncobj *out_syncobj = NULL;
	struct pvr_context_queue *queue;
	struct pvr_job *job;
	int err;

	if (compute_args->flags & ~DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_FLAGS_MASK)
		return -EINVAL;

	/* Copy commands from userspace. */
	if (!compute_args->cmd_stream)
		return -EINVAL;

	job = pvr_create_job(pvr_dev, PVR_JOB_TYPE_COMPUTE);
	if (IS_ERR(job))
		return PTR_ERR(job);

	err = pvr_fw_cmd_init(pvr_dev, job, &pvr_cmd_compute_stream, compute_args->cmd_stream,
			      compute_args->cmd_stream_len);
	if (err)
		goto out;

	cmd_compute = job->cmd;
	cmd_compute->common.frame_num = 0;
	cmd_compute->flags = convert_compute_flags(compute_args->flags);

	job->ctx = pvr_context_lookup(pvr_file, args->context_handle);
	if (!job->ctx) {
		err = -EINVAL;
		goto out;
	}

	/* to_pvr_context_compute() will validate the context type. */
	ctx_compute = to_pvr_context_compute(job->ctx);
	if (!ctx_compute) {
		err = -EINVAL;
		goto out;
	}

	if (compute_args->out_syncobj) {
		out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
					       compute_args->out_syncobj);
		if (!out_syncobj) {
			err = -ENOENT;
			goto out;
		}
	}

	/* Check if the job will ever fit in the CCCB. */
	err = pvr_job_fits_in_cccb(job);
	if (err == -E2BIG)
		goto out;

	err = pvr_job_add_deps(pvr_file, job, args->num_in_syncobj_handles,
			       u64_to_user_ptr(args->in_syncobj_handles));
	if (err)
		goto out;

	queue = get_ctx_queue(job);
	if (!queue) {
		err = -EINVAL;
		goto out;
	}

	job->done_fence = pvr_context_queue_fence_create(queue);
	if (IS_ERR(job->done_fence)) {
		err = PTR_ERR(job->done_fence);
		job->done_fence = NULL;
		goto out;
	}

	if (out_syncobj)
		drm_syncobj_replace_fence(out_syncobj, job->done_fence);

	pvr_job_push(job);
	err = 0;

out:
	if (out_syncobj)
		drm_syncobj_put(out_syncobj);

	pvr_job_put(job);
	return err;
}

static u32
convert_transfer_flags(u32 in_flags)
{
	u32 out_flags = 0;

	if (in_flags & DRM_PVR_SUBMIT_JOB_TRANSFER_CMD_SINGLE_CORE)
		out_flags |= ROGUE_TRANSFER_FLAGS_SINGLE_CORE;

	return out_flags;
}

static int
pvr_process_job_transfer(struct pvr_device *pvr_dev,
			 struct pvr_file *pvr_file,
			 struct drm_pvr_ioctl_submit_job_args *args,
			 struct drm_pvr_job_transfer_args *transfer_args)
{
	struct rogue_fwif_cmd_transfer *cmd_transfer;
	struct pvr_context_transfer *ctx_transfer;
	struct drm_syncobj *out_syncobj = NULL;
	struct pvr_context_queue *queue;
	struct pvr_job *job;
	int err;

	if (transfer_args->flags & ~DRM_PVR_SUBMIT_JOB_TRANSFER_CMD_FLAGS_MASK)
		return -EINVAL;

	/* Copy commands from userspace. */
	if (!transfer_args->cmd_stream)
		return -EINVAL;

	job = pvr_create_job(pvr_dev, PVR_JOB_TYPE_TRANSFER);
	if (IS_ERR(job))
		return PTR_ERR(job);

	err = pvr_fw_cmd_init(pvr_dev, job, &pvr_cmd_transfer_stream, transfer_args->cmd_stream,
			      transfer_args->cmd_stream_len);
	if (err)
		goto out;

	cmd_transfer = job->cmd;
	cmd_transfer->common.frame_num = 0;
	cmd_transfer->flags = convert_transfer_flags(transfer_args->flags);

	job->ctx = pvr_context_lookup(pvr_file, args->context_handle);
	if (!job->ctx) {
		err = -EINVAL;
		goto out;
	}

	ctx_transfer = to_pvr_context_transfer_frag(job->ctx);
	if (!ctx_transfer) {
		err = -EINVAL;
		goto out;
	}

	if (transfer_args->out_syncobj) {
		out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
					       transfer_args->out_syncobj);
		if (!out_syncobj) {
			err = -ENOENT;
			goto out;
		}
	}

	/* Check if the job will ever fit in the CCCB. */
	err = pvr_job_fits_in_cccb(job);
	if (err == -E2BIG)
		goto out;

	err = pvr_job_add_deps(pvr_file, job, args->num_in_syncobj_handles,
			       u64_to_user_ptr(args->in_syncobj_handles));
	if (err)
		goto out;

	queue = get_ctx_queue(job);
	if (!queue) {
		err = -EINVAL;
		goto out;
	}

	job->done_fence = pvr_context_queue_fence_create(queue);
	if (IS_ERR(job->done_fence)) {
		err = PTR_ERR(job->done_fence);
		job->done_fence = NULL;
		goto out;
	}

	if (out_syncobj)
		drm_syncobj_replace_fence(out_syncobj, job->done_fence);

	pvr_job_push(job);
	err = 0;

out:
	if (out_syncobj)
		drm_syncobj_put(out_syncobj);

	pvr_job_put(job);
	return err;
}

static int
pvr_process_job_null(struct pvr_device *pvr_dev,
		     struct pvr_file *pvr_file,
		     struct drm_pvr_ioctl_submit_job_args *args,
		     struct drm_pvr_job_null_args *null_args)
{
	struct drm_syncobj *out_syncobj;
	struct xarray in_fences;
	u32 num_in_fences = 0;
	u32 *syncobj_handles = NULL;
	struct dma_fence_array *fence_array;
	struct dma_fence **in_fence_array;
	struct dma_fence *fence;
	unsigned long id;
	u32 array_idx = 0;
	int err;
	u32 i;

	if (null_args->flags & ~DRM_PVR_SUBMIT_JOB_NULL_CMD_FLAGS_MASK ||
	    !null_args->out_syncobj || null_args->_padding_14 || args->context_handle) {
		err = -EINVAL;
		goto err_out;
	}

	out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
				       null_args->out_syncobj);
	if (!out_syncobj) {
		err = -ENOENT;
		goto err_out;
	}

	if (args->num_in_syncobj_handles) {
		syncobj_handles = get_syncobj_handles(args->num_in_syncobj_handles,
						      args->in_syncobj_handles);

		if (IS_ERR(syncobj_handles)) {
			err = PTR_ERR(syncobj_handles);
			goto err_put_syncobj;
		}
	}

	xa_init_flags(&in_fences, XA_FLAGS_ALLOC);

	for (i = 0; i < args->num_in_syncobj_handles; i++) {
		err = drm_syncobj_find_fence(from_pvr_file(pvr_file),
					     syncobj_handles[i], 0, 0, &fence);
		if (err)
			goto err_release_in_fences;

		err = fence_array_add(&in_fences, fence);
		if (err)
			goto err_release_in_fences;
	}

	xa_for_each(&in_fences, id, fence) {
		struct dma_fence *unwrapped_fence;
		struct dma_fence_unwrap iter;

		dma_fence_unwrap_for_each(unwrapped_fence, &iter, fence)
			num_in_fences++;
	}

	if (!num_in_fences) {
		/* No input fences, just assign a stub fence. */
		fence = dma_fence_allocate_private_stub();

		if (IS_ERR(fence)) {
			err = PTR_ERR(fence);
			goto err_release_in_fences;
		}

		drm_syncobj_replace_fence(out_syncobj, fence);
		drm_syncobj_put(out_syncobj);

		dma_fence_put(fence);

		return 0;
	}

	in_fence_array = kcalloc(num_in_fences, sizeof(*in_fence_array), GFP_KERNEL);
	if (!in_fence_array) {
		err = -ENOMEM;
		goto err_release_in_fences;
	}

	xa_for_each(&in_fences, id, fence) {
		struct dma_fence *unwrapped_fence;
		struct dma_fence_unwrap iter;

		dma_fence_unwrap_for_each(unwrapped_fence, &iter, fence) {
			dma_fence_get(unwrapped_fence);
			in_fence_array[array_idx++] = unwrapped_fence;
		}
	}

	fence_array = dma_fence_array_create(array_idx, in_fence_array,
					     dma_fence_context_alloc(1), 1,
					     false);
	if (!fence_array) {
		err = -ENOMEM;
		goto err_free_in_fence_array;
	}

	/* dma_fence_array now owns in_fence_array[] and the fence references within it. */

	drm_syncobj_replace_fence(out_syncobj, &fence_array->base);
	drm_syncobj_put(out_syncobj);

	dma_fence_put(&fence_array->base);
	release_fences(&in_fences);

	return 0;

err_free_in_fence_array:
	kfree(in_fence_array);

err_release_in_fences:
	release_fences(&in_fences);

	kfree(syncobj_handles);

err_put_syncobj:
	drm_syncobj_put(out_syncobj);

err_out:
	return err;
}

/**
 * pvr_submit_job() - Submit a job to the GPU
 * @pvr_dev: Target PowerVR device.
 * @pvr_file: Pointer to PowerVR file structure.
 * @args: IOCTL arguments.
 *
 * This initial implementation is entirely synchronous; on return the GPU will
 * be idle. This will not be the case for future implementations.
 *
 * Returns:
 *  * 0 on success,
 *  * -%EFAULT if arguments can not be copied from user space,
 *  * -%EINVAL on invalid arguments, or
 *  * Any other error.
 */
int
pvr_submit_job(struct pvr_device *pvr_dev,
	       struct pvr_file *pvr_file,
	       struct drm_pvr_ioctl_submit_job_args *args)
{
	int err;

	/* Process arguments based on job type */
	switch (args->job_type) {
	case DRM_PVR_JOB_TYPE_RENDER: {
		struct drm_pvr_job_render_args render_args;

		if (copy_from_user(&render_args, u64_to_user_ptr(args->data),
				   sizeof(render_args))) {
			err = -EFAULT;
			goto err_out;
		}

		err = pvr_process_job_render(pvr_dev, pvr_file, args, &render_args);
		break;
	}

	case DRM_PVR_JOB_TYPE_COMPUTE: {
		struct drm_pvr_job_compute_args compute_args;

		if (copy_from_user(&compute_args, u64_to_user_ptr(args->data),
				   sizeof(compute_args))) {
			err = -EFAULT;
			goto err_out;
		}

		err = pvr_process_job_compute(pvr_dev, pvr_file, args, &compute_args);
		break;
	}

	case DRM_PVR_JOB_TYPE_TRANSFER_FRAG: {
		struct drm_pvr_job_transfer_args transfer_args;

		if (copy_from_user(&transfer_args, u64_to_user_ptr(args->data),
				   sizeof(transfer_args))) {
			err = -EFAULT;
			goto err_out;
		}

		err = pvr_process_job_transfer(pvr_dev, pvr_file, args, &transfer_args);
		break;
	}

	case DRM_PVR_JOB_TYPE_NULL: {
		struct drm_pvr_job_null_args null_args;

		if (copy_from_user(&null_args, u64_to_user_ptr(args->data),
				   sizeof(null_args))) {
			err = -EFAULT;
			goto err_out;
		}

		err = pvr_process_job_null(pvr_dev, pvr_file, args, &null_args);
		break;
	}

	default:
		err = -EINVAL;
		break;
	}

err_out:
	return err;
}
