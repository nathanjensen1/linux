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
import_fences(struct pvr_file *pvr_file, u32 *in_syncobj_handles, u32 num_in_syncobj_handles,
	      struct xarray *in_fences, struct pvr_fence_context *context)
{
	struct dma_fence *fence;
	unsigned long id;
	int err;
	int i;

	for (i = 0; i < num_in_syncobj_handles; i++) {
		struct dma_fence *pvr_fence;

		err = drm_syncobj_find_fence(from_pvr_file(pvr_file),
					     in_syncobj_handles[i], 0, 0, &fence);
		if (err)
			goto err_release_fences;

		pvr_fence = pvr_fence_import(context, fence);
		if (IS_ERR(pvr_fence)) {
			err = PTR_ERR(pvr_fence);
			dma_fence_put(fence);
			goto err_release_fences;
		}

		err = fence_array_add(in_fences, pvr_fence);
		if (err)
			goto err_release_fences;
	}

	return 0;

err_release_fences:
	xa_for_each(in_fences, id, fence) {
		pvr_fence_deactivate_and_put(fence);
		dma_fence_put(fence);
	}
	xa_destroy(in_fences);

	return err;
}

static void release_fences(struct xarray *in_fences, bool deactivate_fences)
{
	struct dma_fence *fence;
	unsigned long id;

	xa_for_each(in_fences, id, fence) {
		if (deactivate_fences)
			pvr_fence_deactivate_and_put(fence);
		dma_fence_put(fence);
	}
	xa_destroy(in_fences);
}

static int
submit_cmd_geometry(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
		    struct pvr_context_render *ctx_render,
		    struct drm_pvr_ioctl_submit_job_args *args,
		    struct drm_pvr_job_render_args *render_args, struct pvr_hwrt_data *hwrt,
		    struct rogue_fwif_cmd_geom *cmd_geom, u32 *syncobj_handles,
		    struct dma_fence *out_fence)
{
	struct rogue_fwif_cmd_geom_frag_shared *cmd_shared = &cmd_geom->cmd_shared;
	struct pvr_context_geom *ctx_geom = &ctx_render->ctx_geom;
	u32 num_in_fences = args->num_in_syncobj_handles;
	struct drm_syncobj *out_syncobj;
	struct rogue_fwif_ufo *in_ufos;
	struct rogue_fwif_ufo out_ufo;
	struct xarray in_fences;
	u32 ctx_fw_addr;
	u32 ufo_nr = 0;
	int err;

	pvr_gem_get_fw_addr(hwrt->fw_obj, &cmd_shared->hwrt_data_fw_addr);

	pvr_gem_get_fw_addr(ctx_render->fw_obj, &ctx_fw_addr);

	err = pvr_fence_to_ufo(out_fence, &out_ufo);
	if (err)
		goto err_out;

