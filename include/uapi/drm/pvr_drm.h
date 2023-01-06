/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_DRM_H__
#define __PVR_DRM_H__

#include "drm.h"

#include <linux/const.h>
#include <linux/types.h>

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * DOC: IOCTLS
 *
 * The PowerVR IOCTL argument structs have a few limitations in place, in
 * addition to the standard kernel restrictions:
 *
 *  - All members must be type-aligned.
 *  - The overall struct must be padded to 64-bit alignment.
 *  - Explicit padding is almost always required. This takes the form of
 *    &_padding_x members of sufficient size to pad to the next power-of-two
 *    alignment, where x is the offset into the struct in hexadecimal. Arrays
 *    are never used for alignment. Padding fields must be zeroed; this is
 *    always checked.
 *  - Unions may only appear as the last member of a struct.
 *  - Individual union members may grow in the future. The space between the
 *    end of a union member and the end of its containing union is considered
 *    "implicit padding" and must be zeroed. This is always checked.
 */

/* clang-format off */
/**
 * PVR_IOCTL() - Build a PowerVR IOCTL number
 * @_ioctl: An incrementing id for this IOCTL. Added to %DRM_COMMAND_BASE.
 * @_mode: Must be one of DRM_IO{R,W,WR}.
 * @_data: The type of the args struct passed by this IOCTL.
 *
 * The struct referred to by @_data must have a &drm_pvr_ioctl_ prefix and an
 * &_args suffix. They are therefore omitted from @_data.
 *
 * This should only be used to build the constants described below; it should
 * never be used to call an IOCTL directly.
 *
 * Return:
 * An IOCTL number to be passed to ioctl() from userspace.
 */
