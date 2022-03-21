// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_cccb.h"
#include "pvr_context.h"
#include "pvr_device.h"
#include "pvr_gem.h"
#include "pvr_object.h"
#include "pvr_rogue_fwif.h"
#include "pvr_rogue_fwif_common.h"
#include "pvr_rogue_fwif_resetframework.h"

#include <drm/drm_auth.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/xarray.h>

/* TODO: placeholder */
#define MAX_DEADLINE_MS 30000

#define CTX_COMPUTE_CCCB_SIZE_LOG2 15
#define CTX_FRAG_CCCB_SIZE_LOG2 15
#define CTX_GEOM_CCCB_SIZE_LOG2 15

static int
pvr_init_context_common(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
			struct pvr_context *ctx, int type,
			enum pvr_context_priority priority,
			struct drm_pvr_ioctl_create_context_args *args)
{
	int err;

	ctx->type = type;
	ctx->pvr_dev = pvr_dev;
	ctx->pvr_file = pvr_file;

	ctx->flags = args->flags;
	ctx->priority = priority;

	kref_init(&ctx->ref_count);

	if (args->reset_framework_registers) {
		struct rogue_fwif_rf_cmd *reset_framework;
		struct drm_pvr_reset_framework rf_args;

		if (copy_from_user(&rf_args,
				   u64_to_user_ptr(
					   args->reset_framework_registers),
				   sizeof(rf_args))) {
			err = -EFAULT;
			goto err_out;
		}

		if (rf_args.flags || rf_args.format != DRM_PVR_RF_FORMAT_CDM_1) {
			err = -EINVAL;
			goto err_out;
		}

		if (!PVR_IOCTL_UNION_PADDING_CHECK(&rf_args, data,
						  cdm_format_1) ||
		    !rf_args.data.cdm_format_1.cdm_ctrl_stream_base) {
			err = -EINVAL;
			goto err_out;
		}

		reset_framework = pvr_gem_create_and_map_fw_object(pvr_dev, ROGUE_FWIF_RF_CMD_SIZE,
			PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED,
			&ctx->reset_framework_obj);
		if (IS_ERR(reset_framework)) {
			err = PTR_ERR(reset_framework);
			goto err_out;
		}

		reset_framework->fw_registers.cdmreg_cdm_ctrl_stream_base =
			rf_args.data.cdm_format_1.cdm_ctrl_stream_base;

		pvr_fw_object_vunmap(ctx->reset_framework_obj, reset_framework, true);
	}

	return 0;

err_out:
	return err;
}

static void
pvr_fini_context_common(struct pvr_device *pvr_dev, struct pvr_context *ctx)
{
	if (ctx->reset_framework_obj)
		pvr_fw_object_release(ctx->reset_framework_obj);
}

/**
 * pvr_init_geom_context() - Initialise a geometry context
 * @ctx_render: Pointer to parent render context.
 * @render_ctx_args: Arguments from userspace.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_gem_create_and_map_fw_object().
 */
static int
pvr_init_geom_context(
	struct pvr_context_render *ctx_render,
	struct drm_pvr_ioctl_create_render_context_args *render_ctx_args)
{
	struct pvr_device *pvr_dev = ctx_render->base.pvr_dev;
	struct pvr_context_geom *ctx_geom = &ctx_render->ctx_geom;
	struct rogue_fwif_geom_ctx_state *geom_ctx_state_fw;
	int err;

	ctx_geom->ctx_id = atomic_inc_return(&ctx_render->base.pvr_file->ctx_id);

	err = pvr_cccb_init(pvr_dev, &ctx_geom->cccb, CTX_GEOM_CCCB_SIZE_LOG2, "geometry");
	if (err)
		goto err_out;

	geom_ctx_state_fw = pvr_gem_create_and_map_fw_object(pvr_dev, sizeof(*geom_ctx_state_fw),
		PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED,
		&ctx_geom->ctx_state_obj);
	if (IS_ERR(geom_ctx_state_fw)) {
		err = PTR_ERR(geom_ctx_state_fw);
		goto err_cccb_fini;
	}

