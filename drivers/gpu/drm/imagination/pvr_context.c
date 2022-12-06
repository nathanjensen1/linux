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

#define CLEANUP_SLEEP_TIME_MS 20

#define CTX_COMPUTE_CCCB_SIZE_LOG2 15
#define CTX_FRAG_CCCB_SIZE_LOG2 15
#define CTX_GEOM_CCCB_SIZE_LOG2 15
#define CTX_TRANSFER_CCCB_SIZE_LOG2 15

static int
pvr_init_context_common(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
			struct pvr_context *ctx, int type,
			enum pvr_context_priority priority,
			struct drm_pvr_ioctl_create_context_args *args,
			u32 id)
{
	ctx->type = type;
	ctx->pvr_dev = pvr_dev;
	ctx->vm_ctx = pvr_vm_context_get(pvr_file->user_vm_ctx);

	ctx->flags = args->flags;
	ctx->priority = priority;

	ctx->ctx_id = id;

	kref_init(&ctx->ref_count);

	return 0;
}

static void
pvr_fini_context_common(struct pvr_device *pvr_dev, struct pvr_context *ctx)
{
	pvr_vm_context_put(ctx->vm_ctx);
}

/**
 * pvr_init_geom_context() - Initialise a geometry context
 * @pvr_file: Pointer to pvr_file structure.
 * @ctx_render: Pointer to parent render context.
 * @args: Arguments from userspace.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_gem_create_and_map_fw_object().
 */
static int
pvr_init_geom_context(struct pvr_file *pvr_file,
		      struct pvr_context_render *ctx_render,
		      struct drm_pvr_ioctl_create_context_args *args)
{
	struct pvr_device *pvr_dev = ctx_render->base.pvr_dev;
	struct pvr_context_geom *ctx_geom = &ctx_render->ctx_geom;
	struct rogue_fwif_geom_ctx_state *geom_ctx_state_fw;
	int err;

	err = pvr_cccb_init(pvr_dev, &ctx_geom->cccb, CTX_GEOM_CCCB_SIZE_LOG2, "geometry");
	if (err)
		goto err_out;

	geom_ctx_state_fw = pvr_gem_create_and_map_fw_object(pvr_dev, sizeof(*geom_ctx_state_fw),
							     PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
							     DRM_PVR_BO_CREATE_ZEROED,
							     &ctx_geom->ctx_state_obj);
	if (IS_ERR(geom_ctx_state_fw)) {
		err = PTR_ERR(geom_ctx_state_fw);
		goto err_cccb_fini;
	}

	geom_ctx_state_fw->geom_core[0].geom_reg_vdm_call_stack_pointer = args->callstack_addr;

	pvr_fw_object_vunmap(ctx_geom->ctx_state_obj, true);

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
 * @pvr_file: Pointer to pvr_file structure.
 * @ctx_render: Pointer to parent render context.
 * @args: Arguments from userspace.
 *
 * Return:
 *  * 0 on success.
 */
static int
pvr_init_frag_context(struct pvr_file *pvr_file,
		      struct pvr_context_render *ctx_render,
		      struct drm_pvr_ioctl_create_context_args *args)
{
	struct pvr_device *pvr_dev = ctx_render->base.pvr_dev;
	struct pvr_context_frag *ctx_frag = &ctx_render->ctx_frag;
	u32 num_isp_store_registers;
	size_t frag_ctx_state_size;
	int err;

	err = pvr_cccb_init(pvr_dev, &ctx_frag->cccb, CTX_FRAG_CCCB_SIZE_LOG2, "fragment");
	if (err)
		goto err_out;

	if (PVR_HAS_FEATURE(pvr_dev, xe_memory_hierarchy)) {
		WARN_ON(PVR_FEATURE_VALUE(pvr_dev, num_raster_pipes, &num_isp_store_registers));

		if (PVR_HAS_FEATURE(pvr_dev, gpu_multicore_support)) {
			u32 xpu_max_slaves;

			WARN_ON(PVR_FEATURE_VALUE(pvr_dev, xpu_max_slaves, &xpu_max_slaves));

			num_isp_store_registers *= (1 + xpu_max_slaves);
		}
	} else {
		WARN_ON(PVR_FEATURE_VALUE(pvr_dev, num_isp_ipp_pipes, &num_isp_store_registers));
	}