	if (render_args->out_syncobj_geom) {
		out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
					       render_args->out_syncobj_geom);
		if (!out_syncobj) {
			err = -ENOENT;
			goto err_out;
		}
	}

	xa_init_flags(&in_fences, XA_FLAGS_ALLOC);

	if (num_in_fences) {
		struct dma_fence *fence;
		unsigned long id;

		err = import_fences(pvr_file, syncobj_handles, num_in_fences,
				    &in_fences, &ctx_geom->cccb.pvr_fence_context);
		if (err)
			goto err_put_out_syncobj;

		in_ufos = kcalloc(num_in_fences, sizeof(*in_ufos), GFP_KERNEL);
		if (!in_ufos) {
			err = -ENOMEM;
			goto err_release_fences;
		}

		xa_for_each(&in_fences, id, fence) {
			pvr_fence_add_fence_dependency(out_fence, fence);
			err = pvr_fence_to_ufo(fence, &in_ufos[ufo_nr]);
			if (err)
				goto err_kfree_in_ufos;

			ufo_nr++;
		}
	}

	pvr_cccb_lock(&ctx_geom->cccb);

	if (ufo_nr) {
		err = pvr_cccb_write_command_with_header(&ctx_geom->cccb,
							 ROGUE_FWIF_CCB_CMD_TYPE_FENCE,
							 ufo_nr * sizeof(*in_ufos), in_ufos, 0, 0);
		if (err)
			goto err_cccb_unlock_rollback;
	}

	/* Submit job to FW */
	err = pvr_cccb_write_command_with_header(&ctx_geom->cccb, ROGUE_FWIF_CCB_CMD_TYPE_GEOM,
						 sizeof(*cmd_geom), cmd_geom, 0, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_write_command_with_header(&ctx_geom->cccb, ROGUE_FWIF_CCB_CMD_TYPE_UPDATE,
						 sizeof(out_ufo), &out_ufo, 0, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_unlock_send_kccb_kick(pvr_dev, &ctx_geom->cccb, ctx_fw_addr, hwrt);
	if (err)
		goto err_cccb_unlock_rollback;

	/* Signal completion of geometry job */
	if (render_args->out_syncobj_geom) {
		drm_syncobj_replace_fence(out_syncobj, out_fence);
		drm_syncobj_put(out_syncobj);
	}

	release_fences(&in_fences, false);

	if (num_in_fences)
		kfree(in_ufos);

	return 0;

err_cccb_unlock_rollback:
	pvr_cccb_unlock_rollback(&ctx_geom->cccb);

err_kfree_in_ufos:
	if (num_in_fences)
		kfree(in_ufos);

err_release_fences:
	release_fences(&in_fences, true);

err_put_out_syncobj:
	if (render_args->out_syncobj_geom)
		drm_syncobj_put(out_syncobj);

err_out:
	return err;
}

static int
submit_cmd_fragment(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
		    struct pvr_context_render *ctx_render,
		    struct drm_pvr_ioctl_submit_job_args *args,
		    struct drm_pvr_job_render_args *render_args, struct pvr_hwrt_data *hwrt,
		    struct rogue_fwif_cmd_frag *cmd_frag, u32 *syncobj_handles,
		    struct dma_fence *geom_in_fence, struct dma_fence *out_fence)
{
	struct rogue_fwif_cmd_geom_frag_shared *cmd_shared = &cmd_frag->cmd_shared;
	u32 num_in_fences = render_args->num_in_syncobj_handles_frag;
	struct pvr_context_frag *ctx_frag = &ctx_render->ctx_frag;
	struct drm_syncobj *out_syncobj;
	struct rogue_fwif_ufo *in_ufos;
	struct rogue_fwif_ufo out_ufo;
	struct xarray in_fences;
	u32 ctx_fw_addr;
	u32 ufo_nr = 0;
	int err;

	pvr_gem_get_fw_addr(hwrt->fw_obj, &cmd_shared->hwrt_data_fw_addr);

	pvr_gem_get_fw_addr(ctx_render->fw_obj, &ctx_fw_addr);
	ctx_fw_addr += offsetof(struct rogue_fwif_fwrendercontext, frag_context);

	err = pvr_fence_to_ufo(out_fence, &out_ufo);
	if (err)
		goto err_out;

	if (geom_in_fence) {
		/*
		 * Add dependency on geometry fence to the out fence, to ensure the former doesn't
		 * get freed while it's still being waited on.
		 */
		pvr_fence_add_fence_dependency(out_fence, geom_in_fence);
	}

	if (render_args->out_syncobj_frag) {
		out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
					       render_args->out_syncobj_frag);
		if (!out_syncobj) {
			err = -ENOENT;
			goto err_out;
		}
	}

	xa_init_flags(&in_fences, XA_FLAGS_ALLOC);

	if (num_in_fences) {
		struct dma_fence *fence;
		unsigned long id;

		err = import_fences(pvr_file, syncobj_handles, num_in_fences,
				    &in_fences, &ctx_frag->cccb.pvr_fence_context);
		if (err)
			goto err_put_out_syncobj;

		in_ufos = kcalloc(num_in_fences, sizeof(*in_ufos), GFP_KERNEL);
		if (!in_ufos) {
			err = -ENOMEM;
			goto err_release_fences;
		}

		xa_for_each(&in_fences, id, fence) {
			pvr_fence_add_fence_dependency(out_fence, fence);

			err = pvr_fence_to_ufo(fence, &in_ufos[ufo_nr]);
			if (err)
				goto err_kfree_in_ufos;

			ufo_nr++;
		}
	}

	pvr_cccb_lock(&ctx_frag->cccb);

	if (ufo_nr) {
		err = pvr_cccb_write_command_with_header(&ctx_frag->cccb,
							 ROGUE_FWIF_CCB_CMD_TYPE_FENCE,
							 ufo_nr * sizeof(*in_ufos), in_ufos, 0, 0);
		if (err)
			goto err_cccb_unlock_rollback;
	}

	if (geom_in_fence) {
		struct rogue_fwif_ufo geom_in_ufo;

		err = pvr_fence_to_ufo(geom_in_fence, &geom_in_ufo);
		if (err)
			goto err_cccb_unlock_rollback;

		err = pvr_cccb_write_command_with_header(&ctx_frag->cccb,
							 ROGUE_FWIF_CCB_CMD_TYPE_FENCE,
							 sizeof(geom_in_ufo), &geom_in_ufo, 0, 0);
		if (err)
			goto err_cccb_unlock_rollback;
	}

	/* Submit job to FW */
	err = pvr_cccb_write_command_with_header(&ctx_frag->cccb, ROGUE_FWIF_CCB_CMD_TYPE_FRAG,
						 sizeof(*cmd_frag), cmd_frag, 0, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_write_command_with_header(&ctx_frag->cccb, ROGUE_FWIF_CCB_CMD_TYPE_UPDATE,
						 sizeof(out_ufo), &out_ufo, 0, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_unlock_send_kccb_kick(pvr_dev, &ctx_frag->cccb, ctx_fw_addr, hwrt);
	if (err)
		goto err_cccb_unlock_rollback;

	/* Signal completion of fragment job */
	if (render_args->out_syncobj_frag) {
		drm_syncobj_replace_fence(out_syncobj, out_fence);
		drm_syncobj_put(out_syncobj);
	}

	release_fences(&in_fences, false);

	if (num_in_fences)
		kfree(in_ufos);

	return 0;

err_cccb_unlock_rollback:
	pvr_cccb_unlock_rollback(&ctx_frag->cccb);

err_kfree_in_ufos:
	if (num_in_fences)
		kfree(in_ufos);

err_release_fences:
	release_fences(&in_fences, true);

err_put_out_syncobj:
	if (render_args->out_syncobj_frag)
		drm_syncobj_put(out_syncobj);

err_out:
	return err;
}

static int
submit_cmd_compute(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
		   struct pvr_context_compute *ctx_compute,
		   struct drm_pvr_ioctl_submit_job_args *args,
		   struct drm_pvr_job_compute_args *compute_args,
		   struct rogue_fwif_cmd_compute *cmd_compute, u32 *syncobj_handles,
		   struct dma_fence *out_fence)
{
	u32 num_in_fences = args->num_in_syncobj_handles;
	struct drm_syncobj *out_syncobj;
	struct rogue_fwif_ufo *in_ufos;
	struct rogue_fwif_ufo out_ufo;
	struct xarray in_fences;
	u32 ctx_fw_addr;
	u32 ufo_nr = 0;
	int err;

	pvr_gem_get_fw_addr(ctx_compute->fw_obj, &ctx_fw_addr);

	err = pvr_fence_to_ufo(out_fence, &out_ufo);
	if (err)
		goto err_out;

	if (compute_args->out_syncobj) {
		out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
					       compute_args->out_syncobj);
		if (!out_syncobj) {
			err = -ENOENT;
			goto err_out;
		}
	}

	xa_init_flags(&in_fences, XA_FLAGS_ALLOC);

	if (num_in_fences) {
		struct dma_fence *fence;
		unsigned long id;

		err = import_fences(pvr_file, syncobj_handles, num_in_fences,
				    &in_fences, &ctx_compute->cccb.pvr_fence_context);
		if (err)
			goto err_put_out_syncobj;

		in_ufos = kcalloc(num_in_fences, sizeof(*in_ufos), GFP_KERNEL);
		if (!in_ufos) {
			err = -ENOMEM;
			goto err_release_fences;
		}

		xa_for_each(&in_fences, id, fence) {
			pvr_fence_add_fence_dependency(out_fence, fence);

			err = pvr_fence_to_ufo(fence, &in_ufos[ufo_nr]);
			if (err)
				goto err_kfree_in_ufos;

			ufo_nr++;
		}
	}

	pvr_cccb_lock(&ctx_compute->cccb);

	if (num_in_fences) {
		err = pvr_cccb_write_command_with_header(&ctx_compute->cccb,
							 ROGUE_FWIF_CCB_CMD_TYPE_FENCE,
							 ufo_nr * sizeof(*in_ufos), in_ufos, 0, 0);
		if (err)
			goto err_cccb_unlock_rollback;
	}

	/* Submit job to FW */
	err = pvr_cccb_write_command_with_header(&ctx_compute->cccb, ROGUE_FWIF_CCB_CMD_TYPE_CDM,
						 sizeof(*cmd_compute), cmd_compute, 0, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_write_command_with_header(&ctx_compute->cccb, ROGUE_FWIF_CCB_CMD_TYPE_UPDATE,
						 sizeof(out_ufo), &out_ufo, 0, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_unlock_send_kccb_kick(pvr_dev, &ctx_compute->cccb, ctx_fw_addr, NULL);
	if (err)
		goto err_cccb_unlock_rollback;

	/* Signal completion of compute job */
	if (compute_args->out_syncobj) {
		drm_syncobj_replace_fence(out_syncobj, out_fence);
		drm_syncobj_put(out_syncobj);
	}

	release_fences(&in_fences, false);

	if (num_in_fences)
		kfree(in_ufos);

	return 0;

err_cccb_unlock_rollback:
	pvr_cccb_unlock_rollback(&ctx_compute->cccb);

err_kfree_in_ufos:
	if (num_in_fences)
		kfree(in_ufos);

err_release_fences:
	release_fences(&in_fences, true);

err_put_out_syncobj:
	if (compute_args->out_syncobj)
		drm_syncobj_put(out_syncobj);

err_out:
	return err;
}

static int
submit_cmd_transfer(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
		    struct pvr_context_transfer *ctx_transfer,
		    struct drm_pvr_ioctl_submit_job_args *args,
		    struct drm_pvr_job_transfer_args *transfer_args,
		    struct rogue_fwif_cmd_transfer *cmd_transfer, u32 *syncobj_handles,
		    struct dma_fence *out_fence)
{
	u32 num_in_fences = args->num_in_syncobj_handles;
	struct drm_syncobj *out_syncobj;
	struct rogue_fwif_ufo *in_ufos;
	struct rogue_fwif_ufo out_ufo;
	struct xarray in_fences;
	u32 ctx_fw_addr;
	u32 ufo_nr = 0;
	int err;

	pvr_gem_get_fw_addr(ctx_transfer->fw_obj, &ctx_fw_addr);

	err = pvr_fence_to_ufo(out_fence, &out_ufo);
	if (err)
		goto err_out;

	if (transfer_args->out_syncobj) {
		out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
					       transfer_args->out_syncobj);
		if (!out_syncobj) {
			err = -ENOENT;
			goto err_out;
		}
	}

	xa_init_flags(&in_fences, XA_FLAGS_ALLOC);

	if (num_in_fences) {
		struct dma_fence *fence;
		unsigned long id;

		err = import_fences(pvr_file, syncobj_handles, num_in_fences,
				    &in_fences, &ctx_transfer->cccb.pvr_fence_context);
		if (err)
			goto err_put_out_syncobj;

		in_ufos = kcalloc(num_in_fences, sizeof(*in_ufos), GFP_KERNEL);
		if (!in_ufos) {
			err = -ENOMEM;
			goto err_release_fences;
		}

		xa_for_each(&in_fences, id, fence) {
			pvr_fence_add_fence_dependency(out_fence, fence);

			err = pvr_fence_to_ufo(fence, &in_ufos[ufo_nr]);
			if (err)
				goto err_kfree_in_ufos;

			ufo_nr++;
		}
	}

	pvr_cccb_lock(&ctx_transfer->cccb);

	if (num_in_fences) {
		err = pvr_cccb_write_command_with_header(&ctx_transfer->cccb,
							 ROGUE_FWIF_CCB_CMD_TYPE_FENCE,
							 ufo_nr * sizeof(*in_ufos), in_ufos, 0, 0);
		if (err)
			goto err_cccb_unlock_rollback;
	}

	/* Submit job to FW */
	err = pvr_cccb_write_command_with_header(&ctx_transfer->cccb, ROGUE_FWIF_CCB_CMD_TYPE_TQ_3D,
						 sizeof(*cmd_transfer), cmd_transfer, 0, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_write_command_with_header(&ctx_transfer->cccb,
						 ROGUE_FWIF_CCB_CMD_TYPE_UPDATE,
						 sizeof(out_ufo), &out_ufo, 0, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_unlock_send_kccb_kick(pvr_dev, &ctx_transfer->cccb, ctx_fw_addr, NULL);
	if (err)
		goto err_cccb_unlock_rollback;

	/* Signal completion of transfer job */
	if (transfer_args->out_syncobj) {
		drm_syncobj_replace_fence(out_syncobj, out_fence);
		drm_syncobj_put(out_syncobj);
	}

	release_fences(&in_fences, false);

	if (num_in_fences)
		kfree(in_ufos);

	return 0;

err_cccb_unlock_rollback:
	pvr_cccb_unlock_rollback(&ctx_transfer->cccb);

err_kfree_in_ufos:
	if (num_in_fences)
		kfree(in_ufos);

err_release_fences:
	release_fences(&in_fences, true);

err_put_out_syncobj:
	if (transfer_args->out_syncobj)
		drm_syncobj_put(out_syncobj);

err_out:
	return err;
}

static int pvr_fw_cmd_init(struct pvr_device *pvr_dev, const struct pvr_stream_cmd_defs *stream_def,
			   u64 stream_userptr, u32 stream_len, u64 ext_stream_userptr,
			   u32 ext_stream_len, void **cmd_out)
{
	void *ext_stream = NULL;
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

	if (ext_stream_userptr) {
		ext_stream = kzalloc(ext_stream_len, GFP_KERNEL);
		if (!ext_stream) {
			err = -ENOMEM;
			goto err_free_stream;
		}

		if (copy_from_user(ext_stream, u64_to_user_ptr(ext_stream_userptr),
				   ext_stream_len)) {
			err = -EFAULT;
			goto err_free_ext_stream;
		}
	}

	err = pvr_stream_process(pvr_dev, stream_def, stream, stream_len, ext_stream,
				 ext_stream_len, cmd_out);
	if (err)
		goto err_free_ext_stream;

	kfree(ext_stream);
	kfree(stream);

	return 0;

err_free_ext_stream:
	kfree(ext_stream);

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

	return out_flags;
}

static int
pvr_process_job_render(struct pvr_device *pvr_dev,
		       struct pvr_file *pvr_file,
		       struct drm_pvr_ioctl_submit_job_args *args,
		       struct drm_pvr_job_render_args *render_args,
		       struct pvr_job *job)
{
	struct pvr_context_render *ctx_render;
	struct rogue_fwif_cmd_geom *cmd_geom;
	struct rogue_fwif_cmd_frag *cmd_frag;
	struct dma_fence *geom_fence = NULL;
	struct dma_fence *out_fence;
	u32 *syncobj_handles_geom = NULL;
	u32 *syncobj_handles_frag = NULL;
	struct pvr_hwrt_data *hwrt;
	int err;

	if (render_args->_padding_54) {
		err = -EINVAL;
		goto err_out;
	}

	/* Verify that at least one command is provided. */
	if (!render_args->geom_stream && !render_args->frag_stream) {
		err = -EINVAL;
		goto err_out;
	}

	if ((render_args->geom_flags & ~DRM_PVR_SUBMIT_JOB_GEOM_CMD_FLAGS_MASK) ||
	    (render_args->frag_flags & ~DRM_PVR_SUBMIT_JOB_FRAG_CMD_FLAGS_MASK)) {
		err = -EINVAL;
		goto err_out;
	}

	/* Copy commands from userspace. */
	if (render_args->geom_stream) {
		err = pvr_fw_cmd_init(pvr_dev, &pvr_cmd_geom_stream, render_args->geom_stream,
				      render_args->geom_stream_len, render_args->geom_ext_stream,
				      render_args->geom_ext_stream_len, (void **)&cmd_geom);
		if (err)
			goto err_out;

		cmd_geom->cmd_shared.cmn.frame_num = 0;
		cmd_geom->flags = convert_geom_flags(render_args->geom_flags);

		if (args->num_in_syncobj_handles) {
			syncobj_handles_geom =
				get_syncobj_handles(args->num_in_syncobj_handles,
						    args->in_syncobj_handles);

			if (IS_ERR(syncobj_handles_geom)) {
				err = PTR_ERR(syncobj_handles_geom);
				goto err_free_cmd_geom;
			}
		}
	}
	if (render_args->frag_stream) {
		err = pvr_fw_cmd_init(pvr_dev, &pvr_cmd_frag_stream, render_args->frag_stream,
				      render_args->frag_stream_len, render_args->frag_ext_stream,
				      render_args->frag_ext_stream_len, (void **)&cmd_frag);
		if (err)
			goto err_free_syncobj_geom;

		cmd_frag->cmd_shared.cmn.frame_num = 0;
		cmd_frag->flags = convert_frag_flags(render_args->frag_flags);

		if (render_args->num_in_syncobj_handles_frag) {
			syncobj_handles_frag =
				get_syncobj_handles(render_args->num_in_syncobj_handles_frag,
						    render_args->in_syncobj_handles_frag);

			if (IS_ERR(syncobj_handles_frag)) {
				err = PTR_ERR(syncobj_handles_frag);
				goto err_free_cmd_frag;
			}
		}
	}

	hwrt = pvr_hwrt_data_lookup(pvr_file, render_args->hwrt_data_set_handle,
				    render_args->hwrt_data_index);
	if (!hwrt) {
		err = -EINVAL;
		goto err_free_syncobj_frag;
	}

	job->ctx = pvr_context_lookup(pvr_file, args->context_handle);
	if (!job->ctx) {
		err = -EINVAL;
		goto err_put_hwrt;
	}
	/* to_pvr_context_render() will validate the context type. */
	ctx_render = to_pvr_context_render(job->ctx);
	if (!ctx_render) {
		err = -EINVAL;
		goto err_put_context;
	}

	out_fence = pvr_fence_create(render_args->frag_stream ?
					&ctx_render->ctx_frag.cccb.pvr_fence_context :
					&ctx_render->ctx_geom.cccb.pvr_fence_context,
				     job->ctx);
	if (IS_ERR(out_fence)) {
		err = PTR_ERR(out_fence);
		goto err_put_context;
	}

	if (render_args->geom_stream && render_args->frag_stream) {
		geom_fence = pvr_fence_create(&ctx_render->ctx_geom.cccb.pvr_fence_context,
					      job->ctx);
		if (IS_ERR(geom_fence)) {
			err = PTR_ERR(geom_fence);
			goto err_put_out_fence;
		}
	}

	if (render_args->geom_stream) {
		err = submit_cmd_geometry(pvr_dev, pvr_file, ctx_render, args, render_args, hwrt,
					  cmd_geom, syncobj_handles_geom,
					  render_args->frag_stream ? geom_fence : out_fence);
		if (err)
			goto err_put_geom_fence;
	}

	if (render_args->frag_stream) {
		err = submit_cmd_fragment(pvr_dev, pvr_file, ctx_render, args, render_args, hwrt,
					  cmd_frag, syncobj_handles_frag, geom_fence, out_fence);
		if (err)
			goto err_put_geom_fence;
	}

	dma_fence_put(geom_fence);
	dma_fence_put(out_fence);
	pvr_context_put(job->ctx);
	pvr_hwrt_data_put(hwrt);

	kfree(cmd_frag);
	kfree(cmd_geom);

	return 0;

err_put_geom_fence:
	/* As geom_fence will now never be signaled, we need to drop two references here. */
	pvr_fence_deactivate_and_put(geom_fence);
	dma_fence_put(geom_fence);

err_put_out_fence:
	/* As out_fence will now never be signaled, we need to drop two references here. */
	pvr_fence_deactivate_and_put(out_fence);
	dma_fence_put(out_fence);

err_put_context:
	pvr_context_put(job->ctx);

err_put_hwrt:
	pvr_hwrt_data_put(hwrt);

err_free_syncobj_frag:
	kfree(syncobj_handles_frag);

err_free_cmd_frag:
	if (render_args->frag_stream)
		kfree(cmd_frag);

err_free_syncobj_geom:
	kfree(syncobj_handles_geom);

err_free_cmd_geom:
	if (render_args->geom_stream)
		kfree(cmd_geom);

err_out:
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
			struct drm_pvr_job_compute_args *compute_args,
			struct pvr_job *job)
{
	struct rogue_fwif_cmd_compute *cmd_compute;
	struct pvr_context_compute *ctx_compute;
	u32 *syncobj_handles = NULL;
	struct dma_fence *out_fence;
	int err;

	if (compute_args->flags & ~DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_FLAGS_MASK) {
		err = -EINVAL;
		goto err_out;
	}

	/* Copy commands from userspace. */
	if (!compute_args->stream) {
		err = -EINVAL;
		goto err_out;
	}

	err = pvr_fw_cmd_init(pvr_dev, &pvr_cmd_compute_stream, compute_args->stream,
			      compute_args->stream_len, compute_args->ext_stream,
			      compute_args->ext_stream_len, (void **)&cmd_compute);
	if (err)
		goto err_out;

	cmd_compute->common.frame_num = 0;
	cmd_compute->flags = convert_compute_flags(compute_args->flags);

	if (args->num_in_syncobj_handles) {
		syncobj_handles = get_syncobj_handles(args->num_in_syncobj_handles,
						      args->in_syncobj_handles);

		if (IS_ERR(syncobj_handles)) {
			err = PTR_ERR(syncobj_handles);
			goto err_free_cmd_compute;
		}
	}

	job->ctx = pvr_context_lookup(pvr_file, args->context_handle);
	if (!job->ctx) {
		err = -EINVAL;
		goto err_free_syncobj;
	}
	/* to_pvr_context_compute() will validate the context type. */
	ctx_compute = to_pvr_context_compute(job->ctx);
	if (!ctx_compute) {
		err = -EINVAL;
		goto err_put_context;
	}

	out_fence = pvr_fence_create(&ctx_compute->cccb.pvr_fence_context, job->ctx);
	if (IS_ERR(out_fence)) {
		err = PTR_ERR(out_fence);
		goto err_put_context;
	}

	err = submit_cmd_compute(pvr_dev, pvr_file, ctx_compute, args, compute_args, cmd_compute,
				 syncobj_handles, out_fence);
	if (err)
		goto err_put_out_fence;

	dma_fence_put(out_fence);
	pvr_context_put(job->ctx);

	kfree(cmd_compute);

	return 0;

err_put_out_fence:
	/* As out_fence will now never be signaled, we need to drop two references here. */
	pvr_fence_deactivate_and_put(out_fence);
	dma_fence_put(out_fence);

err_put_context:
	pvr_context_put(job->ctx);

err_free_syncobj:
	kfree(syncobj_handles);

err_free_cmd_compute:
	kfree(cmd_compute);

err_out:
	return err;
}

static u32
convert_transfer_flags(u32 in_flags)
{
	/* Currently no flags supported. */
	return 0;
}

static int
pvr_process_job_transfer(struct pvr_device *pvr_dev,
			 struct pvr_file *pvr_file,
			 struct drm_pvr_ioctl_submit_job_args *args,
			 struct drm_pvr_job_transfer_args *transfer_args,
			 struct pvr_job *job)
{
	struct rogue_fwif_cmd_transfer *cmd_transfer;
	struct pvr_context_transfer *ctx_transfer;
	u32 *syncobj_handles = NULL;
	struct dma_fence *out_fence;
	int err;

	if (transfer_args->flags & ~DRM_PVR_SUBMIT_JOB_TRANSFER_CMD_FLAGS_MASK) {
		err = -EINVAL;
		goto err_out;
	}

	/* Copy commands from userspace. */
	if (!transfer_args->stream) {
		err = -EINVAL;
		goto err_out;
	}

	err = pvr_fw_cmd_init(pvr_dev, &pvr_cmd_transfer_stream, transfer_args->stream,
			      transfer_args->stream_len, transfer_args->ext_stream,
			      transfer_args->ext_stream_len, (void **)&cmd_transfer);
	if (err)
		goto err_out;

	cmd_transfer->common.frame_num = 0;
	cmd_transfer->flags = convert_transfer_flags(transfer_args->flags);

	if (args->num_in_syncobj_handles) {
		syncobj_handles = get_syncobj_handles(args->num_in_syncobj_handles,
						      args->in_syncobj_handles);

		if (IS_ERR(syncobj_handles)) {
			err = PTR_ERR(syncobj_handles);
			goto err_free_cmd_transfer;
		}
	}

	job->ctx = pvr_context_lookup(pvr_file, args->context_handle);
	if (!job->ctx) {
		err = -EINVAL;
		goto err_free_syncobj;
	}

	ctx_transfer = to_pvr_context_transfer_frag(job->ctx);
	if (!ctx_transfer) {
		err = -EINVAL;
		goto err_put_context;
	}

	out_fence = pvr_fence_create(&ctx_transfer->cccb.pvr_fence_context, job->ctx);
	if (IS_ERR(out_fence)) {
		err = PTR_ERR(out_fence);
		goto err_put_context;
	}

	err = submit_cmd_transfer(pvr_dev, pvr_file, ctx_transfer, args, transfer_args,
				  cmd_transfer, syncobj_handles, out_fence);
	if (err)
		goto err_put_out_fence;

	dma_fence_put(out_fence);
	pvr_context_put(job->ctx);

	kfree(cmd_transfer);

	return 0;

err_put_out_fence:
	/* As out_fence will now never be signaled, we need to drop two references here. */
	pvr_fence_deactivate_and_put(out_fence);
	dma_fence_put(out_fence);

err_put_context:
	pvr_context_put(job->ctx);

err_free_syncobj:
	kfree(syncobj_handles);

err_free_cmd_transfer:
	kfree(cmd_transfer);

err_out:
	return err;
}

static int
pvr_process_job_null(struct pvr_device *pvr_dev,
		     struct pvr_file *pvr_file,
		     struct drm_pvr_ioctl_submit_job_args *args,
		     struct drm_pvr_job_null_args *null_args,
		     struct pvr_job *job)
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
					     pvr_dev->fence_context.fence_context,
					     atomic_inc_return(&pvr_dev->fence_context.fence_id),
					     false);
	if (!fence_array) {
		err = -ENOMEM;
		goto err_free_in_fence_array;
	}

	/* dma_fence_array now owns in_fence_array[] and the fence references within it. */

	drm_syncobj_replace_fence(out_syncobj, &fence_array->base);
	drm_syncobj_put(out_syncobj);

	dma_fence_put(&fence_array->base);
	release_fences(&in_fences, false);

	return 0;

err_free_in_fence_array:
	kfree(in_fence_array);

err_release_in_fences:
	release_fences(&in_fences, false);

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
	struct pvr_job *job;
	int err;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		err = -ENOMEM;
		goto err_out;
	}

	job->type = args->job_type;

	/* Process arguments based on job type */
	switch (job->type) {
	case DRM_PVR_JOB_TYPE_RENDER: {
		struct drm_pvr_job_render_args render_args;

		if (copy_from_user(&render_args, u64_to_user_ptr(args->data),
				   sizeof(render_args))) {
			err = -EFAULT;
			goto err_free;
		}

		err = pvr_process_job_render(pvr_dev, pvr_file, args, &render_args, job);
		if (err)
			goto err_free;
		break;
	}

	case DRM_PVR_JOB_TYPE_COMPUTE: {
		struct drm_pvr_job_compute_args compute_args;

		if (copy_from_user(&compute_args, u64_to_user_ptr(args->data),
				   sizeof(compute_args))) {
			err = -EFAULT;
			goto err_free;
		}

		err = pvr_process_job_compute(pvr_dev, pvr_file, args, &compute_args, job);
		if (err)
			goto err_free;
		break;
	}

	case DRM_PVR_JOB_TYPE_TRANSFER_FRAG: {
		struct drm_pvr_job_transfer_args transfer_args;

		if (copy_from_user(&transfer_args, u64_to_user_ptr(args->data),
				   sizeof(transfer_args))) {
			err = -EFAULT;
			goto err_free;
		}

		err = pvr_process_job_transfer(pvr_dev, pvr_file, args, &transfer_args, job);
		if (err)
			goto err_free;
		break;
	}

	case DRM_PVR_JOB_TYPE_NULL: {
		struct drm_pvr_job_null_args null_args;

		if (copy_from_user(&null_args, u64_to_user_ptr(args->data),
				   sizeof(null_args))) {
			err = -EFAULT;
			goto err_free;
		}

		err = pvr_process_job_null(pvr_dev, pvr_file, args, &null_args, job);
		if (err)
			goto err_free;
		break;
	}

	default:
		err = -EINVAL;
		goto err_out;
	}

	kfree(job);

	return 0;

err_free:
	kfree(job);

err_out:
	return err;
}
