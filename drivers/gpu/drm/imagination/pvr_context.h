/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_CONTEXT_H__
#define __PVR_CONTEXT_H__

#include <linux/compiler_attributes.h>
#include <linux/kref.h>
#include <linux/types.h>
#include <linux/xarray.h>
#include <uapi/drm/pvr_drm.h>

#include "pvr_cccb.h"
#include "pvr_device.h"

/* Forward declaration from pvr_gem.h. */
struct pvr_fw_object;

/**
 * struct pvr_context_geom - Geometry render context data
 */
struct pvr_context_geom {
	/**
	 * @ctx_state_obj: FW object representing context register state.
	 */
	struct pvr_fw_object *ctx_state_obj;

	/** @cccb: Client Circular Command Buffer. */
	struct pvr_cccb cccb;
};

/**
 * struct pvr_context_frag - Fragment render context data
 */
struct pvr_context_frag {
	/**
	 * @ctx_state_obj: FW object representing context register state.
	 */
	struct pvr_fw_object *ctx_state_obj;

	/** @cccb: Client Circular Command Buffer. */
	struct pvr_cccb cccb;
};

enum pvr_context_priority {
	PVR_CTX_PRIORITY_LOW = 0,
	PVR_CTX_PRIORITY_MEDIUM,
	PVR_CTX_PRIORITY_HIGH,
};

/**
 * struct pvr_context - Context data
 */
struct pvr_context {
	/** @ref_count: Refcount for context. */
	struct kref ref_count;

	/** @pvr_dev: Pointer to owning device. */
	struct pvr_device *pvr_dev;

	/** @vm_ctx: Pointer to associated VM context. */
	struct pvr_vm_context *vm_ctx;

	/** @type: Type of context. */
	enum drm_pvr_ctx_type type;

	/** @flags: Context flags. */
	u32 flags;

	/** @priority: Context priority*/
	enum pvr_context_priority priority;

	/** @ctx_id: FW context ID. */
	u32 ctx_id;
};

/**
 * struct pvr_context_render - Render context data
 */
struct pvr_context_render {
	/** @base: Base context structure. */
	struct pvr_context base;

	/** @ctx_geom: Geometry context data. */
	struct pvr_context_geom ctx_geom;

	/** @ctx_frag: Fragment context data. */
	struct pvr_context_frag ctx_frag;

	/** @fw_obj: FW object representing FW-side context data. */
	struct pvr_fw_object *fw_obj;
};

/**
 * struct pvr_context_compute - Compute context data
 */
struct pvr_context_compute {
	/** @base: Base context structure. */
	struct pvr_context base;

	/** @fw_obj: FW object representing FW-side context data. */
	struct pvr_fw_object *fw_obj;

	/**
	 * @ctx_state_obj: FW object representing context register state.
	 */
	struct pvr_fw_object *ctx_state_obj;

	/** @cccb: Client Circular Command Buffer. */
	struct pvr_cccb cccb;
};

/**
 * struct pvr_context_transfer - Transfer context data
 */
struct pvr_context_transfer {
	/** @base: Base context structure. */
	struct pvr_context base;

	/** @fw_obj: FW object representing FW-side context data. */
	struct pvr_fw_object *fw_obj;

	/**
	 * @ctx_state_obj: FW object representing context register state.
	 */
	struct pvr_fw_object *ctx_state_obj;

	/** @cccb: Client Circular Command Buffer. */
	struct pvr_cccb cccb;
};

struct pvr_context *
pvr_create_render_context(struct pvr_file *pvr_file,
			  struct drm_pvr_ioctl_create_context_args *args,
			  u32 handle);
struct pvr_context *
pvr_create_compute_context(struct pvr_file *pvr_file,
			   struct drm_pvr_ioctl_create_context_args *args,
			   u32 handle);
struct pvr_context *
pvr_create_transfer_context(struct pvr_file *pvr_file,
			    struct drm_pvr_ioctl_create_context_args *args,
			    u32 handle);

static __always_inline struct pvr_context *
from_pvr_context_render(struct pvr_context_render *ctx_render)
{
	return &ctx_render->base;
};

static __always_inline struct pvr_context_render *
to_pvr_context_render(struct pvr_context *ctx)
{
	if (ctx->type != DRM_PVR_CTX_TYPE_RENDER)
		return NULL;

	return container_of(ctx, struct pvr_context_render, base);
}

static __always_inline struct pvr_context *
from_pvr_context_compute(struct pvr_context_compute *ctx_context)
{
	return &ctx_context->base;
};

static __always_inline struct pvr_context_compute *
to_pvr_context_compute(struct pvr_context *ctx)
{
	if (ctx->type != DRM_PVR_CTX_TYPE_COMPUTE)
		return NULL;

	return container_of(ctx, struct pvr_context_compute, base);
}

static __always_inline struct pvr_context *
from_pvr_context_transfer(struct pvr_context_transfer *ctx_context)
{
	return &ctx_context->base;
};

static __always_inline struct pvr_context_transfer *
to_pvr_context_transfer_frag(struct pvr_context *ctx)
{
	if (ctx->type != DRM_PVR_CTX_TYPE_TRANSFER_FRAG)
		return NULL;

	return container_of(ctx, struct pvr_context_transfer, base);
}

/**
 * pvr_context_lookup() - Lookup context pointer from handle and file.
 * @pvr_file: Pointer to pvr_file structure.
 * @handle: Context handle.
 *
 * Takes reference on context. Call pvr_context_put() to release.
 *
 * Return:
 *  * The requested context on success, or
 *  * %NULL on failure (context does not exist, or does not belong to @pvr_file).
 */
static __always_inline struct pvr_context *
pvr_context_lookup(struct pvr_file *pvr_file, u32 handle)
{
	struct pvr_context *ctx = xa_load(&pvr_file->ctx_handles, handle);

	if (ctx) {
		kref_get(&ctx->ref_count);

		return ctx;
	}

	return NULL;
}

/**
 * pvr_context_get() - Take additional reference on context.
 * @ctx: Context pointer.
 *
 * Call pvr_context_put() to release.
 *
 * Returns:
 *  * The requested context on success, or
 *  * %NULL if no context pointer passed.
 */
static __always_inline struct pvr_context *
pvr_context_get(struct pvr_context *ctx)
{
	if (ctx)
		kref_get(&ctx->ref_count);

	return ctx;
}

void pvr_context_put(struct pvr_context *ctx);

int pvr_context_destroy(struct pvr_file *pvr_file, u32 handle);

int pvr_context_wait_idle(struct pvr_context *ctx, u32 timeout);

bool pvr_context_fail_fences(struct pvr_context *ctx, int err);

void pvr_destroy_contexts_for_file(struct pvr_file *pvr_file);

#endif /* __PVR_CONTEXT_H__ */