	geom_ctx_state_fw->geom_core[0].geom_reg_vdm_call_stack_pointer =
		render_ctx_args->vdm_callstack_addr;

	pvr_fw_object_vunmap(ctx_geom->ctx_state_obj, geom_ctx_state_fw, true);

	return 0;

err_cccb_fini:
	pvr_cccb_fini(&ctx_geom->cccb);

err_out:
	return err;
}

/**
 * pvr_fini_geom_context() - Clean up a geometry context
 * @ctx_render: Pointer to parent render context.
 */
static void
pvr_fini_geom_context(struct pvr_context_render *ctx_render)
{
	struct pvr_context_geom *ctx_geom = &ctx_render->ctx_geom;

	pvr_fw_object_release(ctx_geom->ctx_state_obj);

	pvr_cccb_fini(&ctx_geom->cccb);
}

/**
 * pvr_init_frag_context() - Initialise a fragment context
 * @ctx_render: Pointer to parent render context.
 * @render_ctx_args: Arguments from userspace.
 *
 * Return:
 *  * 0 on success.
 */
static int
pvr_init_frag_context(struct pvr_context_render *ctx_render,
		      struct drm_pvr_ioctl_create_render_context_args *render_ctx_args)
{
	struct pvr_device *pvr_dev = ctx_render->base.pvr_dev;
	struct pvr_file *pvr_file = ctx_render->base.pvr_file;
	struct pvr_context_frag *ctx_frag = &ctx_render->ctx_frag;
	int err;

	ctx_frag->ctx_id = atomic_inc_return(&pvr_file->ctx_id);

	err = pvr_cccb_init(pvr_dev, &ctx_frag->cccb, CTX_FRAG_CCCB_SIZE_LOG2, "fragment");
	if (err)
		goto err_out;

	err = pvr_gem_create_fw_object(pvr_dev, sizeof(struct rogue_fwif_frag_ctx_state),
				       PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
				       DRM_PVR_BO_CREATE_ZEROED, &ctx_frag->ctx_state_obj);
	if (err)
		goto err_cccb_fini;

	return 0;

err_cccb_fini:
	pvr_cccb_fini(&ctx_frag->cccb);

err_out:
	return err;
}

/**
 * pvr_fini_frag_context() - Clean up a fragment context
 * @ctx_render: Pointer to parent render context.
 */
static void
pvr_fini_frag_context(struct pvr_context_render *ctx_render)
{
	struct pvr_context_frag *ctx_frag = &ctx_render->ctx_frag;

	pvr_fw_object_release(ctx_frag->ctx_state_obj);

	pvr_cccb_fini(&ctx_frag->cccb);
}