	frag_ctx_state_size = sizeof(struct rogue_fwif_frag_ctx_state) + num_isp_store_registers *
			      sizeof(((struct rogue_fwif_frag_ctx_state *)0)->frag_reg_isp_store[0]);

	err = pvr_gem_create_fw_object(pvr_dev, frag_ctx_state_size,
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
 * @ctx: Pointer to context.
 * @cctx_fw: Pointer to FW common context structure.
 * @dm_type: Data master type.
 * @priority: Context priority.
 * @max_deadline_ms: Maximum deadline for work on this context.
 * @cctx_id: Common context ID.
 * @ctx_state_obj: FW object representing context state.
 * @cccb: Client CCB for this context.
 */
static void
pvr_init_fw_common_context(struct pvr_context *ctx,
			   struct rogue_fwif_fwcommoncontext *cctx_fw,
			   u32 dm_type, u32 priority, u32 max_deadline_ms,
			   u32 cctx_id, struct pvr_fw_object *ctx_state_obj,
			   struct pvr_cccb *cccb)
{
	struct pvr_fw_object *fw_mem_ctx_obj = pvr_vm_get_fw_mem_context(ctx->vm_ctx);

	cctx_fw->ccbctl_fw_addr = cccb->ctrl_fw_addr;
	cctx_fw->ccb_fw_addr = cccb->cccb_fw_addr;

	cctx_fw->dm = dm_type;
	cctx_fw->priority = ctx->priority;
	cctx_fw->priority_seq_num = 0;
	cctx_fw->max_deadline_ms = max_deadline_ms;
	cctx_fw->pid = task_tgid_nr(current);
	cctx_fw->server_common_context_id = cctx_id;

	pvr_gem_get_fw_addr(fw_mem_ctx_obj, &cctx_fw->fw_mem_context_fw_addr);

	pvr_gem_get_fw_addr(ctx_state_obj, &cctx_fw->context_state_addr);
}

static void
pvr_fini_fw_common_context(struct pvr_context *ctx)
{
}

/**
 * pvr_init_fw_render_context() - Initialise an FW-side render context structure
 * @ctx_render: Pointer to parent render context.
 * @args: Context creation arguments from userspace.
 *
 * Return:
 *  * 0 on success.
 */
static int
pvr_init_fw_render_context(struct pvr_context_render *ctx_render,
			   struct drm_pvr_ioctl_create_context_args *args)
{
	struct rogue_fwif_static_rendercontext_state *static_rendercontext_state;
	struct rogue_fwif_fwrendercontext *fw_render_context;
	int err;

	if (args->static_context_state_len != sizeof(*static_rendercontext_state)) {
		err = -EINVAL;
		goto err_out;
	}

	fw_render_context = pvr_gem_create_and_map_fw_object(ctx_render->base.pvr_dev,
							     sizeof(*fw_render_context),
							     PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
							     DRM_PVR_BO_CREATE_ZEROED,
							     &ctx_render->fw_obj);
	if (IS_ERR(fw_render_context)) {
		err = PTR_ERR(fw_render_context);
		goto err_out;
	}

	static_rendercontext_state = &fw_render_context->static_render_context_state;

	/* Copy static render context state from userspace. */
	if (copy_from_user(static_rendercontext_state, u64_to_user_ptr(args->static_context_state),
			   sizeof(*static_rendercontext_state))) {
		err = -EFAULT;
		goto err_destroy_gem_object;
	}

	pvr_init_fw_common_context(&ctx_render->base, &fw_render_context->geom_context,
				   PVR_FWIF_DM_GEOM, args->priority, MAX_DEADLINE_MS,
				   ctx_render->base.ctx_id, ctx_render->ctx_geom.ctx_state_obj,
				   &ctx_render->ctx_geom.cccb);

	pvr_init_fw_common_context(&ctx_render->base, &fw_render_context->frag_context,
				   PVR_FWIF_DM_FRAG, args->priority, MAX_DEADLINE_MS,
				   ctx_render->base.ctx_id, ctx_render->ctx_frag.ctx_state_obj,
				   &ctx_render->ctx_frag.cccb);

	pvr_fw_object_vunmap(ctx_render->fw_obj, true);
	return 0;

err_destroy_gem_object:
	pvr_fw_object_vunmap(ctx_render->fw_obj, true);
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
	struct pvr_context *ctx = from_pvr_context_render(ctx_render);

	pvr_fini_fw_common_context(ctx);
	pvr_fini_fw_common_context(ctx);

	pvr_fw_object_release(ctx_render->fw_obj);
}

/**
 * pvr_init_compute_context() - Initialise a compute context structure
 * @pvr_file: Pointer to pvr_file structure.
 * @ctx_compute: Pointer to parent compute context.
 * @args: Context creation arguments from userspace.
 *
 * Return:
 *  * 0 on success.
 */
static int
pvr_init_compute_context(struct pvr_file *pvr_file, struct pvr_context_compute *ctx_compute,
			 struct drm_pvr_ioctl_create_context_args *args)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct rogue_fwif_cdm_registers_cswitch *ctxswitch_regs;
	struct rogue_fwif_fwcomputecontext *fw_compute_context;
	int err;

	if (args->static_context_state_len != sizeof(*ctxswitch_regs)) {
		err = -EINVAL;
		goto err_out;
	}

	err = pvr_cccb_init(pvr_dev, &ctx_compute->cccb, CTX_COMPUTE_CCCB_SIZE_LOG2, "compute");
	if (err)
		goto err_out;

	err = pvr_gem_create_fw_object(pvr_dev, sizeof(struct rogue_fwif_compute_ctx_state),
				       PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
				       DRM_PVR_BO_CREATE_ZEROED, &ctx_compute->ctx_state_obj);
	if (err)
		goto err_cccb_fini;

	fw_compute_context = pvr_gem_create_and_map_fw_object(ctx_compute->base.pvr_dev,
							      sizeof(*fw_compute_context),
							      PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
							      DRM_PVR_BO_CREATE_ZEROED,
							      &ctx_compute->fw_obj);
	if (IS_ERR(fw_compute_context)) {
		err = PTR_ERR(fw_compute_context);
		goto err_destroy_ctx_state_obj;
	}

	ctxswitch_regs =
		&fw_compute_context->static_compute_context_state.ctxswitch_regs;

	/* Copy static compute context state from userspace. */
	if (copy_from_user(ctxswitch_regs,
			   u64_to_user_ptr(args->static_context_state),
			   sizeof(*ctxswitch_regs))) {
		err = -EFAULT;
		goto err_destroy_gem_object;
	}

	pvr_init_fw_common_context(&ctx_compute->base, &fw_compute_context->cdm_context,
				   PVR_FWIF_DM_CDM, args->priority, MAX_DEADLINE_MS,
				   ctx_compute->base.ctx_id, ctx_compute->ctx_state_obj,
				   &ctx_compute->cccb);

	pvr_fw_object_vunmap(ctx_compute->fw_obj, true);
	return 0;

err_destroy_gem_object:
	pvr_fw_object_vunmap(ctx_compute->fw_obj, true);
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
	struct pvr_context *ctx = from_pvr_context_compute(ctx_compute);

	pvr_fini_fw_common_context(ctx);
	pvr_fw_object_release(ctx_compute->fw_obj);
	pvr_fw_object_release(ctx_compute->ctx_state_obj);
	pvr_cccb_fini(&ctx_compute->cccb);
}

/**
 * pvr_init_transfer_context() - Initialise a transfer context structure
 * @pvr_file: Pointer to pvr_file structure.
 * @ctx_transfer: Pointer to parent transfer context.
 * @args: Context creation arguments from userspace.
 *
 * Return:
 *  * 0 on success.
 */
static int
pvr_init_transfer_context(struct pvr_file *pvr_file, struct pvr_context_transfer *ctx_transfer,
			  struct drm_pvr_ioctl_create_context_args *args)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct rogue_fwif_fwtransfercontext *fw_transfer_context;
	int err;