#define PVR_IOCTL(_ioctl, _mode, _data) \
	_mode(DRM_COMMAND_BASE + (_ioctl), struct drm_pvr_ioctl_##_data##_args)

#define DRM_IOCTL_PVR_CREATE_BO PVR_IOCTL(0x00, DRM_IOWR, create_bo)
#define DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET PVR_IOCTL(0x01, DRM_IOWR, get_bo_mmap_offset)
#define DRM_IOCTL_PVR_GET_PARAM PVR_IOCTL(0x02, DRM_IOWR, get_param)
#define DRM_IOCTL_PVR_CREATE_CONTEXT PVR_IOCTL(0x03, DRM_IOWR, create_context)
#define DRM_IOCTL_PVR_DESTROY_CONTEXT PVR_IOCTL(0x04, DRM_IOW, destroy_context)
#define DRM_IOCTL_PVR_CREATE_FREE_LIST PVR_IOCTL(0x05, DRM_IOWR, create_free_list)
#define DRM_IOCTL_PVR_DESTROY_FREE_LIST PVR_IOCTL(0x06, DRM_IOW, destroy_free_list)
#define DRM_IOCTL_PVR_CREATE_HWRT_DATASET PVR_IOCTL(0x07, DRM_IOWR, create_hwrt_dataset)
#define DRM_IOCTL_PVR_DESTROY_HWRT_DATASET PVR_IOCTL(0x08, DRM_IOW, destroy_hwrt_dataset)
#define DRM_IOCTL_PVR_GET_HEAP_INFO PVR_IOCTL(0x09, DRM_IOWR, get_heap_info)
#define DRM_IOCTL_PVR_VM_MAP PVR_IOCTL(0x0a, DRM_IOW, vm_map)
#define DRM_IOCTL_PVR_VM_UNMAP PVR_IOCTL(0x0b, DRM_IOW, vm_unmap)
#define DRM_IOCTL_PVR_SUBMIT_JOB PVR_IOCTL(0x0c, DRM_IOW, submit_job)
/* clang-format on */

/**
 * DOC: Flags for CREATE_BO
 *
 * The &drm_pvr_ioctl_create_bo_args.flags field is 64 bits wide and consists
 * of three groups of flags: creation, device mapping and CPU mapping.
 *
 * We use "device" to refer to the GPU here because of the ambiguity between
 * CPU and GPU in some fonts.
 *
 * Creation options
 *    These use the prefix ``DRM_PVR_BO_CREATE_``.
 *
 *    :ZEROED: Require the allocated buffer to be zeroed before returning. Note
 *      that this is an active operation, and is never zero cost. Unless it is
 *      explicitly required, this option should not be set.
 *
 * Device mapping options
 *    These use the prefix ``DRM_PVR_BO_DEVICE_``.
 *
 *    :BYPASS_CACHE: There are very few situations where this flag is useful.
 *       By default, the device flushes its memory caches after every job.
 *    :PM_FW_PROTECT: Specify that only the Parameter Manager (PM) and/or
 *       firmware processor should be allowed to access this memory when mapped
 *       to the device. It is not valid to specify this flag with
 *       CPU_ALLOW_USERSPACE_ACCESS.
 *
 * CPU mapping options
 *    These use the prefix ``DRM_PVR_BO_CPU_``.
 *
 *    :ALLOW_USERSPACE_ACCESS: Allow userspace to map and access the contents
 *       of this memory. It is not valid to specify this flag with
 *       DEVICE_PM_FW_PROTECT.
 */
#define DRM_PVR_BO_DEVICE_BYPASS_CACHE _BITULL(0)
#define DRM_PVR_BO_DEVICE_PM_FW_PROTECT _BITULL(1)
#define DRM_PVR_BO_CPU_ALLOW_USERSPACE_ACCESS _BITULL(2)
#define DRM_PVR_BO_CREATE_ZEROED _BITULL(3)
/* Bits 4..63 are reserved. */

/**
 * struct drm_pvr_ioctl_create_bo_args - Arguments for %DRM_IOCTL_PVR_CREATE_BO
 */
struct drm_pvr_ioctl_create_bo_args {
	/**
	 * @size: [IN/OUT] Unaligned size of buffer object to create. On
	 * return, this will be populated with the actual aligned size of the
	 * new buffer.
	 */
	__u64 size;

	/**
	 * @handle: [OUT] GEM handle of the new buffer object for use in
	 * userspace.
	 */
	__u32 handle;

	/** @_padding_c: Reserved. This field must be zeroed. */
	__u32 _padding_c;

	/**
	 * @flags: [IN] Options which will affect the behaviour of this
	 * creation operation and future mapping operations on the created
	 * object. This field must be a valid combination of DRM_PVR_BO_*
	 * values, with all bits marked as reserved set to zero.
	 */
	__u64 flags;
};

/**
 * struct drm_pvr_ioctl_get_bo_mmap_offset_args - Arguments for
 * %DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET
 *
 * Like other DRM drivers, the "mmap" IOCTL doesn't actually map any memory.
 * Instead, it allocates a fake offset which refers to the specified buffer
 * object. This offset can be used with a real mmap call on the DRM device
 * itself.
 */
struct drm_pvr_ioctl_get_bo_mmap_offset_args {
	/** @handle: [IN] GEM handle of the buffer object to be mapped. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;

	/** @offset: [OUT] Fake offset to use in the real mmap call. */
	__u64 offset;
};

/**
 * DOC: Quirks returned by %DRM_PVR_PARAM_QUIRKS0 and
 *      %DRM_PVR_PARAM_QUIRKS_MUSTHAVE0
 *
 * PowerVR quirks come in two classes. "Must-have" quirks have workarounds that
 * must be present to guarantee correct GPU function. If the client does not
 * handle any of the "must-have" quirks then it should fail initialisation.
 *
 * Other quirks are "opt-in" - the client does not have to handle them, and they
 * are disabled by default. The exact opt-in mechanism will vary depending on
 * the quirk, but generally the client will provide additional data during job
 * submission via the extension stream.
 *
 * Only quirks relevant to the UAPI will be included here.
 */
#define DRM_PVR_QUIRK_BRN47217 0
#define DRM_PVR_QUIRK_BRN48545 1
#define DRM_PVR_QUIRK_BRN49927 2
#define DRM_PVR_QUIRK_BRN51764 3
#define DRM_PVR_QUIRK_BRN62269 4

#define DRM_PVR_QUIRK_MASK(quirk) _BITULL((quirk) & 63)

/**
 * DOC: Enhancements returned by %DRM_PVR_PARAM_ENHANCEMENTS0
 *
 * PowerVR enhancements are handled similarly to "opt-in" quirks. They are
 * disabled by default. The exact opt-in mechanism will vary depending on
 * the enhancement, but generally the client will provide additional data during
 * job submission via the extension stream.
 *
 * Only enhancements relevant to the UAPI will be included here.
 */
#define DRM_PVR_ENHANCEMENT_ERN35421 0
#define DRM_PVR_ENHANCEMENT_ERN42064 1

#define DRM_PVR_ENHANCEMENT_MASK(enhancement) _BITULL((enhancement) & 63)

/**
 * enum drm_pvr_param - Arguments for &drm_pvr_ioctl_get_param_args.param
 */
enum drm_pvr_param {
	/** @DRM_PVR_PARAM_INVALID: Invalid parameter. Do not use. */
	DRM_PVR_PARAM_INVALID = 0,

	/**
	 * @DRM_PVR_PARAM_GPU_ID: GPU identifier.
	 *
	 * For all currently supported GPUs this is the BVNC encoded as a 64-bit
	 * value as follows:
	 *
	 *    +--------+--------+--------+-------+
	 *    | 63..48 | 47..32 | 31..16 | 15..0 |
	 *    +========+========+========+=======+
	 *    | B      | V      | N      | C     |
	 *    +--------+--------+--------+-------+
	 */
	DRM_PVR_PARAM_GPU_ID,

	/**
	 * @DRM_PVR_PARAM_HWRT_NUM_GEOMDATAS: Number of geom data arguments
	 * required when creating a HWRT dataset.
	 */
	DRM_PVR_PARAM_HWRT_NUM_GEOMDATAS,

	/**
	 * @DRM_PVR_PARAM_HWRT_NUM_RTDATAS: Number of RT data arguments
	 * required when creating a HWRT dataset.
	 */
	DRM_PVR_PARAM_HWRT_NUM_RTDATAS,

	/**
	 * @DRM_PVR_PARAM_HWRT_NUM_FREELISTS: Number of free list data
	 * arguments required when creating a HWRT dataset.
	 */
	DRM_PVR_PARAM_HWRT_NUM_FREELISTS,

	/**
	 * @DRM_PVR_PARAM_FW_VERSION: Version number of GPU firmware.
	 *
	 * This is encoded with the major version number in the upper 32 bits of
	 * the output value, and the minor version number in the lower 32 bits.
	 */
	DRM_PVR_PARAM_FW_VERSION,

	/**
	 * @DRM_PVR_PARAM_QUIRKS0: Hardware quirks 0.
	 *
	 * These quirks affect userspace and the kernel or firmware. They are
	 * disabled by default and require userspace to opt-in. The opt-in
	 * mechanism depends on the quirk.
	 *
	 * This is a bitmask of %DRM_PVR_QUIRKS0_HAS_*.
	 */
	DRM_PVR_PARAM_QUIRKS0,

	/**
	 * @DRM_PVR_PARAM_QUIRKS_MUSTHAVE0: "Must have" hardware quirks 0.
	 *
	 * This describes a fixed list of quirks that the client must support
	 * for this device. If userspace does not support all the quirks in this
	 * parameter then functionality is not guaranteed and client
	 * initialisation must fail.
	 *
	 * This is a bitmask of %DRM_PVR_QUIRKS0_HAS_*.
	 */
	DRM_PVR_PARAM_QUIRKS_MUSTHAVE0,

	/**
	 * @DRM_PVR_PARAM_ENHANCEMENTS0: Hardware enhancements 0.
	 *
	 * These enhancements affect userspace and the kernel or firmware. They
	 * are disabled by default and require userspace to opt-in. The opt-in
	 * mechanism depends on the quirk.
	 *
	 * This is a bitmask of %DRM_PVR_ENHANCEMENTS0_HAS_*.
	 */
	DRM_PVR_PARAM_ENHANCEMENTS0,

	/*
	 * @DRM_PVR_PARAM_FREE_LIST_MIN_PAGES: Minimum allowed free list size,
	 * in PM physical pages.
	 */
	DRM_PVR_PARAM_FREE_LIST_MIN_PAGES,

	/*
	 * @DRM_PVR_PARAM_FREE_LIST_MAX_PAGES: Maximum allowed free list size,
	 * in PM physical pages.
	 */
	DRM_PVR_PARAM_FREE_LIST_MAX_PAGES,

	/*
	 * @DRM_PVR_PARAM_COMMON_STORE_ALLOC_REGION_SIZE: Size of the Allocation
	 * Region within the Common Store used for coefficient and shared
	 * registers, in dwords.
	 */
	DRM_PVR_PARAM_COMMON_STORE_ALLOC_REGION_SIZE,

	/**
	 * @DRM_PVR_PARAM_COMMON_STORE_PARTITION_SPACE_SIZE: Size of the
	 * Partition Space within the Common Store for output buffers, in
	 * dwords.
	 */
	DRM_PVR_PARAM_COMMON_STORE_PARTITION_SPACE_SIZE,

	/**
	 * @DRM_PVR_PARAM_NUM_PHANTOMS: Number of Phantoms present.
	 */
	DRM_PVR_PARAM_NUM_PHANTOMS,

	/**
	 * @DRM_PVR_PARAM_MAX_COEFFS: Maximum coefficients, in dwords.
	 */
	DRM_PVR_PARAM_MAX_COEFFS,

	/**
	 * @DRM_PVR_PARAM_CDM_MAX_LOCAL_MEM_SIZE_REGS: Maximum amount of local
	 * memory available to a kernel, in dwords.
	 */
	DRM_PVR_PARAM_CDM_MAX_LOCAL_MEM_SIZE_REGS,

	/**
	 * @DRM_PVR_PARAM_NUM_HEAPS: Number of heaps exposed by %DRM_IOCTL_PVR_GET_HEAP_INFO for
	 * this device.
	 */
	DRM_PVR_PARAM_NUM_HEAPS,
};

/**
 * struct drm_pvr_ioctl_get_param_args - Arguments for %DRM_IOCTL_PVR_GET_PARAM
 */
struct drm_pvr_ioctl_get_param_args {
	/**
	 * @param: [IN] Parameter for which a value should be returned.
	 *
	 * This must be one of the values defined by &enum drm_pvr_param, with
	 * the exception of %DRM_PVR_PARAM_INVALID.
	 */
	__u32 param;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;

	/** @value: [OUT] Value for @param. */
	__u64 value;
};

/**
 * enum drm_pvr_ctx_priority - Arguments for
 * &drm_pvr_ioctl_create_context_args.priority
 */
enum drm_pvr_ctx_priority {
	DRM_PVR_CTX_PRIORITY_LOW = -512,
	DRM_PVR_CTX_PRIORITY_NORMAL = 0,
	/* A priority above NORMAL requires CAP_SYS_NICE or DRM_MASTER. */
	DRM_PVR_CTX_PRIORITY_HIGH = 512,
};

/* clang-format off */

/**
 * enum drm_pvr_ctx_type - Arguments for
 * &drm_pvr_ioctl_create_context_args.type
 */
enum drm_pvr_ctx_type {
	/**
	 * @DRM_PVR_CTX_TYPE_RENDER: Render context. Use &struct
	 * drm_pvr_ioctl_create_render_context_args for context creation arguments.
	 */
	DRM_PVR_CTX_TYPE_RENDER = 0,

	/**
	 * @DRM_PVR_CTX_TYPE_COMPUTE: Compute context. Use &struct
	 * drm_pvr_ioctl_create_compute_context_args for context creation arguments.
	 */
	DRM_PVR_CTX_TYPE_COMPUTE,

	/**
	 * @DRM_PVR_CTX_TYPE_TRANSFER_FRAG: Transfer context for fragment data masters. Use
	 * &struct drm_pvr_ioctl_create_transfer_context_args for context creation arguments.
	 */
	DRM_PVR_CTX_TYPE_TRANSFER_FRAG,
};

/* clang-format on */

/**
 * struct drm_pvr_ioctl_create_context_args - Arguments for
 * %DRM_IOCTL_PVR_CREATE_CONTEXT
 */
struct drm_pvr_ioctl_create_context_args {
	/**
	 * @type: [IN] Type of context to create.
	 *
	 * This must be one of the values defined by &enum drm_pvr_ctx_type.
	 */
	__u32 type;

	/** @flags: [IN] Flags for context. */
	__u32 flags;

	/**
	 * @priority: [IN] Priority of new context.
	 *
	 * This must be one of the values defined by &enum drm_pvr_ctx_priority.
	 */
	__s32 priority;

	/** @handle: [OUT] Handle for new context. */
	__u32 handle;

	/**
	 * @static_context_state: [IN] Pointer to static context state to copy to
	 *                             new context.
	 *
	 * The state differs based on the value of @type:
	 * * For %DRM_PVR_CTX_TYPE_RENDER, state should be of type
	 *   &struct rogue_fwif_static_rendercontext_state.
	 * * For %DRM_PVR_CTX_TYPE_COMPUTE, state should be of type
	 *   &struct rogue_fwif_static_computecontext_state.
	 */
	__u64 static_context_state;

	/**
	 * @static_context_state_len: [IN] Length of static context state, in bytes.
	 */
	__u32 static_context_state_len;

	/** @_padding_1c: Reserved. This field must be zeroed. */
	__u32 _padding_1c;

	/**
	 * @callstack_addr: [IN] Address for initial call stack pointer. Only valid
	 *                       if @type is %DRM_PVR_CTX_TYPE_RENDER, otherwise
	 *                       must be 0.
	 */
	__u64 callstack_addr;
};

/**
 * struct drm_pvr_ioctl_destroy_context_args - Arguments for
 * %DRM_IOCTL_PVR_DESTROY_CONTEXT
 */
struct drm_pvr_ioctl_destroy_context_args {
	/**
	 * @handle: [IN] Handle for context to be destroyed.
	 */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;
};

/**
 * struct drm_pvr_ioctl_create_free_list_args - Arguments for
 * %DRM_IOCTL_PVR_CREATE_FREE_LIST
 *
 * Free list arguments have the following constraints :
 *
 * - &max_num_pages must be greater than zero.
 * - &grow_threshold must be between 0 and 100.
 * - &grow_num_pages must be less than or equal to &max_num_pages.
 * - &initial_num_pages, &max_num_pages and &grow_num_pages must be multiples
 *   of 4.
 *
 * When &grow_num_pages is 0 :
 * - &initial_num_pages must be equal to &max_num_pages
 *
 * When &grow_num_pages is non-zero :
 * - &initial_num_pages must be less than &max_num_pages.
 */
struct drm_pvr_ioctl_create_free_list_args {
	/**
	 * @free_list_gpu_addr: [IN] Address of GPU mapping of buffer object
	 *                           containing memory to be used by free list.
	 *
	 * The mapped region of the buffer object must be at least
	 * @max_num_pages * sizeof(__u32).
	 *
	 * The buffer object must have been created with
	 * %DRM_PVR_BO_DEVICE_PM_FW_PROTECT set and
	 * %DRM_PVR_BO_CPU_ALLOW_USERSPACE_ACCESS not set.
	 */
	__u64 free_list_gpu_addr;

	/** @initial_num_pages: [IN] Pages initially allocated to free list. */
	__u32 initial_num_pages;

	/** @max_num_pages: [IN] Maximum number of pages in free list. */
	__u32 max_num_pages;

	/** @grow_num_pages: [IN] Pages to grow free list by per request. */
	__u32 grow_num_pages;

	/**
	 * @grow_threshold: [IN] Percentage of FL memory used that should
	 *                       trigger a new grow request.
	 */
	__u32 grow_threshold;

	/**
	 * @handle: [OUT] Handle for created free list.
	 */
	__u32 handle;

	/** @_padding_1c: Reserved. This field must be zeroed. */
	__u32 _padding_1c;
};

/**
 * struct drm_pvr_ioctl_destroy_free_list_args - Arguments for
 * %DRM_IOCTL_PVR_DESTROY_FREE_LIST
 */
struct drm_pvr_ioctl_destroy_free_list_args {
	/**
	 * @handle: [IN] Handle for free list to be destroyed.
	 */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;
};

struct drm_pvr_create_hwrt_geom_data_args {
	/** @tpc_dev_addr: [IN] Tail pointer cache GPU virtual address. */
	__u64 tpc_dev_addr;

	/** @tpc_size: [IN] Size of TPC, in bytes. */
	__u32 tpc_size;

	/** @tpc_stride: [IN] Stride between layers in TPC, in pages */
	__u32 tpc_stride;

	/** @vheap_table_dev_addr: [IN] VHEAP table GPU virtual address. */
	__u64 vheap_table_dev_addr;

	/** @rtc_dev_addr: [IN] Render Target Cache virtual address. */
	__u64 rtc_dev_addr;
};

struct drm_pvr_create_hwrt_rt_data_args {
	/** @pm_mlist_dev_addr: [IN] PM MLIST GPU virtual address. */
	__u64 pm_mlist_dev_addr;

	/** @macrotile_array_dev_addr: [IN] Macrotile array GPU virtual address. */
	__u64 macrotile_array_dev_addr;

	/** @region_header_dev_addr: [IN] Region header array GPU virtual address. */
	__u64 region_header_dev_addr;
};

/**
 * struct drm_pvr_ioctl_create_hwrt_dataset_args - Arguments for
 * %DRM_IOCTL_PVR_CREATE_HWRT_DATASET
 */
struct drm_pvr_ioctl_create_hwrt_dataset_args {
	/** @geom_data_args: [IN] Geometry data arguments. */
	struct drm_pvr_create_hwrt_geom_data_args geom_data_args;

	/** @rt_data_args: [IN] Array of render target arguments. */
	struct drm_pvr_create_hwrt_rt_data_args rt_data_args[2];

	/**
	 * @free_list_args: [IN] Array of free list handles.
	 *
	 * free_list_handles[0] must have initial size of at least that reported
	 * by %DRM_PVR_PARAM_FREE_LIST_MIN_PAGES.
	 */
	__u32 free_list_handles[2];

	/** @width: [IN] Width in pixels. */
	__u32 width;

	/** @height: [IN] Height in pixels. */
	__u32 height;

	/** @samples: [IN] Number of samples. */
	__u32 samples;

	/** @layers: [IN] Number of layers. */
	__u32 layers;

	/** @isp_merge_lower_x: [IN] Lower X coefficient for triangle merging. */
	__u32 isp_merge_lower_x;

	/** @isp_merge_lower_y: [IN] Lower Y coefficient for triangle merging. */
	__u32 isp_merge_lower_y;

	/** @isp_merge_scale_x: [IN] Scale X coefficient for triangle merging. */
	__u32 isp_merge_scale_x;

	/** @isp_merge_scale_y: [IN] Scale Y coefficient for triangle merging. */
	__u32 isp_merge_scale_y;

	/** @isp_merge_upper_x: [IN] Upper X coefficient for triangle merging. */
	__u32 isp_merge_upper_x;

	/** @isp_merge_upper_y: [IN] Upper Y coefficient for triangle merging. */
	__u32 isp_merge_upper_y;

	/**
	 * @region_header_size: [IN] Size of region header array. This common field is used by
	 *                           both render targets in this data set.
	 *
	 * The units for this field differ depending on what version of the simple internal
	 * parameter format the device uses. If format 2 is in use then this is interpreted as the
	 * number of region headers. For other formats it is interpreted as the size in dwords.
	 */
	__u32 region_header_size;

	/**
	 * @handle: [OUT] Handle for created HWRT dataset.
	 */
	__u32 handle;
};

/**
 * struct drm_pvr_ioctl_destroy_hwrt_dataset_args - Arguments for
 * %DRM_IOCTL_PVR_DESTROY_HWRT_DATASET
 */
struct drm_pvr_ioctl_destroy_hwrt_dataset_args {
	/**
	 * @handle: [IN] Handle for HWRT dataset to be destroyed.
	 */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;
};

/**
 * DOC: Heap UAPI
 *
 * The PowerVR address space is pre-divided into a number of heaps. The exact
 * number and layout of heaps may vary depending on the exact GPU being used.
 *
 * Heaps have the following properties:
 * - ID: Defines the type of heap. In addition to the general heap, there are a
 *   number of special purpose heaps.
 * - Base & size: Defines the heap address range.
 * - Page size: Defined by the GPU device. This may not be constant across all
 *   heaps.
 * - Static data carveout base & size: Defines the static data carveout region
 *   of the heap address range. If the heap does not have a carveout region then
 *   base & size will be zero.
 * - Static data areas: Pre-allocated data areas within the carveout region.
 */

enum drm_pvr_get_heap_info_op {
	/** @DRM_PVR_HEAP_OP_GET_HEAP_INFO: Get &struct drm_pvr_heap for the requested heap. */
	DRM_PVR_HEAP_OP_GET_HEAP_INFO = 0,
	/**
	 * @DRM_PVR_HEAP_OP_GET_STATIC_DATA_AREAS: Get array of &struct drm_pvr_static_data_area
	 * for the requested heap.
	 */
	DRM_PVR_HEAP_OP_GET_STATIC_DATA_AREAS = 1,
};

/**
 * enum drm_pvr_heap_id - Valid heap IDs returned by %DRM_IOCTL_PVR_GET_HEAP_INFO
 */
enum drm_pvr_heap_id {
	/** @DRM_PVR_HEAP_GENERAL: General purpose heap. */
	DRM_PVR_HEAP_GENERAL = 0,
	/** @DRM_PVR_HEAP_PDS_CODE_DATA: PDS code & data heap. */
	DRM_PVR_HEAP_PDS_CODE_DATA,
	/** @DRM_PVR_HEAP_USC_CODE: USC code heap. */
	DRM_PVR_HEAP_USC_CODE,
	/** @DRM_PVR_HEAP_RGNHDR: Region header heap. Only used if GPU has BRN63142. */
	DRM_PVR_HEAP_RGNHDR,
	/** @DRM_PVR_HEAP_VIS_TEST: Visibility test heap. */
	DRM_PVR_HEAP_VIS_TEST,
	/** @DRM_PVR_HEAP_TRANSFER_FRAG: Transfer fragment heap. */
	DRM_PVR_HEAP_TRANSFER_FRAG,
};

/*
 * DOC: Flags for heaps returned by GET_HEAP_INFO ioctl command.
 */
#define DRM_PVR_HEAP_FLAGS_VALID_MASK 0

struct drm_pvr_heap {
	/** @id: Heap ID. This must be one of the values defined by &enum drm_pvr_heap_id. */
	__u32 id;

	/** @flags: Flags for this heap. Currently always 0. */
	__u32 flags;

	/** @base: Base address of heap. */
	__u64 base;

	/** @size: Size of heap, in bytes. */
	__u64 size;

	/**
	 * @static_data_carveout_base: Base address of static data carveout.
	 *
	 * The static data carveout must be located at the beginning or end of the heap. Any other
	 * location is invalid and should be rejected by the caller.
	 */
	__u64 static_data_carveout_base;

	/**
	 * @static_data_carveout_size: Size of static data carveout, in bytes. May be 0 if this
	 *                             heap has no carveout.
	 */
	__u64 static_data_carveout_size;

	/** @page_size_log2: Log2 of page size. */
	__u32 page_size_log2;

	/**
	 * @nr_static_data_areas: Number of &struct drm_pvr_static_data_areas
	 *                        returned for this heap by
	 *                        %DRM_PVR_HEAP_OP_GET_STATIC_DATA_AREAS.
	 */
	__u32 nr_static_data_areas;
};

enum drm_pvr_static_data_area_id {
	/**
	 * @DRM_PVR_STATIC_DATA_AREA_EOT: End of Tile USC program.
	 *
	 * The End of Tile task runs at completion of a tile, and is responsible for emitting the
	 * tile to the Pixel Back End.
	 */
	DRM_PVR_STATIC_DATA_AREA_EOT = 0,

	/**
	 * @DRM_PVR_STATIC_DATA_AREA_FENCE: MCU fence area, used during cache flush and
	 * invalidation.
	 *
	 * This must point to valid physical memory but the contents otherwise are not used.
	 */
	DRM_PVR_STATIC_DATA_AREA_FENCE,

	/** @DRM_PVR_STATIC_DATA_AREA_VDM_SYNC: VDM sync program.
	 *
	 * The VDM sync program is used to synchronise multiple areas of the GPU hardware.
	 */
	DRM_PVR_STATIC_DATA_AREA_VDM_SYNC,

	/**
	 * @DRM_PVR_STATIC_DATA_AREA_YUV_CSC: YUV coefficients.
	 *
	 * Area contains up to 16 slots with stride of 64 bytes. Each is a 3x4 matrix of u16 fixed
	 * point numbers, with 1 sign bit, 2 integer bits and 13 fractional bits.
	 *
	 * The slots are :
	 * 0 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY_KHR
	 * 1 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_IDENTITY_KHR (full range)
	 * 2 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_IDENTITY_KHR (conformant range)
	 * 3 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709_KHR (full range)
	 * 4 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709_KHR (conformant range)
	 * 5 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601_KHR (full range)
	 * 6 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601_KHR (conformant range)
	 * 7 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020_KHR (full range)
	 * 8 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020_KHR (conformant range)
	 * 9 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601_KHR (conformant range, 10 bit)
	 * 10 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709_KHR (conformant range, 10 bit)
	 * 11 = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020_KHR (conformant range, 10 bit)
	 * 14 = Identity (biased)
	 * 15 = Identity
	 */
	DRM_PVR_STATIC_DATA_AREA_YUV_CSC,
};

struct drm_pvr_static_data_area {
	/** @id: ID of static data area. */
	__u32 id;

	/** @size: Size of static data area. */
	__u32 size;

	/** @offset: Offset of static data area from start of static data carveout. */
	__u64 offset;
};

/**
 * struct drm_pvr_ioctl_get_heap_info_args - Arguments for
 * %DRM_IOCTL_PVR_GET_HEAP_INFO
 */
struct drm_pvr_ioctl_get_heap_info_args {
	/**
	 * @op: [IN] Operation to perform for this ioctl. Must be one of
	 *           &enum drm_pvr_get_heap_info_op.
	 */
	__u32 op;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;

	/**
	 * @data: [IN] User pointer to memory that this ioctl writes to. This should point to
	 *             &struct drm_pvr_heap when &op == %DRM_PVR_HEAP_OP_GET_HEAP_INFO, or an
	 *             array of &struct drm_pvr_static_data_area, of size
	 *             %drm_pvr_heap.nr_static_data_areas elements, when &op ==
	 *             %DRM_PVR_HEAP_OP_GET_STATIC_DATA_AREAS.
	 */
	__u64 data;

	/** @heap_nr: [IN] Number of heap to get information for. */
	__u32 heap_nr;
};

/**
 * DOC: VM UAPI
 *
 * The VM UAPI allows userspace to create buffer object mappings in GPU virtual address space.
 *
 * The client is responsible for managing GPU address space. It should allocate mappings within
 * the heaps returned by %DRM_IOCTL_PVR_GET_HEAP_INFO.
 *
 * %DRM_IOCTL_PVR_VM_MAP creates a new mapping. The client provides the target virtual address for
 * the mapping. Size and offset within the mapped buffer object can be specified, so the client can
 * partially map a buffer.
 *
 * %DRM_IOCTL_PVR_VM_UNMAP removes a mapping. The entire mapping will be removed from GPU address
 * space. For this reason only the start address is provided by the client.
 */

/**
 * struct drm_pvr_ioctl_vm_map_args - Arguments for %DRM_IOCTL_PVR_VM_MAP.
 */
struct drm_pvr_ioctl_vm_map_args {
	/**
	 * @device_addr: [IN] Requested device-virtual address for the mapping.
	 * This must be non-zero and aligned to the device page size for the
	 * heap containing the requested address. It is an error to specify an
	 * address which is not contained within one of the heaps returned by
	 * %DRM_IOCTL_PVR_GET_HEAP_INFO.
	 */
	__u64 device_addr;

	/** @flags: [IN] Flags which affect this mapping. Currently always 0. */
	__u32 flags;

	/**
	 * @handle: [IN] Handle of the target buffer object. This must be a
	 * valid handle returned by %DRM_IOCTL_PVR_CREATE_BO.
	 */
	__u32 handle;

	/**
	 * @offset: [IN] Offset into the target bo from which to begin the
	 * mapping.
	 */
	__u64 offset;

	/**
	 * @size: [IN] Size of the requested mapping. Must be aligned to
	 * the device page size for the heap containing the requested address,
	 * as well as the host page size. When added to @device_addr, the
	 * result must not overflow the heap which contains @device_addr (i.e.
	 * the range specified by @device_addr and @size must be completely
	 * contained within a single heap specified by
	 * %DRM_IOCTL_PVR_GET_HEAP_INFO).
	 */
	__u64 size;
};

/**
 * struct drm_pvr_ioctl_vm_unmap_args - Arguments for %DRM_IOCTL_PVR_VM_UNMAP.
 */
struct drm_pvr_ioctl_vm_unmap_args {
	/**
	 * @device_addr: [IN] Device-virtual address at the start of the target
	 * mapping. This must be non-zero.
	 */
	__u64 device_addr;
};

/**
 * DOC: Flags for SUBMIT_JOB ioctl geometry command.
 *
 * Operations
 * ~~~~~~~~~~
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_GEOM_CMD_FIRST
 *
 *    Indicates if this the first command to be issued for a render.
 *
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_GEOM_CMD_LAST
 *
 *    Indicates if this the last command to be issued for a render.
 *
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_GEOM_CMD_SINGLE_CORE
 *
 *    Forces to use single core in a multi core device.
 *
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_GEOM_CMD_FLAGS_MASK
 *
 *    Logical OR of all the geometry cmd flags.
 */
#define DRM_PVR_SUBMIT_JOB_GEOM_CMD_FIRST _BITULL(0)
#define DRM_PVR_SUBMIT_JOB_GEOM_CMD_LAST _BITULL(1)
#define DRM_PVR_SUBMIT_JOB_GEOM_CMD_SINGLE_CORE _BITULL(2)
#define DRM_PVR_SUBMIT_JOB_GEOM_CMD_FLAGS_MASK                                 \
	(DRM_PVR_SUBMIT_JOB_GEOM_CMD_FIRST |                                   \
	 DRM_PVR_SUBMIT_JOB_GEOM_CMD_LAST |                                    \
	 DRM_PVR_SUBMIT_JOB_GEOM_CMD_SINGLE_CORE)

/**
 * DOC: Flags for SUBMIT_JOB ioctl fragment command.
 *
 * Operations
 * ~~~~~~~~~~
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_FRAG_CMD_SINGLE_CORE
 *
 *    Use single core in a multi core setup.
 *
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_FRAG_CMD_DEPTHBUFFER
 *
 *    Indicates whether a depth buffer is present.
 *
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_FRAG_CMD_STENCILBUFFER
 *
 *    Indicates whether a stencil buffer is present.
 *
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_FRAG_CMD_PREVENT_CDM_OVERLAP
 *
 *    Disallow compute overlapped with this render.
 *
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_FRAG_CMD_GET_VIS_RESULTS
 *
 *    Indicates whether this render produces visibility results.
 *
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_FRAG_CMD_FLAGS_MASK
 *
 *    Logical OR of all the fragment cmd flags.
 */
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_SINGLE_CORE _BITULL(0)
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_DEPTHBUFFER _BITULL(1)
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_STENCILBUFFER _BITULL(2)
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_PREVENT_CDM_OVERLAP _BITULL(3)
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_GET_VIS_RESULTS _BITULL(5)
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_FLAGS_MASK                                 \
	(DRM_PVR_SUBMIT_JOB_FRAG_CMD_SINGLE_CORE |                             \
	 DRM_PVR_SUBMIT_JOB_FRAG_CMD_DEPTHBUFFER |                             \
	 DRM_PVR_SUBMIT_JOB_FRAG_CMD_STENCILBUFFER |                           \
	 DRM_PVR_SUBMIT_JOB_FRAG_CMD_PREVENT_CDM_OVERLAP |                     \
	 DRM_PVR_SUBMIT_JOB_FRAG_CMD_GET_VIS_RESULTS)

/*
 * struct drm_pvr_job_render_args - Arguments for %DRM_PVR_JOB_TYPE_RENDER
 */
struct drm_pvr_job_render_args {
	/**
	 * @geom_cmd_stream: [IN] Pointer to command stream for geometry command.
	 *
	 * The geometry command stream must be u64-aligned.
	 */
	__u64 geom_cmd_stream;

	/**
	 * @frag_cmd_stream: [IN] Pointer to command stream for fragment command.
	 *
	 * The fragment command stream must be u64-aligned.
	 */
	__u64 frag_cmd_stream;

	/**
	 * @geom_cmd_stream_len: [IN] Length of geometry command stream, in bytes.
	 */
	__u32 geom_cmd_stream_len;

	/**
	 * @frag_cmd_stream_len: [IN] Length of fragment command stream, in bytes.
	 */
	__u32 frag_cmd_stream_len;

	/**
	 * @in_syncobj_handles_frag: [IN] Pointer to array of drm_syncobj handles for
	 *                                input fences for fragment job.
	 *
	 * This array must be &num_in_syncobj_handles_frag entries large.
	 *
	 * drm_syncobj handles for the geometry job are contained in
	 * &struct drm_pvr_ioctl_submit_job_args.in_syncobj_handles.
	 */
	__u64 in_syncobj_handles_frag;

	/**
	 * @num_in_syncobj_handles_frag: [IN] Number of input syncobj handles for fragment job.
	 */
	__u32 num_in_syncobj_handles_frag;

	/**
	 * @out_syncobj_geom: [OUT] drm_syncobj handle for geometry output fence
	 */
	__u32 out_syncobj_geom;

	/**
	 * @out_syncobj_frag: [OUT] drm_syncobj handle for fragment output fence
	 */
	__u32 out_syncobj_frag;

	/**
	 * @hwrt_data_set_handle: [IN] Handle for HWRT data set.
	 *
	 * This must be a valid handle returned by %DRM_IOCTL_PVR_CREATE_OBJECT.
	 */
	__u32 hwrt_data_set_handle;

	/**
	 * @hwrt_data_index: [IN] Index of HWRT data within data set.
	 */
	__u32 hwrt_data_index;

	/**
	 * @flags: [IN] Flags for geometry command.
	 */
	__u32 geom_flags;

	/**
	 * @flags: [IN] Flags for fragment command.
	 */
	__u32 frag_flags;

	/** @_padding_54: Reserved. This field must be zeroed. */
	__u32 _padding_54;
};

/**
 * DOC: Flags for SUBMIT_JOB ioctl compute command.
 *
 * Operations
 * ~~~~~~~~~~
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP
 *
 *    Disallow other jobs overlapped with this compute.
 *
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE
 *
 *    Forces to use single core in a multi core device.
 */
#define DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP _BITULL(0)
#define DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE _BITULL(1)
#define DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_FLAGS_MASK         \
	(DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP | \
	 DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE)

/*
 * struct drm_pvr_job_compute_args - Arguments for %DRM_PVR_JOB_TYPE_COMPUTE
 */
struct drm_pvr_job_compute_args {
	/**
	 * @cmd_stream: [IN] Pointer to command stream for compute command.
	 *
	 * The command stream must be u64-aligned.
	 */
	__u64 cmd_stream;

	/**
	 * @cmd_stream_len: [IN] Length of compute command stream, in bytes.
	 */
	__u32 cmd_stream_len;

	/**
	 * @flags: [IN] Flags for command.
	 */
	__u32 flags;

	/**
	 * @out_syncobj: [OUT] drm_syncobj handle for output fence
	 */
	__u32 out_syncobj;
};

/**
 * DOC: Flags for SUBMIT_JOB ioctl transfer command.
 *
 * Operations
 * ~~~~~~~~~~
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_TRANSFER_CMD_SINGLE_CORE
 *
 *    Forces job to use a single core in a multi core device.
 */
#define DRM_PVR_SUBMIT_JOB_TRANSFER_CMD_SINGLE_CORE _BITULL(0)

#define DRM_PVR_SUBMIT_JOB_TRANSFER_CMD_FLAGS_MASK \
	DRM_PVR_SUBMIT_JOB_TRANSFER_CMD_SINGLE_CORE

/*
 * struct drm_pvr_job_transfer_args - Arguments for %DRM_PVR_JOB_TYPE_TRANSFER_FRAG
 */
struct drm_pvr_job_transfer_args {
	/**
	 * @cmd_stream: [IN] Pointer to command stream for transfer command.
	 *
	 * The command stream must be u64-aligned.
	 */
	__u64 cmd_stream;

	/**
	 * @cmd_stream_len: [IN] Length of transfer command stream, in bytes.
	 */
	__u32 cmd_stream_len;

	/**
	 * @flags: [IN] Flags for command.
	 */
	__u32 flags;

	/**
	 * @out_syncobj: [OUT] drm_syncobj handle for output fence
	 */
	__u32 out_syncobj;
};

/*
 * DOC: Flags for SUBMIT_JOB ioctl null command.
 */
#define DRM_PVR_SUBMIT_JOB_NULL_CMD_FLAGS_MASK 0

/*
 * struct drm_pvr_job_null_args - Arguments for %DRM_PVR_JOB_TYPE_NULL
 */
struct drm_pvr_job_null_args {
	/**
	 * @flags: [IN] Flags for command.
	 */
	__u32 flags;

	/**
	 * @out_syncobj: [OUT] drm_syncobj handle for output fence
	 */
	__u32 out_syncobj;

	/** @_padding_14: Reserved. This field must be zeroed. */
	__u32 _padding_14;
};

/**
 * enum drm_pvr_job_type - Arguments for &drm_pvr_ioctl_submit_job_args.job_type
 */
enum drm_pvr_job_type {
	DRM_PVR_JOB_TYPE_RENDER = 0,
	DRM_PVR_JOB_TYPE_COMPUTE,
	DRM_PVR_JOB_TYPE_TRANSFER_FRAG,
	DRM_PVR_JOB_TYPE_NULL,
};

/**
 * struct drm_pvr_ioctl_submit_job_args - Arguments for %DRM_IOCTL_PVR_SUBMIT_JOB
 */
struct drm_pvr_ioctl_submit_job_args {
	/**
	 * @job_type: [IN] Type of job being submitted
	 *
	 * This must be one of the values defined by &enum drm_pvr_job_type.
	 */
	__u32 job_type;

	/**
	 * @context: [IN] Context handle.
	 *
	 * When @job_type is %DRM_PVR_JOB_TYPE_RENDER, %DRM_PVR_JOB_TYPE_COMPUTE or
	 * %DRM_PVR_JOB_TYPE_TRANSFER_FRAG, this must be a valid handle returned by
	 * %DRM_IOCTL_PVR_CREATE_CONTEXT. The type of context must be compatible with the type of
	 * job being submitted.
	 *
	 * When @job_type is %DRM_PVR_JOB_TYPE_NULL, this must be zero.
	 */
	__u32 context_handle;

	/** @data: [IN] User pointer to job type specific arguments. */
	__u64 data;

	/**
	 * @in_syncobj_handles: [IN] Pointer to array of drm_syncobj handles for input fences.
	 *
	 * This array must be &num_in_syncobj_handles entries large.
	 */
	__u64 in_syncobj_handles;

	/**
	 * @num_in_syncobj_handles: [IN] Number of input syncobj handles.
	 */
	__u32 num_in_syncobj_handles;

};

/* Definitions for coredump decoding in userspace. */

#define PVR_COREDUMP_HEADER_MAGIC 0x21525650 /* PVR! */
#define PVR_COREDUMP_HEADER_VERSION_MAJ 1
#define PVR_COREDUMP_HEADER_VERSION_MIN 0

/**
 * struct pvr_coredump_header - Header of PowerVR coredump
 */
struct pvr_coredump_header {
	/** @magic: Will be %PVR_COREDUMP_HEADER_MAGIC. */
	__u32 magic;
	/** @major_version: Will be %PVR_COREDUMP_HEADER_VERSION_MAJ. */
	__u32 major_version;
	/** @minor_version: Will be %PVR_COREDUMP_HEADER_VERSION_MIN. */
	__u32 minor_version;
	/** @flags: Flags for this coredump. Currently no flags are defined, this should be zero. */
	__u32 flags;
	/** @size: Size of coredump (including this header) in bytes. */
	__u32 size;
	/** @padding: Reserved. This field must be zero. */
	__u32 padding;
};

/**
 * enum pvr_coredump_block_type - Valid coredump block types
 */
enum pvr_coredump_block_type {
	/**
	 * %PVR_COREDUMP_BLOCK_TYPE_DEVINFO: Device information block.
	 *
	 * Block data is &struct pvr_coredump_block_devinfo.
	 */
	PVR_COREDUMP_BLOCK_TYPE_DEVINFO = 0,

	/**
	 * %PVR_COREDUMP_BLOCK_TYPE_REGISTERS: Register block.
	 *
	 * Block data is an array of &struct pvr_coredump_block_register. Number of registers is
	 * determined by block size.
	 */
	PVR_COREDUMP_BLOCK_TYPE_REGISTERS,

	/**
	 * %PVR_COREDUMP_BLOCK_TYPE_CONTEXT_RESET_DATA: Context reset data block.
	 *
	 * Block data is &struct pvr_coredump_block_reset_data.
	 */
	PVR_COREDUMP_BLOCK_TYPE_CONTEXT_RESET_DATA,

	/**
	 * %PVR_COREDUMP_BLOCK_TYPE_HWRINFO: Hardware Reset information block.
	 *
	 * Block data is &struct pvr_coredump_block_hwrinfo.
	 */
	PVR_COREDUMP_BLOCK_TYPE_HWRINFO,
};

/**
 * struct pvr_coredump_block_header - Header of PowerVR coredump block
 *
 * Block data immediately follows this header. The format is determined by @type.
 */
struct pvr_coredump_block_header {
	/** @type: Block type. One of %PVR_COREDUMP_BLOCK_TYPE_*. */
	__u32 type;
	/** @size: Size of block data following this header, in bytes. */
	__u32 size;
	/** @flags: Type dependent flags. */
	__u32 flags;
	/** @padding: Reserved. This field must be zero. */
	__u32 padding;
};

#define PVR_COREDUMP_PROCESS_NAME_LEN 16
#define PVR_COREDUMP_VERSION_LEN      65
#define PVR_COREDUMP_DEVINFO_PADDING (8 - ((PVR_COREDUMP_PROCESS_NAME_LEN + \
					    PVR_COREDUMP_VERSION_LEN) & 7))

