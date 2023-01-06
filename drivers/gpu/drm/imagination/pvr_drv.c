// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_context.h"
#include "pvr_debugfs.h"
#include "pvr_device.h"
#include "pvr_drv.h"
#include "pvr_free_list.h"
#include "pvr_fw.h"
#include "pvr_gem.h"
#include "pvr_hwrt.h"
#include "pvr_job.h"
#include "pvr_object.h"
#include "pvr_power.h"
#include "pvr_rogue_fwif_client.h"
#include "pvr_rogue_fwif_shared.h"

#include <uapi/drm/pvr_drm.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/xarray.h>

/**
 * DOC: PowerVR Graphics Driver
 *
 * This driver supports the following PowerVR graphics cores from Imagination
 * Technologies:
 *
 * * GX6250 (found in MediaTek MT8173)
 * * AXE-1-16M (found in Texas Instruments AM62)
 */

/**
 * pvr_ioctl_create_bo() - IOCTL to create a GEM buffer object.
 * @drm_dev: [IN] Target DRM device.
 * @raw_args: [IN/OUT] Arguments passed to this IOCTL. This must be of type
 * &struct drm_pvr_ioctl_create_bo_args.
 * @file: [IN] DRM file-private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_CREATE_BO.
 *
 * Return:
 *  * 0 on success,
 *  * -%EINVAL if the value of &drm_pvr_ioctl_create_bo_args.size is zero
 *    or wider than &typedef size_t,
 *  * -%EINVAL if any bits in &drm_pvr_ioctl_create_bo_args.flags that are
 *    reserved or undefined are set,
 *  * -%EINVAL if any padding fields in &drm_pvr_ioctl_create_bo_args are not
 *    zero,
 *  * Any error encountered while creating the object (see
 *    pvr_gem_object_create()), or
 *  * Any error encountered while transferring ownership of the object into a
 *    userspace-accessible handle (see pvr_gem_object_into_handle()).
 */
int
pvr_ioctl_create_bo(struct drm_device *drm_dev, void *raw_args,
		    struct drm_file *file)
{
	struct drm_pvr_ioctl_create_bo_args *args = raw_args;
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	struct pvr_file *pvr_file = to_pvr_file(file);

	struct pvr_gem_object *pvr_obj;
	size_t sanitized_size;
	size_t real_size;

	int err;

	/* All padding fields must be zeroed. */
	if (args->_padding_c != 0)
		return -EINVAL;

	/*
	 * On 64-bit platforms (our primary target), size_t is a u64. However,
	 * on other architectures we have to check for overflow when casting
	 * down to size_t from u64.
	 *
	 * We also disallow zero-sized allocations, and reserved (kernel-only)
	 * flags.
	 */
	if (args->size > SIZE_MAX || args->size == 0 ||
	    args->flags & PVR_BO_RESERVED_MASK) {
		return -EINVAL;
	}

	sanitized_size = (size_t)args->size;

	/*
	 * Create a buffer object and transfer ownership to a userspace-
	 * accessible handle.
	 */
	pvr_obj = pvr_gem_object_create(pvr_dev, sanitized_size, args->flags);
	if (IS_ERR(pvr_obj)) {
		err = PTR_ERR(pvr_obj);
		goto err_out;
	}

	/*
	 * Store the actual size of the created buffer object. We can't fetch
	 * this after this point because we will no longer have a reference to
	 * &pvr_obj.
	 */
	real_size = pvr_gem_object_size(pvr_obj);

	/* This function will not modify &args->handle unless it succeeds. */
	err = pvr_gem_object_into_handle(pvr_obj, pvr_file, &args->handle);
	if (err)
		goto err_destroy_obj;

	/*
	 * Now write the real size back to the args struct, after no further
	 * errors can occur.
	 */
	args->size = real_size;

	return 0;

err_destroy_obj:
	/*
	 * GEM objects are refcounted, so there is no explicit destructor
	 * function. Instead, we release the singular reference we currently
	 * hold on the object and let GEM take care of the rest.
	 */
	pvr_gem_object_put(pvr_obj);

err_out:
	return err;
}

/**
 * pvr_ioctl_get_bo_mmap_offset() - IOCTL to generate a "fake" offset to be
 * used when calling mmap() from userspace to map the given GEM buffer object
 * @drm_dev: [IN] DRM device (unused).
 * @raw_args: [IN/OUT] Arguments passed to this IOCTL. This must be of type
 *                     &struct drm_pvr_ioctl_get_bo_mmap_offset_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET.
 *
 * This IOCTL does *not* perform an mmap. See the docs on
 * &struct drm_pvr_ioctl_get_bo_mmap_offset_args for details.
 *
 * Return:
 *  * 0 on success,
 *  * -%ENOENT if the handle does not reference a valid GEM buffer object,
 *  * -%EINVAL if any padding fields in &struct
 *    drm_pvr_ioctl_get_bo_mmap_offset_args are not zero, or
 *  * Any error returned by drm_gem_create_mmap_offset().
 */