static int
remap_priority(struct pvr_file *pvr_file, s32 uapi_priority,
	       enum pvr_context_priority *priority_out)
{
	switch (uapi_priority) {
	case DRM_PVR_CTX_PRIORITY_LOW:
		*priority_out = PVR_CTX_PRIORITY_LOW;
		break;
	case DRM_PVR_CTX_PRIORITY_NORMAL:
		*priority_out = PVR_CTX_PRIORITY_MEDIUM;
		break;
	case DRM_PVR_CTX_PRIORITY_HIGH:
		if (!capable(CAP_SYS_NICE) && !drm_is_current_master(from_pvr_file(pvr_file)))
			return -EACCES;
		*priority_out = PVR_CTX_PRIORITY_HIGH;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/**
 * pvr_init_fw_common_context() - Initialise an FW-side common context structure
 * @pvr_file: Pointer to pvr_file structure.
 * @ctx: Pointer to context.
 * @cctx_fw: Pointer to FW common context structure.
 * @dm_type: Data master type.
 * @priority: Context priority.
 * @max_deadline_ms: Maximum deadline for work on this context.
 * @cctx_id: Common context ID.
 * @ctx_state_obj: FW object representing context state.
 * @cccb: Client CCB for this context.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_gem_get_fw_addr().
 */
static int
pvr_init_fw_common_context(struct pvr_file *pvr_file, struct pvr_context *ctx,
			   struct rogue_fwif_fwcommoncontext *cctx_fw,
			   u32 dm_type, u32 priority, u32 max_deadline_ms,
			   u32 cctx_id, struct pvr_fw_object *ctx_state_obj,
			   struct pvr_cccb *cccb)
{
	int err = 0;

	cctx_fw->ccbctl_fw_addr = cccb->ctrl_fw_addr;
	cctx_fw->ccb_fw_addr = cccb->cccb_fw_addr;

	cctx_fw->dm = dm_type;
	cctx_fw->priority = ctx->priority;
	cctx_fw->priority_seq_num = 0;
	cctx_fw->max_deadline_ms = max_deadline_ms;
	cctx_fw->pid = task_tgid_nr(current);
	cctx_fw->server_common_context_id = cctx_id;

	if (ctx->reset_framework_obj) {
		if (!pvr_gem_get_fw_addr(ctx->reset_framework_obj,
					  &cctx_fw->rf_cmd_addr)) {
			err = -EINVAL;
			goto err_out;
		}
	}

	WARN_ON(!pvr_gem_get_fw_addr(pvr_file->fw_mem_ctx_obj, &cctx_fw->fw_mem_context_fw_addr));

	if (!pvr_gem_get_fw_addr(ctx_state_obj, &cctx_fw->context_state_addr)) {
		err = -EINVAL;
		goto err_out;
	}

err_out:
	return err;
}

/**
 * pvr_init_fw_render_context() - Initialise an FW-side render context structure
 * @pvr_file: Pointer to pvr_file structure.
 * @ctx_render: Pointer to parent render context.
 * @args: Context creation arguments from userspace.
 * @render_ctx_args: Render context specific arguments from userspace.
 *
 * Return:
 *  * 0 on success.
 */
static int
pvr_init_fw_render_context(
	struct pvr_file *pvr_file,
	struct pvr_context_render *ctx_render,
	struct drm_pvr_ioctl_create_context_args *args,
	struct drm_pvr_ioctl_create_render_context_args *render_ctx_args)
{
	struct rogue_fwif_geom_registers_caswitch *ctxswitch_regs;
	struct rogue_fwif_fwrendercontext *fw_render_context;
	struct drm_pvr_static_render_context_state srcs_args;
	int err;

	fw_render_context = pvr_gem_create_and_map_fw_object(ctx_render->base.pvr_dev,
		sizeof(*fw_render_context), PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
		DRM_PVR_BO_CREATE_ZEROED, &ctx_render->fw_obj);
	if (IS_ERR(fw_render_context)) {
		err = PTR_ERR(fw_render_context);
		goto err_out;
	}

	/* Copy static render context state from userspace. */
	if (copy_from_user(&srcs_args,
			   u64_to_user_ptr(
				   render_ctx_args->static_render_context_state),
			   sizeof(srcs_args))) {
		err = -EFAULT;
		goto err_destroy_gem_object;
	}

	if (srcs_args.format != DRM_PVR_SRCS_FORMAT_1 ||
	    srcs_args._padding_4) {
		err = -EINVAL;
		goto err_destroy_gem_object;
	}

	if (!PVR_IOCTL_UNION_PADDING_CHECK(&srcs_args, data, format_1)) {
		err = -EINVAL;
		goto err_destroy_gem_object;
	}

	ctxswitch_regs = &fw_render_context->static_render_context_state.ctxswitch_regs[0];

	BUILD_BUG_ON(sizeof(*ctxswitch_regs) != sizeof(srcs_args.data));
	memcpy(ctxswitch_regs, &srcs_args.data, sizeof(srcs_args.data));

	err = pvr_init_fw_common_context(pvr_file, &ctx_render->base,
					 &fw_render_context->geom_context,
					 PVR_FWIF_DM_GEOM, args->priority,
					 MAX_DEADLINE_MS,
					 ctx_render->ctx_geom.ctx_id,
					 ctx_render->ctx_geom.ctx_state_obj,
					 &ctx_render->ctx_geom.cccb);
	if (err)
		goto err_destroy_gem_object;

	err = pvr_init_fw_common_context(pvr_file, &ctx_render->base,
					 &fw_render_context->frag_context,
					 PVR_FWIF_DM_FRAG, args->priority,
					 MAX_DEADLINE_MS,
					 ctx_render->ctx_frag.ctx_id,
					 ctx_render->ctx_frag.ctx_state_obj,
					 &ctx_render->ctx_frag.cccb);
	if (err)
		goto err_destroy_gem_object;

	pvr_fw_object_vunmap(ctx_render->fw_obj, fw_render_context, true);
	return 0;

err_destroy_gem_object:
	pvr_fw_object_vunmap(ctx_render->fw_obj, fw_render_context, true);
	pvr_fw_object_release(ctx_render->fw_obj);

err_out:
	return err;
}

/**
 * pvr_fini_fw_render_context() - Clean up an FW-side render context structure
 * @ctx_render: Pointer to parent render context.
 */
static void
pvr_fini_fw_render_context(struct pvr_context_render *ctx_render)
{
	pvr_fw_object_release(ctx_render->fw_obj);
}

/**
 * pvr_init_compute_context() - Initialise a compute context structure
 * @pvr_file: Pointer to pvr_file structure.
 * @ctx_compute: Pointer to parent compute context.
 * @args: Context creation arguments from userspace.
 * @compute_ctx_args: Compute context specific arguments from userspace.
 *
 * Return:
 *  * 0 on success.
 */
static int
pvr_init_compute_context(
	struct pvr_file *pvr_file,
	struct pvr_context_compute *ctx_compute,
	struct drm_pvr_ioctl_create_context_args *args,
	struct drm_pvr_ioctl_create_compute_context_args *compute_ctx_args)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct rogue_fwif_cdm_registers_cswitch *ctxswitch_regs;
	struct rogue_fwif_fwcomputecontext *fw_compute_context;
	struct drm_pvr_static_compute_context_state sccs_args;
	int err;

	ctx_compute->ctx_id = atomic_inc_return(&pvr_file->ctx_id);

	err = pvr_cccb_init(pvr_dev, &ctx_compute->cccb, CTX_COMPUTE_CCCB_SIZE_LOG2, "compute");
	if (err)
		goto err_out;

	err = pvr_gem_create_fw_object(pvr_dev, sizeof(struct rogue_fwif_compute_ctx_state),
				       PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
				       DRM_PVR_BO_CREATE_ZEROED, &ctx_compute->ctx_state_obj);
	if (err)
		goto err_cccb_fini;

	fw_compute_context = pvr_gem_create_and_map_fw_object(ctx_compute->base.pvr_dev,
		sizeof(*fw_compute_context), PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
		DRM_PVR_BO_CREATE_ZEROED, &ctx_compute->fw_obj);
	if (IS_ERR(fw_compute_context)) {
		err = PTR_ERR(fw_compute_context);
		goto err_destroy_ctx_state_obj;
	}

	/* Copy static render context state from userspace. */
	if (copy_from_user(&sccs_args,
			   u64_to_user_ptr(compute_ctx_args->static_compute_context_state),
			   sizeof(sccs_args))) {
		err = -EFAULT;
		goto err_destroy_gem_object;
	}

	if (sccs_args.format != DRM_PVR_SCCS_FORMAT_1 || sccs_args._padding_4) {
		err = -EINVAL;
		goto err_destroy_gem_object;
	}

	if (!PVR_IOCTL_UNION_PADDING_CHECK(&sccs_args, data, format_1)) {
		err = -EINVAL;
		goto err_destroy_gem_object;
	}

	ctxswitch_regs =
		&fw_compute_context->static_compute_context_state.ctxswitch_regs;

	BUILD_BUG_ON(sizeof(*ctxswitch_regs) != sizeof(sccs_args.data));
	memcpy(ctxswitch_regs, &sccs_args.data, sizeof(sccs_args.data));

	err = pvr_init_fw_common_context(pvr_file, &ctx_compute->base,
					 &fw_compute_context->cdm_context, PVR_FWIF_DM_CDM,
					 args->priority, MAX_DEADLINE_MS,
					 ctx_compute->ctx_id,
					 ctx_compute->ctx_state_obj,
					 &ctx_compute->cccb);
	if (err)
		goto err_destroy_gem_object;

	pvr_fw_object_vunmap(ctx_compute->fw_obj, fw_compute_context, true);
	return 0;

err_destroy_gem_object:
	pvr_fw_object_vunmap(ctx_compute->fw_obj, fw_compute_context, true);
	pvr_fw_object_release(ctx_compute->fw_obj);

err_destroy_ctx_state_obj:
	pvr_fw_object_release(ctx_compute->ctx_state_obj);

err_cccb_fini:
	pvr_cccb_fini(&ctx_compute->cccb);

err_out:
	return err;
}

/**
 * pvr_fini_compute_context() - Clean up a compute context structure
 * @ctx_compute: Pointer to compute context.
 */
static void
pvr_fini_compute_context(struct pvr_context_compute *ctx_compute)
{
	pvr_fw_object_release(ctx_compute->fw_obj);
	pvr_fw_object_release(ctx_compute->ctx_state_obj);
	pvr_cccb_fini(&ctx_compute->cccb);
}

/**
 * pvr_create_render_context() - Create a combination geometry/fragment render
 *                               context and return a handle
 * @pvr_file: Pointer to pvr_file structure.
 * @args: Creation arguments from userspace.
 * @render_ctx_args: Render context creation args from userspace.
 * @handle_out: Output handle pointer.
 *
 * The context is initialised with refcount of 1.
 *
 * Return:
 *  * 0 on success, or
 *  * -%ENOMEM on out-of-memory, or
 *  * Any error returned by xa_alloc().
 */
int
pvr_create_render_context(struct pvr_file *pvr_file,
			  struct drm_pvr_ioctl_create_context_args *args,
			  struct drm_pvr_ioctl_create_render_context_args *render_ctx_args,
			  u32 *handle_out)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct pvr_context_render *ctx_render;
	enum pvr_context_priority priority;
	u32 handle;
	int err;