/**
 * struct pvr_coredump_block_devinfo - Device information block
 */
struct pvr_coredump_block_devinfo {
	/** @gpu_id: GPU ID. */
	__u64 gpu_id;
	/** @fw_version: Version of PowerVR firmware on system that created the coredump. */
	struct {
		/** @major: Major version number. */
		__u32 major;
		/** @minor: Minor version number. */
		__u32 minor;
	} fw_version;
	/** @process_name: Name of process that submitted the failed job. */
	char process_name[PVR_COREDUMP_PROCESS_NAME_LEN];
	/** @kernel_version: String of kernel version on system that created the coredump. */
	char kernel_version[PVR_COREDUMP_VERSION_LEN];
	/** @padding: Reserved. This field must be zero. */
	__u8 padding[PVR_COREDUMP_DEVINFO_PADDING];
};

/** %PVR_COREDUMP_REGISTER_FLAG_SIZE_MASK: Mask of register size field. */
#define PVR_COREDUMP_REGISTER_FLAG_SIZE_MASK 7
/** %PVR_COREDUMP_REGISTER_FLAG_SIZE_32BIT: Register is 32-bits wide. */
#define PVR_COREDUMP_REGISTER_FLAG_SIZE_32BIT 2
/** %PVR_COREDUMP_REGISTER_FLAG_SIZE_64BIT: Register is 64-bits wide. */
#define PVR_COREDUMP_REGISTER_FLAG_SIZE_64BIT 3