	err = pvr_cccb_init(pvr_dev, &ctx_transfer->cccb, CTX_TRANSFER_CCCB_SIZE_LOG2,
			    "transfer_frag");
	if (err)
		goto err_out;

	err = pvr_gem_create_fw_object(pvr_dev, sizeof(struct rogue_fwif_frag_ctx_state),
				       PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
				       DRM_PVR_BO_CREATE_ZEROED, &ctx_transfer->ctx_state_obj);
	if (err)
		goto err_cccb_fini;

	fw_transfer_context = pvr_gem_create_and_map_fw_object(ctx_transfer->base.pvr_dev,
							       sizeof(*fw_transfer_context),
							       PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
							       DRM_PVR_BO_CREATE_ZEROED,
							       &ctx_transfer->fw_obj);
	if (IS_ERR(fw_transfer_context)) {
		err = PTR_ERR(fw_transfer_context);
		goto err_destroy_ctx_state_obj;
	}

	pvr_init_fw_common_context(&ctx_transfer->base, &fw_transfer_context->tq_context,
				   PVR_FWIF_DM_FRAG, args->priority, MAX_DEADLINE_MS,
				   ctx_transfer->base.ctx_id, ctx_transfer->ctx_state_obj,
				   &ctx_transfer->cccb);

	pvr_fw_object_vunmap(ctx_transfer->fw_obj, true);
	return 0;

err_destroy_ctx_state_obj:
	pvr_fw_object_release(ctx_transfer->ctx_state_obj);

err_cccb_fini:
	pvr_cccb_fini(&ctx_transfer->cccb);

err_out:
	return err;
}

/**
 * pvr_fini_transfer_context() - Clean up a transfer context structure
 * @ctx_transfer: Pointer to transfer context.
 */
static void
pvr_fini_transfer_context(struct pvr_context_transfer *ctx_transfer)
{
	struct pvr_context *ctx = from_pvr_context_transfer(ctx_transfer);

	pvr_fini_fw_common_context(ctx);
	pvr_fw_object_release(ctx_transfer->fw_obj);
	pvr_fw_object_release(ctx_transfer->ctx_state_obj);
	pvr_cccb_fini(&ctx_transfer->cccb);
}

/**
 * pvr_create_render_context() - Create a combination geometry/fragment render
 *                               context and return a handle
 * @pvr_file: Pointer to pvr_file structure.
 * @args: Creation arguments from userspace.
 * @id: FW context ID.
 *
 * The context is initialised with refcount of 1.
 *
 * Return:
 *  * Context pointer on success, or
 *  * -%ENOMEM on out-of-memory, or
 *  * Any error returned by xa_alloc().
 */
struct pvr_context *
pvr_create_render_context(struct pvr_file *pvr_file,
			  struct drm_pvr_ioctl_create_context_args *args,
			  u32 id)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct pvr_context_render *ctx_render;
	enum pvr_context_priority priority;
	int err;