	if (!render_ctx_args->static_render_context_state) {
		err = -EINVAL;
		goto err_out;
	}

	err = remap_priority(pvr_file, args->priority, &priority);
	if (err)
		goto err_out;

	ctx_render = kzalloc(sizeof(*ctx_render), GFP_KERNEL);
	if (!ctx_render) {
		err = -ENOMEM;
		goto err_out;
	}

	err = pvr_init_context_common(pvr_dev, pvr_file,
				      from_pvr_context_render(ctx_render),
				      DRM_PVR_CTX_TYPE_RENDER, priority, args);
	if (err < 0)
		goto err_free;

	err = pvr_init_geom_context(ctx_render, render_ctx_args);
	if (err < 0)
		goto err_destroy_common_context;

	err = pvr_init_frag_context(ctx_render, render_ctx_args);
	if (err < 0)
		goto err_destroy_geom_context;

	err = pvr_init_fw_render_context(pvr_file, ctx_render, args, render_ctx_args);
	if (err < 0)
		goto err_destroy_frag_context;

	/* Add to context list, and get handle */
	err = xa_alloc(&pvr_file->contexts, &handle,
		       from_pvr_context_render(ctx_render), xa_limit_1_32b,
		       GFP_KERNEL);
	if (err < 0)
		goto err_destroy_fw_render_context;