/**
 * struct pvr_coredump_block_register - PowerVR register dump
 */
struct pvr_coredump_block_register {
	/** @offset: Offset of register. */
	__u32 offset;
	/** @flags: Flags for this register. Combination of %PVR_COREDUMP_REGISTER_FLAG_*. */
	__u32 flags;
	/** @value: Value of register. */
	__u64 value;
};

/** %PVR_COREDUMP_RESET_DATA_FLAG_PF: Set if a page fault happened. */
#define PVR_COREDUMP_RESET_DATA_FLAG_PF _BITUL(0)
/** %PVR_COREDUMP_RESET_DATA_FLAG_ALL_CTXS: Set if reset applicable to all contexts. */
#define PVR_COREDUMP_RESET_DATA_FLAG_ALL_CTXS _BITUL(1)

/** %PVR_COREDUMP_RESET_REASON_NONE: No reset reason recorded. */
#define PVR_COREDUMP_RESET_REASON_NONE 0
/** %PVR_COREDUMP_RESET_REASON_GUILTY_LOCKUP: Caused a reset due to locking up. */
#define PVR_COREDUMP_RESET_REASON_GUILTY_LOCKUP 1
/** %PVR_COREDUMP_RESET_REASON_INNOCENT_LOCKUP: Affected by another context locking up. */
#define PVR_COREDUMP_RESET_REASON_INNOCENT_LOCKUP 2
/** %PVR_COREDUMP_RESET_REASON_GUILTY_OVERRUNING: Overran the global deadline. */
#define PVR_COREDUMP_RESET_REASON_GUILTY_OVERRUNING 3
/** %PVR_COREDUMP_RESET_REASON_INNOCENT_OVERRUNING: Affected by another context overrunning. */
#define PVR_COREDUMP_RESET_REASON_INNOCENT_OVERRUNING 4
/** %PVR_COREDUMP_RESET_REASON_HARD_CONTEXT_SWITCH: Forced reset to meet scheduling requirements. */
#define PVR_COREDUMP_RESET_REASON_HARD_CONTEXT_SWITCH 5
/** %PVR_COREDUMP_RESET_REASON_FW_WATCHDOG: FW Safety watchdog triggered. */
#define PVR_COREDUMP_RESET_REASON_FW_WATCHDOG 12
/** %PVR_COREDUMP_RESET_REASON_FW_PAGEFAULT: FW page fault (no HWR). */
#define PVR_COREDUMP_RESET_REASON_FW_PAGEFAULT 13
/** %PVR_COREDUMP_RESET_REASON_FW_EXEC_ERR: FW execution error (GPU reset requested). */
#define PVR_COREDUMP_RESET_REASON_FW_EXEC_ERR 14
/** %PVR_COREDUMP_RESET_REASON_HOST_WDG_FW_ERR: Host watchdog detected FW error. */
#define PVR_COREDUMP_RESET_REASON_HOST_WDG_FW_ERR 15
/** %PVR_COREDUMP_RESET_REASON_GEOM_OOM_DISABLED: Geometry DM OOM event is not allowed. */
#define PVR_COREDUMP_RESET_REASON_GEOM_OOM_DISABLED 16