	if (!args->static_context_state) {
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
				      DRM_PVR_CTX_TYPE_RENDER, priority, args, id);
	if (err < 0)
		goto err_free;

	err = pvr_init_geom_context(pvr_file, ctx_render, args);
	if (err < 0)
		goto err_destroy_common_context;

	err = pvr_init_frag_context(pvr_file, ctx_render, args);
	if (err < 0)
		goto err_destroy_geom_context;

	err = pvr_init_fw_render_context(ctx_render, args);
	if (err < 0)
		goto err_destroy_frag_context;

	return from_pvr_context_render(ctx_render);

err_destroy_frag_context:
	pvr_fini_frag_context(ctx_render);

err_destroy_geom_context:
	pvr_fini_geom_context(ctx_render);

err_destroy_common_context:
	pvr_fini_context_common(pvr_dev, from_pvr_context_render(ctx_render));

err_free:
	kfree(ctx_render);

err_out:
	return ERR_PTR(err);
}

/**
 * pvr_create_compute_context() - Create a compute context and return a handle
 * @pvr_file: Pointer to pvr_file structure.
 * @args: Creation arguments from userspace.
 * @id: FW context ID.
 *
 * The context is initialised with refcount of 1.
 *
 * Return:
 *  * Context pointer on success, or
 *  * -%ENOMEM on out-of-memory, or
 *  * Any error returned by xa_alloc().
 */
struct pvr_context *
pvr_create_compute_context(struct pvr_file *pvr_file,
			   struct drm_pvr_ioctl_create_context_args *args,
			   u32 id)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct pvr_context_compute *ctx_compute;
	enum pvr_context_priority priority;
	int err;

	if (!args->static_context_state || args->callstack_addr) {
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
				      DRM_PVR_CTX_TYPE_COMPUTE, priority, args, id);
	if (err < 0)
		goto err_free;

	err = pvr_init_compute_context(pvr_file, ctx_compute, args);
	if (err < 0)
		goto err_destroy_common_context;

	return from_pvr_context_compute(ctx_compute);

