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

#include <drm/drm_gem.h>
#include <drm/drm_syncobj.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/xarray.h>
#include <uapi/drm/pvr_drm.h>

static int
import_fences(struct pvr_file *pvr_file, u32 num_in_syncobj_handles,
	      u64 in_syncobj_handles_p, struct xarray *in_fences, struct pvr_fence_context *context)
{
	const void __user *uptr = u64_to_user_ptr(in_syncobj_handles_p);
	u32 *in_syncobj_handles;
	struct dma_fence *fence;
	unsigned long id;
	int err;
	int i;

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

		err = drm_gem_fence_array_add(in_fences, pvr_fence);
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

err_free_memory:
	kfree(in_syncobj_handles);

err_out:
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
import_implicit_fences(struct pvr_file *pvr_file, struct pvr_job *job,
		       u32 num_in_bo_handles, u64 in_bo_handles_p,
		       struct xarray *in_fences)
{
	struct drm_file *drm_file = from_pvr_file(pvr_file);
	struct drm_pvr_bo_ref *bo_refs;
	struct dma_fence *fence;
	unsigned long id;
	int err;
	int i;

	bo_refs = kvmalloc_array(num_in_bo_handles, sizeof(*bo_refs), GFP_KERNEL);
	if (!bo_refs) {
		err = -ENOMEM;
		goto err_out;
	}

	if (copy_from_user(bo_refs, u64_to_user_ptr(in_bo_handles_p),
			   num_in_bo_handles * sizeof(*bo_refs))) {
		err = -EFAULT;
		goto err_free_bo_refs;
	}

	job->bos = kvmalloc_array(num_in_bo_handles, sizeof(*job->bos), GFP_KERNEL | __GFP_ZERO);
	if (!job->bos) {
		err = -ENOMEM;
		goto err_free_bo_refs;
	}

	job->num_bos = num_in_bo_handles;

	for (i = 0; i < job->num_bos; i++) {
		job->bos[i] = drm_gem_object_lookup(drm_file, bo_refs[i].handle);
		if (job->bos[i])
			goto err_release_fences;

		err = drm_gem_fence_array_add_implicit(in_fences, job->bos[i],
						       bo_refs[i].flags & DRM_PVR_BO_REF_WRITE);
		if (err)
			goto err_release_fences;
	}

	return 0;

err_release_fences:
	xa_for_each(in_fences, id, fence) {
		dma_fence_put(fence);
	}
	xa_destroy(in_fences);

	for (i = 0; i < num_in_bo_handles; i++)
		drm_gem_object_put(job->bos[i]);
	kfree(job->bos);

err_free_bo_refs:
	kfree(bo_refs);

err_out:
	return err;
}

static void release_implicit_fences(struct pvr_job *job, struct xarray *in_fences)
{
	struct dma_fence *fence;
	unsigned long id;
	int i;

	xa_for_each(in_fences, id, fence) {
		dma_fence_put(fence);
	}
	xa_destroy(in_fences);

	for (i = 0; i < job->num_bos; i++)
		drm_gem_object_put(job->bos[i]);
	kfree(job->bos);
}

static void wait_fences(struct xarray *fence_array)
{
	struct dma_fence *fence;
	unsigned long id;

	xa_for_each(fence_array, id, fence) {
		dma_fence_wait(fence, false);
	}
}

static int
submit_cmd_geometry(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
		    struct pvr_context_render *ctx_render,
		    struct drm_pvr_ioctl_submit_job_args *args,
		    struct drm_pvr_job_render_args *render_args, struct pvr_hwrt_data *hwrt,
		    struct rogue_fwif_cmd_geom *cmd_geom, struct dma_fence **out_fence_out)
{
	struct rogue_fwif_cmd_geom_frag_shared *cmd_shared = &cmd_geom->cmd_shared;
	u32 num_in_syncobj_handles = render_args->num_in_syncobj_handles_geom;
	struct pvr_context_geom *ctx_geom = &ctx_render->ctx_geom;
	struct drm_syncobj *out_syncobj;
	struct rogue_fwif_ufo *in_ufos;
	struct rogue_fwif_ufo out_ufo;
	struct dma_fence *out_fence;
	struct xarray in_fences;
	u32 ctx_fw_addr;
	int err;

	WARN_ON(!pvr_gem_get_fw_addr(hwrt->fw_obj, &cmd_shared->hwrt_data_fw_addr));

	WARN_ON(!pvr_gem_get_fw_addr(ctx_render->fw_obj, &ctx_fw_addr));

	out_fence = pvr_fence_create(&ctx_geom->cccb.pvr_fence_context);
	if (IS_ERR(out_fence)) {
		err = PTR_ERR(out_fence);
		goto err_out;
	}

	err = pvr_fence_to_ufo(out_fence, &out_ufo);
	if (err)
		goto err_put_out_fence;

	if (render_args->out_syncobj_geom) {
		out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
					     render_args->out_syncobj_geom);
		if (!out_syncobj) {
			err = -ENOENT;
			goto err_put_out_fence;
		}
	}

	xa_init_flags(&in_fences, XA_FLAGS_ALLOC);

	if (num_in_syncobj_handles) {
		struct dma_fence *fence;
		unsigned long id;
		u32 ufo_nr = 0;

		err = import_fences(pvr_file, num_in_syncobj_handles,
				    render_args->in_syncobj_handles_geom, &in_fences,
				    &ctx_geom->cccb.pvr_fence_context);
		if (err)
			goto err_put_out_syncobj;

		in_ufos = kcalloc(num_in_syncobj_handles, sizeof(*in_ufos), GFP_KERNEL);
		if (!in_ufos) {
			err = -EINVAL;
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

	if (num_in_syncobj_handles) {
		err = pvr_cccb_write_command_with_header(&ctx_geom->cccb,
							 ROGUE_FWIF_CCB_CMD_TYPE_FENCE,
							 num_in_syncobj_handles * sizeof(*in_ufos),
							 in_ufos, args->ext_job_ref, 0);
		if (err)
			goto err_cccb_unlock_rollback;
	}

	/* Submit job to FW */
	err = pvr_cccb_write_command_with_header(&ctx_geom->cccb, ROGUE_FWIF_CCB_CMD_TYPE_GEOM,
						 sizeof(*cmd_geom), cmd_geom, args->ext_job_ref, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_write_command_with_header(&ctx_geom->cccb, ROGUE_FWIF_CCB_CMD_TYPE_UPDATE,
						 sizeof(out_ufo), &out_ufo, args->ext_job_ref, 0);
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

	/* Write out fence to be used as input by fragment job. */
	*out_fence_out = out_fence;
	release_fences(&in_fences, false);

	if (num_in_syncobj_handles)
		kfree(in_ufos);

	return 0;

err_cccb_unlock_rollback:
	pvr_cccb_unlock_rollback(&ctx_geom->cccb);

err_kfree_in_ufos:
	if (num_in_syncobj_handles)
		kfree(in_ufos);

err_release_fences:
	release_fences(&in_fences, true);

err_put_out_syncobj:
	if (render_args->out_syncobj_geom)
		drm_syncobj_put(out_syncobj);

err_put_out_fence:
	/* As out_fence will now never be signaled, we need to drop two references here. */
	pvr_fence_deactivate_and_put(out_fence);
	dma_fence_put(out_fence);

err_out:
	return err;
}

static int
submit_cmd_fragment(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
		    struct pvr_context_render *ctx_render,
		    struct drm_pvr_ioctl_submit_job_args *args,
		    struct drm_pvr_job_render_args *render_args, struct pvr_hwrt_data *hwrt,
		    struct rogue_fwif_cmd_frag *cmd_frag, struct dma_fence *geom_in_fence)
{
	struct rogue_fwif_cmd_geom_frag_shared *cmd_shared = &cmd_frag->cmd_shared;
	u32 num_in_syncobj_handles = render_args->num_in_syncobj_handles_frag;
	struct pvr_context_frag *ctx_frag = &ctx_render->ctx_frag;
	struct drm_syncobj *out_syncobj;
	struct rogue_fwif_ufo *in_ufos;
	struct rogue_fwif_ufo out_ufo;
	struct dma_fence *out_fence;
	struct xarray in_fences;
	u32 ctx_fw_addr;
	int err;

	WARN_ON(!pvr_gem_get_fw_addr(hwrt->fw_obj, &cmd_shared->hwrt_data_fw_addr));

	WARN_ON(!pvr_gem_get_fw_addr(ctx_render->fw_obj, &ctx_fw_addr));
	ctx_fw_addr += offsetof(struct rogue_fwif_fwrendercontext, frag_context);

	out_fence = pvr_fence_create(&ctx_frag->cccb.pvr_fence_context);
	if (IS_ERR(out_fence)) {
		err = PTR_ERR(out_fence);
		goto err_out;
	}

	err = pvr_fence_to_ufo(out_fence, &out_ufo);
	if (err)
		goto err_put_out_fence;

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
			goto err_put_out_fence;
		}
	}

	xa_init_flags(&in_fences, XA_FLAGS_ALLOC);

	if (num_in_syncobj_handles) {
		struct dma_fence *fence;
		unsigned long id;
		u32 ufo_nr = 0;

		err = import_fences(pvr_file, num_in_syncobj_handles,
				    render_args->in_syncobj_handles_frag, &in_fences,
				    &ctx_frag->cccb.pvr_fence_context);
		if (err)
			goto err_put_out_syncobj;

		in_ufos = kcalloc(num_in_syncobj_handles, sizeof(*in_ufos), GFP_KERNEL);
		if (!in_ufos) {
			err = -EINVAL;
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

	if (num_in_syncobj_handles) {
		err = pvr_cccb_write_command_with_header(&ctx_frag->cccb,
							 ROGUE_FWIF_CCB_CMD_TYPE_FENCE,
							 num_in_syncobj_handles * sizeof(*in_ufos),
							 in_ufos, args->ext_job_ref, 0);
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
							 sizeof(geom_in_ufo), &geom_in_ufo,
							 args->ext_job_ref, 0);
		if (err)
			goto err_cccb_unlock_rollback;
	}

	/* Submit job to FW */
	err = pvr_cccb_write_command_with_header(&ctx_frag->cccb, ROGUE_FWIF_CCB_CMD_TYPE_FRAG,
						 sizeof(*cmd_frag), cmd_frag, args->ext_job_ref, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_write_command_with_header(&ctx_frag->cccb, ROGUE_FWIF_CCB_CMD_TYPE_UPDATE,
						 sizeof(out_ufo), &out_ufo, args->ext_job_ref, 0);
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

	dma_fence_put(out_fence);
	release_fences(&in_fences, false);

	if (num_in_syncobj_handles)
		kfree(in_ufos);

	return 0;

err_cccb_unlock_rollback:
	pvr_cccb_unlock_rollback(&ctx_frag->cccb);

err_kfree_in_ufos:
	if (num_in_syncobj_handles)
		kfree(in_ufos);

err_release_fences:
	release_fences(&in_fences, true);

err_put_out_syncobj:
	if (render_args->out_syncobj_frag)
		drm_syncobj_put(out_syncobj);

err_put_out_fence:
	/* As out_fence will now never be signaled, we need to drop two references here. */
	pvr_fence_deactivate_and_put(out_fence);
	dma_fence_put(out_fence);

err_out:
	return err;
}

static int
submit_cmd_compute(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
		   struct pvr_context_compute *ctx_compute,
		   struct drm_pvr_ioctl_submit_job_args *args,
		   struct drm_pvr_job_compute_args *compute_args,
		   struct rogue_fwif_cmd_compute *cmd_compute)
{
	u32 num_in_syncobj_handles = compute_args->num_in_syncobj_handles;
	struct drm_syncobj *out_syncobj;
	struct rogue_fwif_ufo *in_ufos;
	struct rogue_fwif_ufo out_ufo;
	struct dma_fence *out_fence;
	struct xarray in_fences;
	u32 ctx_fw_addr;
	int err;

	WARN_ON(!pvr_gem_get_fw_addr(ctx_compute->fw_obj, &ctx_fw_addr));

	out_fence = pvr_fence_create(&ctx_compute->cccb.pvr_fence_context);
	if (IS_ERR(out_fence)) {
		err = PTR_ERR(out_fence);
		goto err_out;
	}

	err = pvr_fence_to_ufo(out_fence, &out_ufo);
	if (err)
		goto err_put_out_fence;

	if (compute_args->out_syncobj) {
		out_syncobj = drm_syncobj_find(from_pvr_file(pvr_file),
					       compute_args->out_syncobj);
		if (!out_syncobj) {
			err = -ENOENT;
			goto err_put_out_fence;
		}
	}

	xa_init_flags(&in_fences, XA_FLAGS_ALLOC);

	if (num_in_syncobj_handles) {
		struct dma_fence *fence;
		unsigned long id;
		u32 ufo_nr = 0;

		err = import_fences(pvr_file, num_in_syncobj_handles,
				    compute_args->in_syncobj_handles, &in_fences,
				    &ctx_compute->cccb.pvr_fence_context);
		if (err)
			goto err_put_out_syncobj;

		in_ufos = kcalloc(num_in_syncobj_handles, sizeof(*in_ufos), GFP_KERNEL);
		if (!in_ufos) {
			err = -EINVAL;
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

	if (num_in_syncobj_handles) {
		err = pvr_cccb_write_command_with_header(&ctx_compute->cccb,
							 ROGUE_FWIF_CCB_CMD_TYPE_FENCE,
							 num_in_syncobj_handles * sizeof(*in_ufos),
							 in_ufos, args->ext_job_ref, 0);
		if (err)
			goto err_cccb_unlock_rollback;
	}

	/* Submit job to FW */
	err = pvr_cccb_write_command_with_header(&ctx_compute->cccb, ROGUE_FWIF_CCB_CMD_TYPE_CDM,
						 sizeof(*cmd_compute), cmd_compute,
						 args->ext_job_ref, 0);
	if (err)
		goto err_cccb_unlock_rollback;

	err = pvr_cccb_write_command_with_header(&ctx_compute->cccb, ROGUE_FWIF_CCB_CMD_TYPE_UPDATE,
						 sizeof(out_ufo), &out_ufo, args->ext_job_ref, 0);
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

	dma_fence_put(out_fence);
	release_fences(&in_fences, false);

	if (num_in_syncobj_handles)
		kfree(in_ufos);

	return 0;

err_cccb_unlock_rollback:
	pvr_cccb_unlock_rollback(&ctx_compute->cccb);

err_kfree_in_ufos:
	if (num_in_syncobj_handles)
		kfree(in_ufos);

err_release_fences:
	release_fences(&in_fences, true);

err_put_out_syncobj:
	if (compute_args->out_syncobj)
		drm_syncobj_put(out_syncobj);

err_put_out_fence:
	/* As out_fence will now never be signaled, we need to drop two references here. */
	pvr_fence_deactivate_and_put(out_fence);
	dma_fence_put(out_fence);

err_out:
	return err;
}

static int
convert_cmd_geom(struct rogue_fwif_cmd_geom *cmd_geom, struct drm_pvr_cmd_geom *cmd_geom_user)
{
	struct drm_pvr_cmd_geom_format_1 *cmd_geom_format_1 =
		&cmd_geom_user->data.cmd_geom_format_1;

	if (cmd_geom_user->format != DRM_PVR_CMD_GEOM_FORMAT_1 || cmd_geom_user->_padding_4)
		return -EINVAL;

	if (cmd_geom_format_1->flags & ~DRM_PVR_SUBMIT_JOB_GEOM_CMD_FLAGS_MASK)
		return -EINVAL;

	cmd_geom->cmd_shared.cmn.frame_num = cmd_geom_format_1->frame_num;
	/* HWRT and PR buffers filled out later. */
	cmd_geom->flags = cmd_geom_format_1->flags;
	/* PR fence filled out later. */

	cmd_geom->geom_regs.vdm_ctrl_stream_base =
		cmd_geom_format_1->geom_regs.vdm_ctrl_stream_base;
	cmd_geom->geom_regs.tpu_border_colour_table =
		cmd_geom_format_1->geom_regs.tpu_border_colour_table;
	cmd_geom->geom_regs.ppp_ctrl = cmd_geom_format_1->geom_regs.ppp_ctrl;
	cmd_geom->geom_regs.te_psg = cmd_geom_format_1->geom_regs.te_psg;
	cmd_geom->geom_regs.tpu = cmd_geom_format_1->geom_regs.tpu;
	cmd_geom->geom_regs.vdm_context_resume_task0_size =
		cmd_geom_format_1->geom_regs.vdm_context_resume_task0_size;
	cmd_geom->geom_regs.pds_ctrl = cmd_geom_format_1->geom_regs.pds_ctrl;
	cmd_geom->geom_regs.view_idx = cmd_geom_format_1->geom_regs.view_idx;

	return 0;
}

static int
convert_cmd_frag(struct rogue_fwif_cmd_frag *cmd_frag, struct drm_pvr_cmd_frag *cmd_frag_user)
{
	struct drm_pvr_cmd_frag_format_1 *cmd_frag_format_1 =
		&cmd_frag_user->data.cmd_frag_format_1;

	if (cmd_frag_user->format != DRM_PVR_CMD_FRAG_FORMAT_1 || cmd_frag_user->_padding_4)
		return -EINVAL;

	if (cmd_frag_format_1->flags & ~DRM_PVR_SUBMIT_JOB_FRAG_CMD_FLAGS_MASK)
		return -EINVAL;

	cmd_frag->cmd_shared.cmn.frame_num = cmd_frag_format_1->frame_num;
	/* HWRT and PR buffers filled out later. */
	cmd_frag->flags = cmd_frag_format_1->flags;
	cmd_frag->zls_stride = cmd_frag_format_1->zls_stride;
	cmd_frag->sls_stride = cmd_frag_format_1->sls_stride;

	memcpy(cmd_frag->regs.usc_clear_register, cmd_frag_format_1->regs.usc_clear_register,
	       sizeof(cmd_frag->regs.usc_clear_register));
	cmd_frag->regs.usc_pixel_output_ctrl = cmd_frag_format_1->regs.usc_pixel_output_ctrl;
	cmd_frag->regs.isp_bgobjdepth = cmd_frag_format_1->regs.isp_bgobjdepth;
	cmd_frag->regs.isp_bgobjvals = cmd_frag_format_1->regs.isp_bgobjvals;
	cmd_frag->regs.isp_aa = cmd_frag_format_1->regs.isp_aa;
	cmd_frag->regs.isp_ctl = cmd_frag_format_1->regs.isp_ctl;
	cmd_frag->regs.tpu = cmd_frag_format_1->regs.tpu;
	cmd_frag->regs.event_pixel_pds_info = cmd_frag_format_1->regs.event_pixel_pds_info;
	cmd_frag->regs.pixel_phantom = cmd_frag_format_1->regs.pixel_phantom;
	cmd_frag->regs.view_idx = cmd_frag_format_1->regs.view_idx;
	cmd_frag->regs.event_pixel_pds_data = cmd_frag_format_1->regs.event_pixel_pds_data;
	cmd_frag->regs.isp_scissor_base = cmd_frag_format_1->regs.isp_scissor_base;
	cmd_frag->regs.isp_dbias_base = cmd_frag_format_1->regs.isp_dbias_base;
	cmd_frag->regs.isp_oclqry_base = cmd_frag_format_1->regs.isp_oclqry_base;
	cmd_frag->regs.isp_zlsctl = cmd_frag_format_1->regs.isp_zlsctl;
	cmd_frag->regs.isp_zload_store_base = cmd_frag_format_1->regs.isp_zload_store_base;
	cmd_frag->regs.isp_stencil_load_store_base =
		cmd_frag_format_1->regs.isp_stencil_load_store_base;
	cmd_frag->regs.isp_zls_pixels = cmd_frag_format_1->regs.isp_zls_pixels;
	memcpy(cmd_frag->regs.pbe_word, cmd_frag_format_1->regs.pbe_word,
	       sizeof(cmd_frag->regs.pbe_word));
	cmd_frag->regs.tpu_border_colour_table = cmd_frag_format_1->regs.tpu_border_colour_table;
	memcpy(cmd_frag->regs.pds_bgnd, cmd_frag_format_1->regs.pds_bgnd,
	       sizeof(cmd_frag->regs.pds_bgnd));
	memcpy(cmd_frag->regs.pds_pr_bgnd, cmd_frag_format_1->regs.pds_pr_bgnd,
	       sizeof(cmd_frag->regs.pds_pr_bgnd));

	return 0;
}

static int
convert_cmd_compute(struct rogue_fwif_cmd_compute *cmd_compute,
		    struct drm_pvr_cmd_compute *cmd_compute_user)
{
	struct drm_pvr_cmd_compute_format_1 *cmd_compute_format_1 =
		&cmd_compute_user->data.cmd_compute_format_1;

	if (cmd_compute_user->format != DRM_PVR_CMD_COMPUTE_FORMAT_1 ||
	    cmd_compute_user->_padding_4)
		return -EINVAL;

	if (cmd_compute_format_1->flags & ~DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_FLAGS_MASK)
		return -EINVAL;

	cmd_compute->common.frame_num = cmd_compute_format_1->frame_num;
	cmd_compute->flags = cmd_compute_format_1->flags;

	cmd_compute->cmd_regs.tpu_border_colour_table =
		cmd_compute_format_1->regs.tpu_border_colour_table;
	cmd_compute->cmd_regs.cdm_item = cmd_compute_format_1->regs.cdm_item;
	cmd_compute->cmd_regs.compute_cluster = cmd_compute_format_1->regs.compute_cluster;
	cmd_compute->cmd_regs.cdm_ctrl_stream_base =
		cmd_compute_format_1->regs.cdm_ctrl_stream_base;
	cmd_compute->cmd_regs.tpu = cmd_compute_format_1->regs.tpu;
	cmd_compute->cmd_regs.cdm_resume_pds1 = cmd_compute_format_1->regs.cdm_resume_pds1;

	return 0;
}

static int
pvr_fw_cmd_geom_init(struct drm_pvr_job_render_args *render_args,
		     struct rogue_fwif_cmd_geom **cmd_geom_out)
{
	struct drm_pvr_cmd_geom *cmd_geom_user;
	struct rogue_fwif_cmd_geom *cmd_geom;
	int err;

	cmd_geom_user = kzalloc(sizeof(*cmd_geom_user), GFP_KERNEL);
	if (!cmd_geom_user) {
		err = -ENOMEM;
		goto err_out;
	}

	cmd_geom = kzalloc(sizeof(*cmd_geom), GFP_KERNEL);
	if (!cmd_geom) {
		err = -ENOMEM;
		goto err_free_cmd_geom_user;
	}

	if (copy_from_user(cmd_geom_user, u64_to_user_ptr(render_args->cmd_geom),
			   sizeof(*cmd_geom_user))) {
		err = -EFAULT;
		goto err_free_cmd_geom;
	}

	err = convert_cmd_geom(cmd_geom, cmd_geom_user);
	if (err)
		goto err_free_cmd_geom;

	kfree(cmd_geom_user);

	*cmd_geom_out = cmd_geom;

	return 0;

err_free_cmd_geom:
	kfree(cmd_geom);

err_free_cmd_geom_user:
	kfree(cmd_geom_user);

err_out:
	return err;
}

static int
pvr_fw_cmd_frag_init(struct drm_pvr_job_render_args *render_args,
		     struct rogue_fwif_cmd_frag **cmd_frag_out)
{
	struct drm_pvr_cmd_frag *cmd_frag_user;
	struct rogue_fwif_cmd_frag *cmd_frag;
	int err;

	cmd_frag_user = kzalloc(sizeof(*cmd_frag_user), GFP_KERNEL);
	if (!cmd_frag_user) {
		err = -ENOMEM;
		goto err_out;
	}

	cmd_frag = kzalloc(sizeof(*cmd_frag), GFP_KERNEL);
	if (!cmd_frag) {
		err = -ENOMEM;
		goto err_free_cmd_frag_user;
	}

	if (copy_from_user(cmd_frag_user, u64_to_user_ptr(render_args->cmd_frag),
			   sizeof(*cmd_frag_user))) {
		err = -EFAULT;
		goto err_free_cmd_frag;
	}

	err = convert_cmd_frag(cmd_frag, cmd_frag_user);
	if (err)
		goto err_free_cmd_frag;

	kfree(cmd_frag_user);

	*cmd_frag_out = cmd_frag;

	return 0;

err_free_cmd_frag:
	kfree(cmd_frag);

err_free_cmd_frag_user:
	kfree(cmd_frag_user);

err_out:
	return err;
}

static int
pvr_fw_cmd_compute_init(struct drm_pvr_job_compute_args *compute_args,
			struct rogue_fwif_cmd_compute **cmd_compute_out)
{
	struct drm_pvr_cmd_compute *cmd_compute_user;
	struct rogue_fwif_cmd_compute *cmd_compute;
	int err;

	cmd_compute_user = kzalloc(sizeof(*cmd_compute_user), GFP_KERNEL);
	if (!cmd_compute_user) {
		err = -ENOMEM;
		goto err_out;
	}

	cmd_compute = kzalloc(sizeof(*cmd_compute), GFP_KERNEL);
	if (!cmd_compute) {
		err = -ENOMEM;
		goto err_free_cmd_compute_user;
	}

	if (copy_from_user(cmd_compute_user, u64_to_user_ptr(compute_args->cmd),
			   sizeof(*cmd_compute_user))) {
		err = -EFAULT;
		goto err_free_cmd_compute;
	}

	err = convert_cmd_compute(cmd_compute, cmd_compute_user);
	if (err)
		goto err_free_cmd_compute;

	kfree(cmd_compute_user);

	*cmd_compute_out = cmd_compute;

	return 0;

err_free_cmd_compute:
	kfree(cmd_compute);

err_free_cmd_compute_user:
	kfree(cmd_compute_user);

err_out:
	return err;
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
	struct xarray implicit_fences;
	struct dma_fence *geom_fence = NULL;
	struct pvr_hwrt_data *hwrt;
	int err;

	/* Copy commands from userspace. */
	if (render_args->cmd_geom) {
		err = pvr_fw_cmd_geom_init(render_args, &cmd_geom);
		if (err)
			goto err_out;
	}
	if (render_args->cmd_frag) {
		err = pvr_fw_cmd_frag_init(render_args, &cmd_frag);
		if (err)
			goto err_free_cmd_geom;
	}

	hwrt = pvr_hwrt_data_get(pvr_file, render_args->hwrt_data_set_handle,
				 render_args->hwrt_data_index);
	if (!hwrt) {
		err = -EINVAL;
		goto err_free_cmd_frag;
	}

	job->ctx = pvr_context_get(pvr_file, args->context_handle);
	if (!job->ctx) {
		err = -EINVAL;
		goto err_put_hwrt;
	}
	ctx_render = to_pvr_context_render(job->ctx);

	xa_init_flags(&implicit_fences, XA_FLAGS_ALLOC);

	err = import_implicit_fences(pvr_file, job, render_args->num_bo_handles,
				     render_args->bo_handles, &implicit_fences);
	if (err)
		goto err_put_context;

	/* Wait on implicit fences */
	wait_fences(&implicit_fences);

	if (render_args->cmd_geom) {
		err = submit_cmd_geometry(pvr_dev, pvr_file, ctx_render, args, render_args, hwrt,
					  cmd_geom, &geom_fence);
		if (err)
			goto err_release_implicit_fences;
	}

	if (render_args->cmd_frag) {
		err = submit_cmd_fragment(pvr_dev, pvr_file, ctx_render, args, render_args, hwrt,
					  cmd_frag, geom_fence);
		if (err)
			goto err_put_geom_fence;
	}

	dma_fence_put(geom_fence);
	release_implicit_fences(job, &implicit_fences);
	pvr_context_put(job->ctx);
	pvr_hwrt_data_put(hwrt);

	kfree(cmd_frag);
	kfree(cmd_geom);

	return 0;

err_put_geom_fence:
	dma_fence_put(geom_fence);

err_release_implicit_fences:
	release_implicit_fences(job, &implicit_fences);

err_put_context:
	pvr_context_put(job->ctx);

err_put_hwrt:
	pvr_hwrt_data_put(hwrt);

err_free_cmd_frag:
	if (render_args->cmd_frag)
		kfree(cmd_frag);

err_free_cmd_geom:
	if (render_args->cmd_geom)
		kfree(cmd_geom);

err_out:
	return err;
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
	struct xarray implicit_fences;
	int err;

	/* Copy commands from userspace. */
	if (!compute_args->cmd || compute_args->_padding_24) {
		err = -EINVAL;
		goto err_out;
	}

	err = pvr_fw_cmd_compute_init(compute_args, &cmd_compute);
	if (err)
		goto err_out;

	job->ctx = pvr_context_get(pvr_file, args->context_handle);
	if (!job->ctx) {
		err = -EINVAL;
		goto err_free_cmd_compute;
	}
	ctx_compute = to_pvr_context_compute(job->ctx);

	xa_init_flags(&implicit_fences, XA_FLAGS_ALLOC);

	err = import_implicit_fences(pvr_file, job, compute_args->num_bo_handles,
				     compute_args->bo_handles, &implicit_fences);
	if (err)
		goto err_put_context;

	/* Wait on implicit fences */
	wait_fences(&implicit_fences);

	err = submit_cmd_compute(pvr_dev, pvr_file, ctx_compute, args, compute_args, cmd_compute);
	if (err)
		goto err_release_implicit_fences;

	release_implicit_fences(job, &implicit_fences);
	pvr_context_put(job->ctx);

	kfree(cmd_compute);

	return 0;

err_release_implicit_fences:
	release_implicit_fences(job, &implicit_fences);

err_put_context:
	pvr_context_put(job->ctx);

err_free_cmd_compute:
	kfree(cmd_compute);

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

	if (args->_padding_c) {
		err = -EINVAL;
		goto err_out;
	}

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