/** %PVR_COREDUMP_DM_GP: General purpose Data Master. */
#define PVR_COREDUMP_DM_GP 0
/** %PVR_COREDUMP_DM_2D: 2D Data Master. */
#define PVR_COREDUMP_DM_2D 1
/** %PVR_COREDUMP_DM_GEOM: Geometry Data Master. */
#define PVR_COREDUMP_DM_GEOM 2
/** %PVR_COREDUMP_DM_FRAG: Fragment Data Master. */
#define PVR_COREDUMP_DM_FRAG 3
/** %PVR_COREDUMP_DM_CDM: Compute Data Master. */
#define PVR_COREDUMP_DM_CDM 4
/** %PVR_COREDUMP_DM_RAY: Ray tracing Data Master. */
#define PVR_COREDUMP_DM_RAY 5
/** %PVR_COREDUMP_DM_GEOM2: Geometry 2 Data Master. */
#define PVR_COREDUMP_DM_GEOM2 6
/** %PVR_COREDUMP_DM_GEOM3: Geometry 3 Data Master. */
#define PVR_COREDUMP_DM_GEOM3 7
/** %PVR_COREDUMP_DM_GEOM4: Geometry 4 Data Master. */
#define PVR_COREDUMP_DM_GEOM4 8

/**
 * struct pvr_coredump_block_reset_data - Firmware context reset data
 */