err_destroy_common_context:
	pvr_fini_context_common(pvr_dev, from_pvr_context_compute(ctx_compute));

err_free:
	kfree(ctx_compute);

err_out:
	return ERR_PTR(err);
}

/**
 * pvr_create_transfer_context() - Create a transfer context and return a handle
 * @pvr_file: Pointer to pvr_file structure.
 * @args: Creation arguments from userspace.
 * @id: FW context ID.
 *
 * The context is initialised with refcount of 1.
 *
 * Return:
 *  * Context pointer on success, or
 *  * -%ENOMEM on out-of-memory, or
 *  * Any error returned by xa_alloc().
 */
struct pvr_context *
pvr_create_transfer_context(struct pvr_file *pvr_file,
			    struct drm_pvr_ioctl_create_context_args *args,
			    u32 id)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct pvr_context_transfer *ctx_transfer;
	enum pvr_context_priority priority;
	int err;

	if (args->callstack_addr || args->static_context_state) {
		err = -EINVAL;
		goto err_out;
	}

	err = remap_priority(pvr_file, args->priority, &priority);
	if (err)
		goto err_out;

	ctx_transfer = kzalloc(sizeof(*ctx_transfer), GFP_KERNEL);
	if (!ctx_transfer) {
		err = -ENOMEM;
		goto err_out;
	}

	err = pvr_init_context_common(pvr_dev, pvr_file,
				      from_pvr_context_transfer(ctx_transfer),
				      args->type, priority, args, id);
	if (err < 0)
		goto err_free;

	err = pvr_init_transfer_context(pvr_file, ctx_transfer, args);
	if (err < 0)
		goto err_destroy_common_context;

	return from_pvr_context_transfer(ctx_transfer);

err_destroy_common_context:
	pvr_fini_context_common(pvr_dev, from_pvr_context_transfer(ctx_transfer));

err_free:
	kfree(ctx_transfer);

err_out:
	return ERR_PTR(err);
}