	*handle_out = handle;

	return 0;

err_destroy_fw_render_context:
	pvr_fini_fw_render_context(ctx_render);

err_destroy_frag_context:
	pvr_fini_frag_context(ctx_render);

err_destroy_geom_context:
	pvr_fini_geom_context(ctx_render);

err_destroy_common_context:
	pvr_fini_context_common(pvr_dev, from_pvr_context_render(ctx_render));

err_free:
	kfree(ctx_render);

err_out:
	return err;
}

/**
 * pvr_create_compute_context() - Create a compute context and return a handle
 * @pvr_file: Pointer to pvr_file structure.
 * @args: Creation arguments from userspace.
 * @compute_ctx_args: Compute context creation args from userspace.
 * @handle_out: Output handle pointer.
 *
 * The context is initialised with refcount of 1.
 *
 * Return:
 *  * 0 on success, or
 *  * -%ENOMEM on out-of-memory, or
 *  * Any error returned by xa_alloc().
 */
int
pvr_create_compute_context(struct pvr_file *pvr_file,
			   struct drm_pvr_ioctl_create_context_args *args,
			   struct drm_pvr_ioctl_create_compute_context_args *compute_ctx_args,
			   u32 *handle_out)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct pvr_context_compute *ctx_compute;
	enum pvr_context_priority priority;
	u32 handle;
	int err;

	if (!compute_ctx_args->static_compute_context_state) {
		err = -EINVAL;
		goto err_out;
	}

	err = remap_priority(pvr_file, args->priority, &priority);
	if (err)
		goto err_out;

	ctx_compute = kzalloc(sizeof(*ctx_compute), GFP_KERNEL);
	if (!ctx_compute) {
		err = -ENOMEM;
		goto err_out;
	}

	err = pvr_init_context_common(pvr_dev, pvr_file,
				      from_pvr_context_compute(ctx_compute),
				      DRM_PVR_CTX_TYPE_COMPUTE, priority, args);
	if (err < 0)
		goto err_free;

	err = pvr_init_compute_context(pvr_file, ctx_compute, args, compute_ctx_args);
	if (err < 0)
		goto err_destroy_common_context;

	/* Add to context list, and get handle */
	err = xa_alloc(&pvr_file->contexts, &handle,
		       from_pvr_context_compute(ctx_compute), xa_limit_1_32b,
		       GFP_KERNEL);
	if (err < 0)
		goto err_destroy_compute_context;

	*handle_out = handle;

	return 0;