struct pvr_coredump_block_reset_data {
	/** @context_id: FW ID of context affected by the reset */
	__u32 context_id;
	/** @reset_reason: Reason for reset. One of %PVR_COREDUMP_RESET_REASON_*. */
	__u32 reset_reason;
	/** @dm: Data Master affected by the reset. One of %PVR_COREDUMP_DM_. */
	__u32 dm;
	/** @reset_job_ref: Internal job ref running at the time of reset. */
	__u32 reset_job_ref;
	/** @flags: Reset data flags. Combination of %PVR_COREDUMP_RESET_DATA_FLAG_*. */
	__u32 flags;
	/** @padding: Reserved. This field must be zero. */
	__u32 padding;
	/**
	 * @fault_address: Page fault address. Only valid when %PVR_COREDUMP_RESET_DATA_FLAG_PF is
	 *                 set in @flags.
	 */
	__u64 fault_address;
};

/** %PVR_COREDUMP_HWRTYPE_UNKNOWNFAILURE: HWR triggered by unknown failure. */
#define PVR_COREDUMP_HWRTYPE_UNKNOWNFAILURE 0
/** %PVR_COREDUMP_HWRTYPE_OVERRUN: HWR triggered by overrun. */
#define PVR_COREDUMP_HWRTYPE_OVERRUN 1
/** %PVR_COREDUMP_HWRTYPE_POLLFAILURE: HWR triggered by poll timeout. */
#define PVR_COREDUMP_HWRTYPE_POLLFAILURE 2
/** %PVR_COREDUMP_HWRTYPE_BIF0FAULT: HWR triggered by fault from Bus Interface 0. */
#define PVR_COREDUMP_HWRTYPE_BIF0FAULT 3
/** %PVR_COREDUMP_HWRTYPE_BIF1: HWR triggered by fault from Bus Interface 1. */
#define PVR_COREDUMP_HWRTYPE_BIF1FAULT 4
/** %PVR_COREDUMP_HWRTYPE_TEXASBIF0FAULT: HWR triggered by fault from Texas Bus Interface 0. */
#define PVR_COREDUMP_HWRTYPE_TEXASBIF0FAULT 5
/** %PVR_COREDUMP_HWRTYPE_MMUFAULT: HWR triggered by MMU fault. */
#define PVR_COREDUMP_HWRTYPE_MMUFAULT 6
/** %PVR_COREDUMP_HWRTYPE_MMUMETAFAULT: HWR triggered by MMU fault caused by META FW processor. */
#define PVR_COREDUMP_HWRTYPE_MMUMETAFAULT 7
/** %PVR_COREDUMP_HWRTYPE_MIPSTLBFAULT: HWR triggered by TLB fault from MIPS FW processor. */
#define PVR_COREDUMP_HWRTYPE_MIPSTLBFAULT 8
/** %PVR_COREDUMP_HWRTYPE_ECCFAULT: HWR triggered by ECC fault. */
#define PVR_COREDUMP_HWRTYPE_ECCFAULT 9
/** %PVR_COREDUMP_HWRTYPE_MMURISCVFAULT: HWR triggered by MMU fault from RISC-V FW processor. */
#define PVR_COREDUMP_HWRTYPE_MMURISCVFAULT 10