static void
pvr_release_context(struct kref *ref_count)
{
	struct pvr_context *ctx =
		container_of(ref_count, struct pvr_context, ref_count);
	struct pvr_device *pvr_dev = ctx->pvr_dev;

	WARN_ON(pvr_context_wait_idle(ctx, 0));

	xa_erase(&pvr_dev->ctx_ids, ctx->ctx_id);

	if (ctx->type == DRM_PVR_CTX_TYPE_RENDER) {
		struct pvr_context_render *ctx_render = to_pvr_context_render(ctx);

		pvr_fini_fw_render_context(ctx_render);

		/* Destroy owned geometry & fragment contexts. */
		pvr_fini_frag_context(ctx_render);
		pvr_fini_geom_context(ctx_render);
	} else if (ctx->type == DRM_PVR_CTX_TYPE_COMPUTE) {
		struct pvr_context_compute *ctx_compute = to_pvr_context_compute(ctx);

		pvr_fini_compute_context(ctx_compute);
	} else if (ctx->type == DRM_PVR_CTX_TYPE_TRANSFER_FRAG) {
		struct pvr_context_transfer *ctx_transfer = to_pvr_context_transfer_frag(ctx);

		pvr_fini_transfer_context(ctx_transfer);
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
 * @handle: Userspace context handle.
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
	struct pvr_context *ctx = xa_load(&pvr_file->ctx_handles, handle);

	if (!ctx)
		return -EINVAL;

	xa_erase(&pvr_file->ctx_handles, handle);
	pvr_context_put(ctx);

	return 0;
}

/**
 * pvr_destroy_contexts_for_file: Destroy any contexts associated with the given file
 * @pvr_file: Pointer to pvr_file structure.
 *
 * Removes all contexts associated with @pvr_file from the device context list and drops initial
 * references. Contexts will then be destroyed once all outstanding references are dropped.
 */
void pvr_destroy_contexts_for_file(struct pvr_file *pvr_file)
{
	struct pvr_context *ctx;
	unsigned long handle;

	xa_for_each(&pvr_file->ctx_handles, handle, ctx) {
		xa_erase(&pvr_file->ctx_handles, handle);
		pvr_context_put(ctx);
	}
}

/**
 * pvr_context_wait_idle() - Wait for context to go idle
 * @ctx: Target context.
 * @timeout: Timeout, in jiffies
 *
 * Return:
 *  * 0 on success, or
 *  * -ETIMEDOUT on timeout.
 */
int
pvr_context_wait_idle(struct pvr_context *ctx, u32 timeout)
{
	struct pvr_device *pvr_dev = ctx->pvr_dev;
	u32 jiffies_start = jiffies;
	int err = 0;

	if (ctx->type == DRM_PVR_CTX_TYPE_RENDER) {
		struct pvr_context_render *ctx_render = to_pvr_context_render(ctx);

		do {
			err = pvr_object_cleanup(pvr_dev, ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT,
						 ctx_render->fw_obj,
						 offsetof(struct rogue_fwif_fwrendercontext,
							  geom_context));
			if (err && err != -EBUSY)
				goto err_out;

			if (!err) {
				err = pvr_object_cleanup(pvr_dev,
							 ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT,
							 ctx_render->fw_obj,
							 offsetof(struct rogue_fwif_fwrendercontext,
								  frag_context));
				if (err && err != -EBUSY)
					goto err_out;
			}

			if (err)
				msleep(CLEANUP_SLEEP_TIME_MS);
		} while (err && (jiffies - jiffies_start) < timeout);
	} else if (ctx->type == DRM_PVR_CTX_TYPE_COMPUTE) {
		struct pvr_context_compute *ctx_compute = to_pvr_context_compute(ctx);

		do {
			err = pvr_object_cleanup(pvr_dev, ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT,
						 ctx_compute->fw_obj,
						 offsetof(struct rogue_fwif_fwcomputecontext,
							  cdm_context));
			if (err && err != -EBUSY)
				goto err_out;
			if (err)
				msleep(CLEANUP_SLEEP_TIME_MS);
		} while (err && (jiffies - jiffies_start) < timeout);
	} else if (ctx->type == DRM_PVR_CTX_TYPE_TRANSFER_FRAG) {
		struct pvr_context_transfer *ctx_transfer = to_pvr_context_transfer_frag(ctx);

		do {
			err = pvr_object_cleanup(pvr_dev, ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT,
						 ctx_transfer->fw_obj,
						 offsetof(struct rogue_fwif_fwtransfercontext,
							  tq_context));
			if (err && err != -EBUSY)
				goto err_out;
			if (err)
				msleep(CLEANUP_SLEEP_TIME_MS);
		} while (err && (jiffies - jiffies_start) < timeout);
	}

	if (err)
		err = -ETIMEDOUT;

err_out:
	return err;
}

/**
 * pvr_context_fail_fences() - Fail all outstanding fences associated with a context
 * @ctx: Target PowerVR context.
 * @err: Error code.
 *
 * Returns:
 *  * %true if any fences were failed, or
 *  * %false if there were no outstanding fences.
 */
bool
pvr_context_fail_fences(struct pvr_context *ctx, int err)
{
	bool ret = false;

	if (ctx->type == DRM_PVR_CTX_TYPE_RENDER) {
		struct pvr_context_render *ctx_render = to_pvr_context_render(ctx);

		ret = pvr_fence_context_fail_fences(&ctx_render->ctx_geom.cccb.pvr_fence_context,
						    err);
		ret |= pvr_fence_context_fail_fences(&ctx_render->ctx_frag.cccb.pvr_fence_context,
						     err);
	} else if (ctx->type == DRM_PVR_CTX_TYPE_COMPUTE) {
		struct pvr_context_compute *ctx_compute = to_pvr_context_compute(ctx);

		ret = pvr_fence_context_fail_fences(&ctx_compute->cccb.pvr_fence_context, err);
	} else if (ctx->type == DRM_PVR_CTX_TYPE_TRANSFER_FRAG) {
		struct pvr_context_transfer *ctx_transfer = to_pvr_context_transfer_frag(ctx);

		ret = pvr_fence_context_fail_fences(&ctx_transfer->cccb.pvr_fence_context, err);
	}

	return ret;
}