int
pvr_ioctl_get_bo_mmap_offset(__always_unused struct drm_device *drm_dev,
			     void *raw_args, struct drm_file *file)
{
	struct drm_pvr_ioctl_get_bo_mmap_offset_args *args = raw_args;
	struct pvr_file *pvr_file = to_pvr_file(file);

	struct pvr_gem_object *pvr_obj;
	struct drm_gem_object *gem_obj;
	int ret;

	/* All padding fields must be zeroed. */
	if (args->_padding_4 != 0)
		return -EINVAL;

	/*
	 * Obtain a kernel reference to the buffer object. This reference is
	 * counted and must be manually dropped before returning. If a buffer
	 * object cannot be found for the specified handle, return -%ENOENT (No
	 * such file or directory).
	 */
	pvr_obj = pvr_gem_object_from_handle(pvr_file, args->handle);
	if (!pvr_obj)
		return -ENOENT;

	gem_obj = from_pvr_gem_object(pvr_obj);

	/*
	 * Allocate a fake offset which can be used in userspace calls to mmap
	 * on the DRM device file. If this fails, return the error code. This
	 * operation is idempotent.
	 */
	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret != 0) {
		/* Drop our reference to the buffer object. */
		drm_gem_object_put(gem_obj);
		return ret;
	}

	/*
	 * Read out the fake offset allocated by the earlier call to
	 * drm_gem_create_mmap_offset.
	 */
	args->offset = drm_vma_node_offset_addr(&gem_obj->vma_node);

	/* Drop our reference to the buffer object. */
	pvr_gem_object_put(pvr_obj);

	return 0;
}

static __always_inline u64
pvr_fw_version_packed(u32 major, u32 minor)
{
	return ((u64)major << 32) | minor;
}

static u32
rogue_get_common_store_partition_space_size(struct pvr_device *pvr_dev)
{
	u32 max_partitions = 0;
	u32 tile_size_x = 0;
	u32 tile_size_y = 0;

	PVR_FEATURE_VALUE(pvr_dev, tile_size_x, &tile_size_x);
	PVR_FEATURE_VALUE(pvr_dev, tile_size_y, &tile_size_y);
	PVR_FEATURE_VALUE(pvr_dev, max_partitions, &max_partitions);

	if (tile_size_x == 16 && tile_size_y == 16) {
		u32 usc_min_output_registers_per_pix = 0;

		PVR_FEATURE_VALUE(pvr_dev, usc_min_output_registers_per_pix,
				  &usc_min_output_registers_per_pix);

		return tile_size_x * tile_size_y * max_partitions *
		       usc_min_output_registers_per_pix;
	}

	return max_partitions * 1024;
}

static u32
rogue_get_common_store_alloc_region_size(struct pvr_device *pvr_dev)
{
	u32 common_store_size_in_dwords = 512 * 4 * 4;
	u32 alloc_region_size;

	PVR_FEATURE_VALUE(pvr_dev, common_store_size_in_dwords, &common_store_size_in_dwords);

	alloc_region_size = common_store_size_in_dwords - (256U * 4U) -
			    rogue_get_common_store_partition_space_size(pvr_dev);

	if (PVR_HAS_QUIRK(pvr_dev, 44079)) {
		u32 common_store_split_point = (768U * 4U * 4U);

		return min(common_store_split_point - (256U * 4U), alloc_region_size);
	}

	return alloc_region_size;
}

static inline u32
rogue_get_num_phantoms(struct pvr_device *pvr_dev)
{
	u32 num_clusters = 1;

	PVR_FEATURE_VALUE(pvr_dev, num_clusters, &num_clusters);

	return ROGUE_REQ_NUM_PHANTOMS(num_clusters);
}

static inline u32
rogue_get_max_coeffs(struct pvr_device *pvr_dev)
{
	u32 max_coeff_additional_portion = ROGUE_MAX_VERTEX_SHARED_REGISTERS;
	u32 pending_allocation_shared_regs = 2U * 1024U;
	u32 pending_allocation_coeff_regs = 0U;
	u32 num_phantoms = rogue_get_num_phantoms(pvr_dev);
	u32 tiles_in_flight = 0;
	u32 max_coeff_pixel_portion;

	PVR_FEATURE_VALUE(pvr_dev, isp_max_tiles_in_flight, &tiles_in_flight);
	max_coeff_pixel_portion = DIV_ROUND_UP(tiles_in_flight, num_phantoms);
	max_coeff_pixel_portion *= ROGUE_MAX_PIXEL_SHARED_REGISTERS;

	/*
	 * Compute tasks on cores with BRN48492 and without compute overlap may lock
	 * up without two additional lines of coeffs.
	 */
	if (PVR_HAS_QUIRK(pvr_dev, 48492) && !PVR_HAS_FEATURE(pvr_dev, compute_overlap))
		pending_allocation_coeff_regs = 2U * 1024U;

	if (PVR_HAS_ENHANCEMENT(pvr_dev, 38748))
		pending_allocation_shared_regs = 0;

	if (PVR_HAS_ENHANCEMENT(pvr_dev, 38020))
		max_coeff_additional_portion += ROGUE_MAX_COMPUTE_SHARED_REGISTERS;

	return rogue_get_common_store_alloc_region_size(pvr_dev) + pending_allocation_coeff_regs -
		(max_coeff_pixel_portion + max_coeff_additional_portion +
		 pending_allocation_shared_regs);
}

static inline u32
rogue_get_cdm_max_local_mem_size_regs(struct pvr_device *pvr_dev)
{
	u32 available_coeffs_in_dwords = rogue_get_max_coeffs(pvr_dev);

	if (PVR_HAS_QUIRK(pvr_dev, 48492) && PVR_HAS_FEATURE(pvr_dev, roguexe) &&
	    !PVR_HAS_FEATURE(pvr_dev, compute_overlap)) {
		/* Driver must not use the 2 reserved lines. */
		available_coeffs_in_dwords -= ROGUE_CSRM_LINE_SIZE_IN_DWORDS * 2;
	}

	/*
	 * The maximum amount of local memory available to a kernel is the minimum
	 * of the total number of coefficient registers available and the max common
	 * store allocation size which can be made by the CDM.
	 *
	 * If any coeff lines are reserved for tessellation or pixel then we need to
	 * subtract those too.
	 */
	return min(available_coeffs_in_dwords, (u32)ROGUE_MAX_PER_KERNEL_LOCAL_MEM_SIZE_REGS);
}