/* DM is working if all flags are cleared */
#define PVR_COREDUMP_HWRINFO_DM_STATE_WORKING 0
/* DM is idle and ready for HWR */
#define PVR_COREDUMP_HWRINFO_DM_STATE_READY_FOR_HWR _BITUL(0)
/* DM need to skip to next cmd before resuming processing */
#define PVR_COREDUMP_HWRINFO_DM_STATE_NEEDS_SKIP _BITUL(2)
/* DM need partial render cleanup before resuming processing */
#define PVR_COREDUMP_HWRINFO_DM_STATE_NEEDS_PR_CLEANUP _BITUL(3)
/* DM need to increment Recovery Count once fully recovered */
#define PVR_COREDUMP_HWRINFO_DM_STATE_NEEDS_TRACE_CLEAR _BITUL(4)
/* DM was identified as locking up and causing HWR */
#define PVR_COREDUMP_HWRINFO_DM_STATE_GUILTY_LOCKUP _BITUL(5)
/* DM was innocently affected by another lockup which caused HWR */
#define PVR_COREDUMP_HWRINFO_DM_STATE_INNOCENT_LOCKUP _BITUL(6)
/* DM was identified as over-running and causing HWR */
#define PVR_COREDUMP_HWRINFO_DM_STATE_GUILTY_OVERRUNING _BITUL(7)
/* DM was innocently affected by another DM over-running which caused HWR */
#define PVR_COREDUMP_HWRINFO_DM_STATE_INNOCENT_OVERRUNING _BITUL(8)
/* DM was forced into HWR as it delayed more important workloads */
#define PVR_COREDUMP_HWRINFO_DM_STATE_HARD_CONTEXT_SWITCH _BITUL(9)
/* DM was forced into HWR due to an uncorrected GPU ECC error */
#define PVR_COREDUMP_HWRINFO_DM_STATE_GPU_ECC_HWR _BITUL(10)