err_destroy_compute_context:
	pvr_fini_compute_context(ctx_compute);

err_destroy_common_context:
	pvr_fini_context_common(pvr_dev, from_pvr_context_compute(ctx_compute));

err_free:
	kfree(ctx_compute);

err_out:
	return err;
}

static void
pvr_release_context(struct kref *ref_count)
{
	struct pvr_context *ctx =
		container_of(ref_count, struct pvr_context, ref_count);
	struct pvr_device *pvr_dev = ctx->pvr_dev;

	if (ctx->type == DRM_PVR_CTX_TYPE_RENDER) {
		struct pvr_context_render *ctx_render =
			to_pvr_context_render(ctx);

		WARN_ON(pvr_object_cleanup(pvr_dev, ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT,
					   ctx_render->fw_obj,
					   offsetof(struct rogue_fwif_fwrendercontext,
						    geom_context)));
		WARN_ON(pvr_object_cleanup(pvr_dev, ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT,
					   ctx_render->fw_obj,
					   offsetof(struct rogue_fwif_fwrendercontext,
						    frag_context)));

		pvr_fini_fw_render_context(ctx_render);

		/* Destroy owned geometry & fragment contexts. */
		pvr_fini_frag_context(ctx_render);
		pvr_fini_geom_context(ctx_render);
	} else if (ctx->type == DRM_PVR_CTX_TYPE_COMPUTE) {
		struct pvr_context_compute *ctx_compute =
			to_pvr_context_compute(ctx);

		pvr_fini_compute_context(ctx_compute);
	}

	pvr_fini_context_common(ctx->pvr_dev, ctx);

	kfree(ctx);
}

/**
 * pvr_context_put() - Release reference on context
 * @ctx: Target context.
 */
void
pvr_context_put(struct pvr_context *ctx)
{
	kref_put(&ctx->ref_count, pvr_release_context);
}

/**
 * pvr_context_destroy() - Destroy context
 * @pvr_file: Pointer to pvr_file structure.
 * @handle: Context handle.
 *
 * Removes context from context list and drops initial reference. Context will
 * then be destroyed once all outstanding references are dropped.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EINVAL if context not in context list.
 */
int
pvr_context_destroy(struct pvr_file *pvr_file, u32 handle)
{
	struct pvr_context *ctx = xa_erase(&pvr_file->contexts, handle);

	if (!ctx)
		return -EINVAL;

	pvr_context_put(ctx);

	return 0;
}