/**
 * pvr_get_quirks0() - Get first word of quirks mask for the current GPU & FW
 * @pvr_dev: Device pointer
 *
 * Returns:
 *  * Mask of quirks (combination of %DRM_PVR_QUIRKS0_HAS_BRN_*).
 */
static __always_inline u64
pvr_get_quirks0(struct pvr_device *pvr_dev)
{
	u64 value = 0;

#define PVR_SET_QUIRKS0_FLAG(pvr_dev, quirk)                                        \
	do {                                                                        \
		if (pvr_device_has_uapi_quirk(pvr_dev, DRM_PVR_QUIRK_BRN ## quirk)) \
			value |= DRM_PVR_QUIRK_MASK(DRM_PVR_QUIRK_BRN ## quirk);    \
	} while (0)

	PVR_SET_QUIRKS0_FLAG(pvr_dev, 47217);
	PVR_SET_QUIRKS0_FLAG(pvr_dev, 48545);
	PVR_SET_QUIRKS0_FLAG(pvr_dev, 49927);
	PVR_SET_QUIRKS0_FLAG(pvr_dev, 51764);
	PVR_SET_QUIRKS0_FLAG(pvr_dev, 62269);

#undef PVR_SET_QUIRKS0_FLAG

	return value;
}

/**
 * pvr_get_enhancements0() - Get first word of enhancements mask for the current
 *                           GPU & FW
 * @pvr_dev: Device pointer
 *
 * Returns:
 *  * Mask of enhancements (combination of %DRM_PVR_ENHANCEMENTS0_HAS_ERN_*).
 */
static __always_inline u64
pvr_get_enhancements0(struct pvr_device *pvr_dev)
{
	u64 value = 0;

#define PVR_SET_ENHANCEMENTS0_FLAG(pvr_dev, enhancement)                                              \
	do {                                                                                          \
		if (pvr_device_has_uapi_enhancement(pvr_dev, DRM_PVR_ENHANCEMENT_ERN ## enhancement)) \
			value |= DRM_PVR_ENHANCEMENT_MASK(DRM_PVR_ENHANCEMENT_ERN ## enhancement);    \
	} while (0)

	PVR_SET_ENHANCEMENTS0_FLAG(pvr_dev, 35421);
	PVR_SET_ENHANCEMENTS0_FLAG(pvr_dev, 42064);

#undef PVR_SET_ENHANCEMENTS0_FLAG

	return value;
}

/**
 * pvr_get_quirks_musthave0() - Get first word of must have quirks mask for the current GPU & FW
 * @pvr_dev: Device pointer
 *
 * Returns:
 *  * Mask of must have quirks (combination of %DRM_PVR_QUIRKS0_HAS_BRN_*).
 */
static __always_inline u64
pvr_get_quirks_musthave0(struct pvr_device *pvr_dev)
{
	u64 value = 0;

#define PVR_SET_QUIRKS_MUSTHAVE0_FLAG(pvr_dev, quirk)                               \
	do {                                                                        \
		if (pvr_device_has_uapi_quirk(pvr_dev, DRM_PVR_QUIRK_BRN ## quirk)) \
			value |= DRM_PVR_QUIRK_MASK(DRM_PVR_QUIRK_BRN ## quirk);    \
	} while (0)

	PVR_SET_QUIRKS_MUSTHAVE0_FLAG(pvr_dev, 47217);
	PVR_SET_QUIRKS_MUSTHAVE0_FLAG(pvr_dev, 49927);
	PVR_SET_QUIRKS_MUSTHAVE0_FLAG(pvr_dev, 62269);

#undef PVR_SET_QUIRKS_MUSTHAVE0_FLAG

	return value;
}

/**
 * pvr_ioctl_get_param() - IOCTL to get information about a device
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN/OUT] Arguments passed to this IOCTL. This must be of type
 *                     &struct drm_pvr_ioctl_get_param_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_GET_PARAM.
 *
 * Return:
 *  * 0 on success,
 *  * -%EINVAL if the value of &struct drm_pvr_ioctl_get_param_args.param is
 *    not one of those in &enum drm_pvr_param or is %DRM_PVR_PARAM_INVALID, or
 *  * -%EINVAL if any padding fields in &struct drm_pvr_ioctl_get_param_args
 *    are not zero.
 */
int
pvr_ioctl_get_param(struct drm_device *drm_dev, void *raw_args,
		    struct drm_file *file)
{
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	struct drm_pvr_ioctl_get_param_args *args = raw_args;
	u64 value;

	/* All padding fields must be zeroed. */
	if (args->_padding_4 != 0)
		return -EINVAL;

	switch (args->param) {
	case DRM_PVR_PARAM_GPU_ID:
		value = pvr_gpu_id_to_packed_bvnc(&pvr_dev->gpu_id);
		break;
	case DRM_PVR_PARAM_HWRT_NUM_GEOMDATAS:
		value = ROGUE_FWIF_NUM_GEOMDATAS;
		break;
	case DRM_PVR_PARAM_HWRT_NUM_RTDATAS:
		value = ROGUE_FWIF_NUM_RTDATAS;
		break;
	case DRM_PVR_PARAM_HWRT_NUM_FREELISTS:
		value = ROGUE_FWIF_NUM_RTDATA_FREELISTS;
		break;
	case DRM_PVR_PARAM_FW_VERSION:
		value = pvr_fw_version_packed(pvr_dev->fw_version.major, pvr_dev->fw_version.minor);
		break;
	case DRM_PVR_PARAM_QUIRKS0:
		value = pvr_get_quirks0(pvr_dev);
		break;
	case DRM_PVR_PARAM_QUIRKS_MUSTHAVE0:
		value = pvr_get_quirks_musthave0(pvr_dev);
		break;
	case DRM_PVR_PARAM_ENHANCEMENTS0:
		value = pvr_get_enhancements0(pvr_dev);
		break;
	case DRM_PVR_PARAM_FREE_LIST_MIN_PAGES:
		value = pvr_get_free_list_min_pages(pvr_dev);
		break;
	case DRM_PVR_PARAM_FREE_LIST_MAX_PAGES:
		value = ROGUE_PM_MAX_FREELIST_SIZE / ROGUE_PM_PAGE_SIZE;
		break;
	case DRM_PVR_PARAM_COMMON_STORE_ALLOC_REGION_SIZE:
		value = rogue_get_common_store_alloc_region_size(pvr_dev);
		break;
	case DRM_PVR_PARAM_COMMON_STORE_PARTITION_SPACE_SIZE:
		value = rogue_get_common_store_partition_space_size(pvr_dev);
		break;
	case DRM_PVR_PARAM_NUM_PHANTOMS:
		value = rogue_get_num_phantoms(pvr_dev);
		break;
	case DRM_PVR_PARAM_MAX_COEFFS:
		value = rogue_get_max_coeffs(pvr_dev);
		break;
	case DRM_PVR_PARAM_CDM_MAX_LOCAL_MEM_SIZE_REGS:
		value = rogue_get_cdm_max_local_mem_size_regs(pvr_dev);
		break;
	case DRM_PVR_PARAM_NUM_HEAPS:
		value = pvr_get_num_heaps(pvr_dev);
		break;
	case DRM_PVR_PARAM_INVALID:
	default:
		return -EINVAL;
	}

	args->value = value;

	return 0;
}

/**
 * pvr_ioctl_create_context() - IOCTL to create a context
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN/OUT] Arguments passed to this IOCTL. This must be of type
 *                     &struct drm_pvr_ioctl_create_context_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_CREATE_CONTEXT.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EINVAL if provided arguments are invalid, or
 *  * -%EFAULT if arguments can't be copied from userspace, or
 *  * Any error returned by pvr_create_render_context().
 */
int
pvr_ioctl_create_context(struct drm_device *drm_dev, void *raw_args,
			 struct drm_file *file)
{
	struct drm_pvr_ioctl_create_context_args *args = raw_args;
	struct pvr_file *pvr_file = file->driver_priv;
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct pvr_context *ctx = NULL;
	u32 handle;
	void *old;
	int err;
	u32 id;

	if (args->flags || args->_padding_1c) {
		/* Context creation flags are currently unused and must be zero. */
		err = -EINVAL;
		goto err_out;
	}

	/*
	 * Allocate global ID for firmware. We will update this with the context once it is created.
	 */
	err = xa_alloc(&pvr_dev->ctx_ids, &id, NULL, xa_limit_32b,
		       GFP_KERNEL);
	if (err < 0)
		goto err_out;

	/*
	 * Allocate context handle for userspace. We will update this with the context once it
	 * is created.
	 */
	err = xa_alloc(&pvr_file->ctx_handles, &handle, NULL, xa_limit_32b,
		       GFP_KERNEL);
	if (err < 0)
		goto err_id_xa_erase;

	switch (args->type) {
	case DRM_PVR_CTX_TYPE_RENDER: {
		ctx = pvr_create_render_context(pvr_file, args, id);
		break;
	}

	case DRM_PVR_CTX_TYPE_COMPUTE: {
		ctx = pvr_create_compute_context(pvr_file, args, id);
		break;
	}

	case DRM_PVR_CTX_TYPE_TRANSFER_FRAG: {
		ctx = pvr_create_transfer_context(pvr_file, args, id);
		break;
	}

	default:
		ctx = ERR_PTR(-EINVAL);
		break;
	}

	if (IS_ERR(ctx)) {
		err = PTR_ERR(ctx);
		goto err_handle_xa_erase;
	}

	old = xa_store(&pvr_dev->ctx_ids, id, ctx, GFP_KERNEL);
	if (xa_is_err(old)) {
		err = xa_err(old);
		goto err_context_destroy;
	}

	old = xa_store(&pvr_file->ctx_handles, handle, ctx, GFP_KERNEL);
	if (xa_is_err(old)) {
		err = xa_err(old);
		goto err_context_destroy;
	}

	args->handle = handle;

	return 0;

err_context_destroy:
	pvr_context_destroy(pvr_file, handle);

err_handle_xa_erase:
	xa_erase(&pvr_file->ctx_handles, handle);

err_id_xa_erase:
	xa_erase(&pvr_dev->ctx_ids, id);

err_out:
	return err;
}

/**
 * pvr_ioctl_destroy_context() - IOCTL to destroy a context
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN/OUT] Arguments passed to this IOCTL. This must be of type
 *                     &struct drm_pvr_ioctl_destroy_context_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_DESTROY_CONTEXT.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EINVAL if context not in context list.
 */
int
pvr_ioctl_destroy_context(struct drm_device *drm_dev, void *raw_args,
			  struct drm_file *file)
{
	struct drm_pvr_ioctl_destroy_context_args *args = raw_args;
	struct pvr_file *pvr_file = file->driver_priv;

	if (args->_padding_4)
		return -EINVAL;

	return pvr_context_destroy(pvr_file, args->handle);
}

/**
 * pvr_ioctl_create_free_list() - IOCTL to create a free list
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN/OUT] Arguments passed to this IOCTL. This must be of type
 *                     &struct drm_pvr_ioctl_create_free_list_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_CREATE_FREE_LIST.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_free_list_create().
 */
int
pvr_ioctl_create_free_list(struct drm_device *drm_dev, void *raw_args,
			   struct drm_file *file)
{
	struct drm_pvr_ioctl_create_free_list_args *args = raw_args;
	struct pvr_file *pvr_file = to_pvr_file(file);
	struct pvr_free_list *free_list;
	int err;

	if (args->_padding_1c)
		return -EINVAL;

	free_list = pvr_free_list_create(pvr_file, args);
	if (IS_ERR(free_list)) {
		err = PTR_ERR(free_list);
		goto err_out;
	}

	/* Allocate object handle for userspace. */
	err = xa_alloc(&pvr_file->obj_handles,
		       &args->handle,
		       &free_list->base,
		       xa_limit_32b,
		       GFP_KERNEL);
	if (err < 0)
		goto err_cleanup;

	return 0;

err_cleanup:
	pvr_free_list_put(free_list);

err_out:
	return err;
}

/**
 * pvr_ioctl_destroy_free_list() - IOCTL to destroy a free list
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN] Arguments passed to this IOCTL. This must be of type
 *                 &struct drm_pvr_ioctl_destroy_free_list_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_DESTROY_FREE_LIST.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EINVAL if free list not in object list.
 */
int
pvr_ioctl_destroy_free_list(struct drm_device *drm_dev, void *raw_args,
			    struct drm_file *file)
{
	struct drm_pvr_ioctl_destroy_free_list_args *args = raw_args;
	struct pvr_file *pvr_file = to_pvr_file(file);

	if (args->_padding_4)
		return -EINVAL;

	return pvr_object_destroy(pvr_file, args->handle, PVR_OBJECT_TYPE_FREE_LIST);
}

/**
 * pvr_ioctl_create_hwrt_dataset() - IOCTL to create a HWRT dataset
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN/OUT] Arguments passed to this IOCTL. This must be of type
 *                     &struct drm_pvr_ioctl_create_hwrt_dataset_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_CREATE_HWRT_DATASET.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_hwrt_dataset_create().
 */
int
pvr_ioctl_create_hwrt_dataset(struct drm_device *drm_dev, void *raw_args,
			      struct drm_file *file)
{
	struct drm_pvr_ioctl_create_hwrt_dataset_args *args = raw_args;
	struct pvr_file *pvr_file = to_pvr_file(file);
	struct pvr_hwrt_dataset *hwrt;
	int err;

	hwrt = pvr_hwrt_dataset_create(pvr_file, args);
	if (IS_ERR(hwrt)) {
		err = PTR_ERR(hwrt);
		goto err_out;
	}

	/* Allocate object handle for userspace. */
	err = xa_alloc(&pvr_file->obj_handles,
		       &args->handle,
		       &hwrt->base,
		       xa_limit_32b,
		       GFP_KERNEL);
	if (err < 0)
		goto err_cleanup;

	return 0;

err_cleanup:
	pvr_hwrt_dataset_put(hwrt);

err_out:
	return err;
}

/**
 * pvr_ioctl_destroy_hwrt_dataset() - IOCTL to destroy a HWRT dataset
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN] Arguments passed to this IOCTL. This must be of type
 *                 &struct drm_pvr_ioctl_destroy_hwrt_dataset_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_DESTROY_HWRT_DATASET.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EINVAL if HWRT dataset not in object list.
 */
int
pvr_ioctl_destroy_hwrt_dataset(struct drm_device *drm_dev, void *raw_args,
			       struct drm_file *file)
{
	struct drm_pvr_ioctl_destroy_hwrt_dataset_args *args = raw_args;
	struct pvr_file *pvr_file = to_pvr_file(file);

	if (args->_padding_4)
		return -EINVAL;

	return pvr_object_destroy(pvr_file, args->handle, PVR_OBJECT_TYPE_HWRT_DATASET);
}

/**
 * pvr_ioctl_get_heap_info() - IOCTL to get information on device heaps
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN] Arguments passed to this IOCTL. This must be of type
 *                 &struct drm_pvr_ioctl_get_heap_info.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_GET_HEAP_INFO.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EFAULT on failure to write to user buffer.
 */
int
pvr_ioctl_get_heap_info(struct drm_device *drm_dev, void *raw_args,
			struct drm_file *file)
{
	struct drm_pvr_ioctl_get_heap_info_args *args = raw_args;

	if (args->_padding_4 != 0)
		return -EINVAL;

	return pvr_get_heap_info(to_pvr_device(drm_dev), args);
}

/**
 * pvr_ioctl_vm_map() - IOCTL to map buffer to GPU address space.
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN] Arguments passed to this IOCTL. This must be of type
 *                 &struct drm_pvr_ioctl_vm_map_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_VM_MAP.
 *
 * Return:
 *  * 0 on success,
 *  * -%EINVAL if &drm_pvr_ioctl_vm_op_map_args.flags is not zero,
 *  * -%EINVAL if the bounds specified by &drm_pvr_ioctl_vm_op_map_args.offset
 *    and &drm_pvr_ioctl_vm_op_map_args.size are not valid or do not fall
 *    within the buffer object specified by
 *    &drm_pvr_ioctl_vm_op_map_args.handle,
 *  * -%EINVAL if the bounds specified by
 *    &drm_pvr_ioctl_vm_op_map_args.device_addr and
 *    &drm_pvr_ioctl_vm_op_map_args.size do not form a valid device-virtual
 *    address range which falls entirely within a single heap, or
 *  * -%ENOENT if &drm_pvr_ioctl_vm_op_map_args.handle does not refer to a
 *    valid PowerVR buffer object.
 */
static int
pvr_ioctl_vm_map(struct drm_device *drm_dev, void *raw_args,
		 struct drm_file *file)
{
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	struct drm_pvr_ioctl_vm_map_args *args = raw_args;
	struct pvr_file *pvr_file = to_pvr_file(file);
	struct pvr_vm_context *vm_ctx = pvr_file->user_vm_ctx;

	struct pvr_gem_object *pvr_obj;
	size_t pvr_obj_size;

	u64 offset_plus_size;
	int err;

	/* Initial validation of args. */
	if (args->flags != 0 ||
	    check_add_overflow(args->offset, args->size, &offset_plus_size) ||
	    !pvr_find_heap_containing(pvr_dev, args->device_addr, args->size)) {
		err = -EINVAL;
		goto err_out;
	}

	pvr_obj = pvr_gem_object_from_handle(pvr_file, args->handle);
	if (!pvr_obj) {
		err = -ENOENT;
		goto err_out;
	}

	pvr_obj_size = pvr_gem_object_size(pvr_obj);

	/*
	 * Validate offset and size args. The alignment of these will be
	 * checked when mapping; for now just check that they're within valid
	 * bounds
	 */
	if (args->offset >= pvr_obj_size || offset_plus_size > pvr_obj_size) {
		err = -EINVAL;
		goto err_put_pvr_object;
	}

	/*
	 * If the caller has specified that the entire object should be mapped,
	 * use the more efficient pvr_vm_map().
	 */
	if (args->offset == 0 && args->size == pvr_obj_size) {
		err = pvr_vm_map(vm_ctx, pvr_obj, args->device_addr);
	} else {
		err = pvr_vm_map_partial(vm_ctx, pvr_obj, args->offset,
					 args->device_addr, args->size);
	}
	if (err)
		goto err_put_pvr_object;

	/*
	 * In order to set up the mapping, we needed a reference to &pvr_obj.
	 * However, pvr_vm_map() obtains and stores its own reference, so we
	 * must release ours before returning.
	 */
	err = 0;
	goto err_put_pvr_object;

err_put_pvr_object:
	pvr_gem_object_put(pvr_obj);

err_out:
	return err;
}

/**
 * pvr_ioctl_vm_unmap() - IOCTL to unmap buffer from GPU address space.
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN] Arguments passed to this IOCTL. This must be of type
 *                 &struct drm_pvr_ioctl_vm_unmap_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_VM_UNMAP.
 *
 * Return:
 *  * 0 on success,
 *  * -%EINVAL if &drm_pvr_ioctl_vm_op_unmap_args.device_addr is not a valid
 *    device page-aligned device-virtual address, or
 *  * -%ENOENT if there is currently no PowerVR buffer object mapped at
 *    &drm_pvr_ioctl_vm_op_unmap_args.device_addr.
 */
static int
pvr_ioctl_vm_unmap(struct drm_device *drm_dev, void *raw_args,
		   struct drm_file *file)
{
	struct drm_pvr_ioctl_vm_unmap_args *args = raw_args;
	struct pvr_file *pvr_file = to_pvr_file(file);

	return pvr_vm_unmap(pvr_file->user_vm_ctx, args->device_addr);
}

/*
 * pvr_ioctl_submit_job() - IOCTL to submit a job to the GPU
 * @drm_dev: [IN] DRM device.
 * @raw_args: [IN] Arguments passed to this IOCTL. This must be of type
 *                 &struct drm_pvr_ioctl_submit_job_args.
 * @file: [IN] DRM file private data.
 *
 * Called from userspace with %DRM_IOCTL_PVR_SUBMIT_JOB.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EINVAL if arguments are invalid.
 */
int
pvr_ioctl_submit_job(struct drm_device *drm_dev, void *raw_args,
		     struct drm_file *file)
{
	struct drm_pvr_ioctl_submit_job_args *args = raw_args;
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	struct pvr_file *pvr_file = to_pvr_file(file);

	return pvr_submit_job(pvr_dev, pvr_file, args);
}

#define DRM_PVR_IOCTL(_name, _func, _flags) \
	DRM_IOCTL_DEF_DRV(PVR_##_name, pvr_ioctl_##_func, _flags)

/* clang-format off */

static const struct drm_ioctl_desc pvr_drm_driver_ioctls[] = {
	DRM_PVR_IOCTL(CREATE_BO, create_bo, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(GET_BO_MMAP_OFFSET, get_bo_mmap_offset, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(GET_PARAM, get_param, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(CREATE_CONTEXT, create_context, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(DESTROY_CONTEXT, destroy_context, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(CREATE_FREE_LIST, create_free_list, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(DESTROY_FREE_LIST, destroy_free_list, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(CREATE_HWRT_DATASET, create_hwrt_dataset, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(DESTROY_HWRT_DATASET, destroy_hwrt_dataset, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(GET_HEAP_INFO, get_heap_info, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(VM_MAP, vm_map, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(VM_UNMAP, vm_unmap, DRM_RENDER_ALLOW),
	DRM_PVR_IOCTL(SUBMIT_JOB, submit_job, DRM_RENDER_ALLOW),
};

/* clang-format on */

#undef DRM_PVR_IOCTL

/**
 * pvr_drm_driver_open() - Driver callback when a new &struct drm_file is opened
 * @drm_dev: [IN] DRM device.
 * @file: [IN] DRM file private data.
 *
 * Allocates powervr-specific file private data (&struct pvr_file).
 *
 * Registered in &pvr_drm_driver.
 *
 * Return:
 *  * 0 on success,
 *  * -%ENOMEM if the allocation of a &struct ipvr_file fails, or
 *  * Any error returned by pvr_memory_context_init().
 */
static int
pvr_drm_driver_open(struct drm_device *drm_dev, struct drm_file *file)
{
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	struct pvr_file *pvr_file;

	int err;

	pvr_file = kzalloc(sizeof(*pvr_file), GFP_KERNEL);
	if (!pvr_file) {
		err = -ENOMEM;
		goto err_out;
	}

	/*
	 * Store reference to base DRM file private data for use by
	 * from_pvr_file.
	 */
	pvr_file->file = file;

	/*
	 * Store reference to powervr-specific outer device struct in file
	 * private data for convenient access.
	 */
	pvr_file->pvr_dev = pvr_dev;

	/* Initialize the file-scoped memory context. */
	pvr_file->user_vm_ctx = pvr_vm_create_context(pvr_dev, true);
	if (IS_ERR(pvr_file->user_vm_ctx)) {
		err = PTR_ERR(pvr_file->user_vm_ctx);
		goto err_free_file;
	}

	xa_init_flags(&pvr_file->ctx_handles, XA_FLAGS_ALLOC1);
	xa_init_flags(&pvr_file->obj_handles, XA_FLAGS_ALLOC1);

	/*
	 * Store reference to powervr-specific file private data in DRM file
	 * private data.
	 */
	file->driver_priv = pvr_file;

	return 0;

err_free_file:
	kfree(pvr_file);

err_out:
	return err;
}

/**
 * pvr_drm_driver_postclose() - One of the driver callbacks when a &struct
 * drm_file is closed.
 * @drm_dev: [IN] DRM device (unused).
 * @file: [IN] DRM file private data.
 *
 * Frees powervr-specific file private data (&struct pvr_file).
 *
 * Registered in &pvr_drm_driver.
 */
static void
pvr_drm_driver_postclose(__always_unused struct drm_device *drm_dev,
			 struct drm_file *file)
{
	struct pvr_file *pvr_file = to_pvr_file(file);
	struct pvr_context *ctx;
	unsigned long handle;

	/* clang-format off */
	xa_for_each(&pvr_file->ctx_handles, handle, ctx) {
		WARN_ON(pvr_context_wait_idle(ctx, HZ));
		WARN_ON(pvr_context_fail_fences(ctx, -ENODEV));
	}
	/* clang-format on */

	/* Drop references on any remaining objects. */
	pvr_destroy_objects_for_file(pvr_file);

	/* Drop references on any remaining contexts. */
	pvr_destroy_contexts_for_file(pvr_file);

	pvr_vm_context_teardown_mappings(pvr_file->user_vm_ctx, false);

	pvr_vm_context_put(pvr_file->user_vm_ctx);

	kfree(pvr_file);
	file->driver_priv = NULL;
}

DEFINE_DRM_GEM_FOPS(pvr_drm_driver_fops);

static struct drm_driver pvr_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ,
	.open = pvr_drm_driver_open,
	.postclose = pvr_drm_driver_postclose,
	.ioctls = pvr_drm_driver_ioctls,
	.num_ioctls = ARRAY_SIZE(pvr_drm_driver_ioctls),
	.fops = &pvr_drm_driver_fops,
#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = pvr_debugfs_init,
#endif

	.name = PVR_DRIVER_NAME,
	.desc = PVR_DRIVER_DESC,
	.date = PVR_DRIVER_DATE,
	.major = PVR_DRIVER_MAJOR,
	.minor = PVR_DRIVER_MINOR,
	.patchlevel = PVR_DRIVER_PATCHLEVEL,

	/*
	 * These three (four) helper functions implement PRIME buffer sharing
	 * for us. The last is set implicitly when not assigned here. The only
	 * additional requirement to make PRIME work is to call dma_set_mask()
	 * in pvr_probe() to tell DMA that we can read from more than the first
	 * 4GB (32 bits) of memory address space. The subsequent call to
	 * dma_set_max_seg_size() is not strictly required, but prevents some
	 * warnings from appearing when CONFIG_DMA_API_DEBUG_SG is enabled.
	 */
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.gem_prime_import_sg_table = __pvr_gem_prime_import_sg_table,
	/* .gem_prime_import = drm_gem_prime_import, */
};

static int
pvr_probe(struct platform_device *plat_dev)
{
	struct pvr_device *pvr_dev;
	struct drm_device *drm_dev;
	int err;

	pvr_dev = devm_drm_dev_alloc(&plat_dev->dev, &pvr_drm_driver,
				     struct pvr_device, base);
	if (IS_ERR(pvr_dev)) {
		err = IS_ERR(pvr_dev);
		goto err_out;
	}
	drm_dev = &pvr_dev->base;

	platform_set_drvdata(plat_dev, drm_dev);

	pvr_fence_device_init(pvr_dev);

	pm_runtime_enable(&plat_dev->dev);
	pvr_power_init(pvr_dev);

	pvr_dev->vendor.callbacks = of_device_get_match_data(&plat_dev->dev);

	if (pvr_dev->vendor.callbacks && pvr_dev->vendor.callbacks->init) {
		err = pvr_dev->vendor.callbacks->init(pvr_dev);
		if (err)
			goto err_pm_runtime_disable;
	}

	err = pvr_device_init(pvr_dev);
	if (err)
		goto err_vendor_fini;

	err = drm_dev_register(drm_dev, 0);
	if (err)
		goto err_device_fini;

	xa_init_flags(&pvr_dev->ctx_ids, XA_FLAGS_ALLOC1);
	xa_init_flags(&pvr_dev->obj_ids, XA_FLAGS_ALLOC1);
	xa_init_flags(&pvr_dev->job_ids, XA_FLAGS_ALLOC1);

	return 0;

err_device_fini:
	pvr_device_fini(pvr_dev);

err_vendor_fini:
	if (pvr_dev->vendor.callbacks && pvr_dev->vendor.callbacks->fini)
		pvr_dev->vendor.callbacks->fini(pvr_dev);

err_pm_runtime_disable:
	pm_runtime_disable(&plat_dev->dev);

err_out:
	return err;
}

static int
pvr_remove(struct platform_device *plat_dev)
{
	struct drm_device *drm_dev = platform_get_drvdata(plat_dev);
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);

	WARN_ON(!xa_empty(&pvr_dev->job_ids));
	WARN_ON(!xa_empty(&pvr_dev->obj_ids));
	WARN_ON(!xa_empty(&pvr_dev->ctx_ids));

	xa_destroy(&pvr_dev->job_ids);
	xa_destroy(&pvr_dev->obj_ids);
	xa_destroy(&pvr_dev->ctx_ids);

	drm_dev_unregister(drm_dev);
	pvr_device_fini(pvr_dev);
	if (pvr_dev->vendor.callbacks && pvr_dev->vendor.callbacks->fini)
		pvr_dev->vendor.callbacks->fini(pvr_dev);
	pm_runtime_disable(&plat_dev->dev);

	return 0;
}

static const struct of_device_id dt_match[] = {
	{ .compatible = "mediatek,mt8173-gpu", .data = &pvr_mt8173_callbacks },
	{ .compatible = "ti,am62-gpu", .data = NULL },
	{ .compatible = "img,powervr-series6xt", .data = NULL },
	{ .compatible = "img,powervr-seriesaxe", .data = NULL },
	{}
};
MODULE_DEVICE_TABLE(of, dt_match);

static int pvr_device_suspend(struct device *dev)
{
	struct platform_device *plat_dev = to_platform_device(dev);
	struct drm_device *drm_dev = platform_get_drvdata(plat_dev);
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	int err = 0;

	if (pvr_dev->vendor.callbacks &&
	    pvr_dev->vendor.callbacks->power_disable) {
		err = pvr_dev->vendor.callbacks->power_disable(pvr_dev);
		if (err)
			goto err_out;
	}

	clk_disable(pvr_dev->mem_clk);
	clk_disable(pvr_dev->sys_clk);
	clk_disable(pvr_dev->core_clk);

	if (pvr_dev->regulator)
		regulator_disable(pvr_dev->regulator);

err_out:
	return err;
}

static int pvr_device_resume(struct device *dev)
{
	struct platform_device *plat_dev = to_platform_device(dev);
	struct drm_device *drm_dev = platform_get_drvdata(plat_dev);
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	int err;

	if (pvr_dev->regulator) {
		err = regulator_enable(pvr_dev->regulator);
		if (err)
			goto err_out;
	}

	clk_enable(pvr_dev->core_clk);
	clk_enable(pvr_dev->sys_clk);
	clk_enable(pvr_dev->mem_clk);

	if (pvr_dev->vendor.callbacks &&
	    pvr_dev->vendor.callbacks->power_enable) {
		err = pvr_dev->vendor.callbacks->power_enable(pvr_dev);
		if (err)
			goto err_clk_disable;
	}

	return 0;

err_clk_disable:
	clk_disable(pvr_dev->mem_clk);
	clk_disable(pvr_dev->sys_clk);
	clk_disable(pvr_dev->core_clk);

err_out:
	return err;
}

static const struct dev_pm_ops pvr_pm_ops = {
	SET_RUNTIME_PM_OPS(pvr_device_suspend, pvr_device_resume, NULL)
};

static struct platform_driver pvr_driver = {
	.probe = pvr_probe,
	.remove = pvr_remove,
	.driver = {
		.name = PVR_DRIVER_NAME,
		.pm = &pvr_pm_ops,
		.of_match_table = dt_match,
	},
};
module_platform_driver(pvr_driver);

MODULE_AUTHOR("Imagination Technologies Ltd.");
MODULE_DESCRIPTION(PVR_DRIVER_DESC);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_IMPORT_NS(DMA_BUF);
MODULE_FIRMWARE("powervr/rogue_4.40.2.51_v1.fw");
MODULE_FIRMWARE("powervr/rogue_33.15.11.3_v1.fw");