struct pvr_coredump_hwrinfo_bifinfo {
	/** @bif_req_status: Request status for affected BIF. */
	__u64 bif_req_status;
	/** @bif_mmu_status: MMU status for affected BIF. */
	__u64 bif_mmu_status;
};

struct pvr_coredump_hwrinfo_eccinfo {
	/** @fault_gpu: GPU fault information. */
	__u32 fault_gpu;
};

struct pvr_coredump_hwrinfo_mmuinfo {
	/** @mmu_status: MMU status. */
	__u64 mmu_status[2];
};

struct pvr_coredump_hwrinfo_pollinfo {
	/** @thread_num: Number of thread which timed out on a poll. */
	__u32 thread_num;
	/** @cr_poll_addr: Address of timed out poll. */
	__u32 cr_poll_addr;
	/** @cr_poll_mask: Mask of timed out poll. */
	__u32 cr_poll_mask;
	/** @cr_poll_last_value: Last value read from polled location. */
	__u32 cr_poll_last_value;
};

struct pvr_coredump_hwrinfo_tlbinfo {
	/** @bad_addr: Virtual address of failed access. */
	__u32 bad_addr;
	/** @entry_lo: MIPS TLB EntryLo for failed access. */
	__u32 entry_lo;
};

/**
 * struct pvr_coredump_block_hwrinfo - Firmware hardware reset information
 */
struct pvr_coredump_block_hwrinfo {
	/** @hwr_type: Type of HWR event. One of %PVR_COREDUMP_HWRTYPE_*. */
	__u32 hwr_type;
	/** @dm: Data Master affected by the HWR event. One of %PVR_COREDUMP_DM_. */
	__u32 dm;
	/** @core_id: ID of GPU core affected by the HWR event. */
	__u32 core_id;
	/** @event_status: Event status of Data Master. */
	__u32 event_status;
	/** @dm_state: Data Master state. Combination of %PVR_COREDUMP_HWRINFO_DM_STATE_. */
	__u32 dm_state;
	/** @active_hwrt_data: FW address of affected HWRT data. */
	__u32 active_hwrt_data;

	/** @hwr_data: HWR type specific data. Determined by @hwr_type. */
	union {
		/**
		 * @bif_info: Bus Interface specific information.
		 *
		 * Used for %PVR_COREDUMP_HWRTYPE_BIF0FAULT, %PVR_COREDUMP_HWRTYPE_BIF1FAULT,
		 * %PVR_COREDUMP_HWRTYPE_TEXASBIF0FAULT and %PVR_COREDUMP_HWRTYPE_MMURISCVFAULT.
		 */
		struct pvr_coredump_hwrinfo_bifinfo bif_info;

		/**
		 * @mmu_info: MMU specific information.
		 *
		 * Used for %PVR_COREDUMP_HWRTYPE_MMUFAULT and %PVR_COREDUMP_HWRTYPE_MMUMETAFAULT.
		 */
		struct pvr_coredump_hwrinfo_mmuinfo mmu_info;

		/**
		 * @poll_info: Poll timeout specific information.
		 *
		 * Used for %PVR_COREDUMP_HWRTYPE_POLLFAILURE.
		 */
		struct pvr_coredump_hwrinfo_pollinfo poll_info;

		/**
		 * @tlb_info: MIPS TLB specific information.
		 *
		 * Used for %PVR_COREDUMP_HWRTYPE_MIPSTLBFAULT.
		 */
		struct pvr_coredump_hwrinfo_tlbinfo tlb_info;

		/**
		 * @ecc_info: ECC specific information.
		 *
		 * Used for %PVR_COREDUMP_HWRTYPE_ECCFAULT.
		 */
		struct pvr_coredump_hwrinfo_eccinfo ecc_info;
	} hwr_data;
};

#if defined(__cplusplus)
}
#endif

#endif /* __PVR_DRM_H__ */
