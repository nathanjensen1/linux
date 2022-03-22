/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_DRM_H__
#define __PVR_DRM_H__

#include "drm.h"

#include <linux/const.h>
#include <linux/types.h>

/**
 * DOC: PowerVR UAPI
 *
 * TODO
 */

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
#define DRM_IOCTL_PVR_CREATE_OBJECT PVR_IOCTL(0x05, DRM_IOWR, create_object)
#define DRM_IOCTL_PVR_DESTROY_OBJECT PVR_IOCTL(0x06, DRM_IOW, destroy_object)
#define DRM_IOCTL_PVR_GET_HEAP_INFO PVR_IOCTL(0x07, DRM_IOWR, get_heap_info)
#define DRM_IOCTL_PVR_VM_MAP PVR_IOCTL(0x08, DRM_IOW, vm_map)
#define DRM_IOCTL_PVR_VM_UNMAP PVR_IOCTL(0x09, DRM_IOW, vm_unmap)
#define DRM_IOCTL_PVR_SUBMIT_JOB PVR_IOCTL(0x0a, DRM_IOW, submit_job)
/* clang-format on */

/**
 * DOC: IOCTL CREATE_BO
 *
 * TODO
 */

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
 * DOC: IOCTL GET_BO_MMAP_OFFSET
 *
 * TODO
 */

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
 * DOC: IOCTL GET_PARAM
 *
 * TODO
 */

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
 * DOC: IOCTL CREATE_CONTEXT
 *
 * TODO
 */

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

/**
 * enum drm_pvr_static_render_context_state_format - Arguments for
 * &drm_pvr_static_render_context_state.format
 */
enum drm_pvr_static_render_context_state_format {
	/** @DRM_PVR_SRCS_FORMAT_1: Format 1, used by firmware version 1.14. */
	DRM_PVR_SRCS_FORMAT_1 = 0,
};

/**
 * struct drm_pvr_static_render_context_state - Static render context state
 * arguments for %DRM_IOCTL_PVR_CREATE_CONTEXT
 */
struct drm_pvr_static_render_context_state {
	/**
	 * @format: [IN] Format of @data.
	 *
	 * This must be one of the values defined by
	 * &enum drm_pvr_static_render_context_state_format.
	 *
	 * For firmware version 1.14, this is %DRM_PVR_SRCS_FORMAT_1.
	 */
	__u32 format;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;

	/** @data: [IN] Static render context state data. */
	union {
		/**
		 * @format_1: Static render context state data when @format ==
		 *            %DRM_PVR_SRCS_FORMAT_1.
		 */
		struct {
			__u64 geom_reg_vdm_context_state_base_addr;
			__u64 geom_reg_vdm_context_state_resume_addr;
			__u64 geom_reg_ta_context_state_base_addr;

			struct {
				__u64 geom_reg_vdm_context_store_task0;
				__u64 geom_reg_vdm_context_store_task1;
				__u64 geom_reg_vdm_context_store_task2;

				__u64 geom_reg_vdm_context_resume_task0;
				__u64 geom_reg_vdm_context_resume_task1;
				__u64 geom_reg_vdm_context_resume_task2;

				__u64 geom_reg_vdm_context_store_task3;
				__u64 geom_reg_vdm_context_store_task4;

				__u64 geom_reg_vdm_context_resume_task3;
				__u64 geom_reg_vdm_context_resume_task4;
			} geom_state[2];
		} format_1;
	} data;
};

/**
 * enum drm_pvr_static_compute_context_state_format - Arguments for
 * &drm_pvr_static_compute_context_state.format
 */
enum drm_pvr_static_compute_context_state_format {
	/** @DRM_PVR_SCCS_FORMAT_1: Format 1, used by firmware version 1.14. */
	DRM_PVR_SCCS_FORMAT_1 = 0,
};

/**
 * struct drm_pvr_static_compute_context_state - Static compute context state
 * arguments for %DRM_IOCTL_PVR_CREATE_CONTEXT
 */
struct drm_pvr_static_compute_context_state {
	/**
	 * @format: [IN] Format of @data.
	 *
	 * This must be one of the values defined by
	 * &enum drm_pvr_static_compute_context_state_format.
	 *
	 * For firmware version 1.14, this is %DRM_PVR_SCCS_FORMAT_1.
	 */
	__u32 format;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;

	/** @data: [IN] Static compute context state data. */
	union {
		/**
		 * @format_1: Static compute context state data when @format ==
		 *            %DRM_PVR_SCCS_FORMAT_1.
		 */
		struct {
			__u64 cdmreg_cdm_context_state_base_addr;
			__u64 cdmreg_cdm_context_pds0;
			__u64 cdmreg_cdm_context_pds1;
			__u64 cdmreg_cdm_terminate_pds;
			__u64 cdmreg_cdm_terminate_pds1;
			__u64 cdmreg_cdm_resume_pds0;
			__u64 cdmreg_cdm_context_pds0_b;
			__u64 cdmreg_cdm_resume_pds0_b;
		} format_1;
	} data;
};

/**
 * struct drm_pvr_ioctl_create_render_context_args - Arguments for
 * drm_pvr_ioctl_create_context_args.args.render.
 */
struct drm_pvr_ioctl_create_render_context_args {
	/**
	 * @vdm_callstack_addr: [IN] Address for initial TA call stack pointer.
	 */
	__u64 vdm_callstack_addr;

	/**
	 * @static_render_context_state: [IN] Pointer to static render context
	 *                                    state to copy to new context.
	 */
	__u64 static_render_context_state;
};

/**
 * struct drm_pvr_ioctl_create_compute_context_args - Arguments for %DRM_PVR_CTX_COMPUTE.
 */
struct drm_pvr_ioctl_create_compute_context_args {
	/**
	 * @static_compute_context_state: [IN] Pointer to static compute context state to copy to
	 *                                     new context.
	 */
	__u64 static_compute_context_state;
};

/**
 * enum drm_pvr_reset_framework_format - Arguments for
 * &drm_pvr_reset_framework.format
 */
enum drm_pvr_reset_framework_format {
	/** @DRM_PVR_RF_FORMAT_CDM_1: Format 1, used by firmware 1.14. */
	DRM_PVR_RF_FORMAT_CDM_1 = 0,
};

/**
 * struct drm_pvr_reset_framework - Reset framework arguments for
 * %DRM_IOCTL_PVR_CREATE_CONTEXT
 */
struct drm_pvr_reset_framework {
	/**
	 * @flags: [IN] Flags for reset framework.
	 *
	 * This is currently unused and must be set to 0.
	 */
	__u32 flags;

	/**
	 * @format: [IN] Format of @data.
	 *
	 * This must be one of the values defined by
	 * &enum drm_pvr_reset_framework_format.
	 *
	 * For firmware version 1.14, this is %DRM_PVR_RF_FORMAT_CDM_1.
	 */
	__u32 format;

	/** @data: [IN] Reset framework data. */
	union {
		/**
		 * @cdm_format_1: Reset framework data when @format ==
		 *                %DRM_PVR_RF_FORMAT_CDM_1.
		 */
		struct {
			/**
			 * @cdm_ctrl_stream_base: Base address of CDM control
			 *                        stream
			 */
			__u64 cdm_ctrl_stream_base;
		} cdm_format_1;
	} data;
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
	 * @reset_framework_registers: [IN] Pointer to reset framework
	 *                             registers.
	 *
	 * May be 0 to indicate no reset framework.
	 */
	__u64 reset_framework_registers;

	/**
	 * @priority: [IN] Priority of new context.
	 *
	 * This must be one of the values defined by &enum drm_pvr_ctx_priority.
	 */
	__s32 priority;

	/** @handle: [OUT] Handle for new context. */
	__u32 handle;

	/** @data: [IN] User pointer to context type specific arguments. */
	__u64 data;
};

/**
 * DOC: IOCTL DESTROY_CONTEXT
 *
 * TODO
 */

/**
 * struct drm_pvr_ioctl_destroy_context_args - Arguments for
 * %DRM_IOCTL_PVR_DESTROY_CONTEXT
 */
struct drm_pvr_ioctl_destroy_context_args {
	/**
	 * @handle: [IN] Handle for context to be destroyed.
	 */
	__u32 handle;
};

/**
 * DOC: IOCTL CREATE_OBJECT
 *
 * TODO
 */

/* clang-format off */

/**
 * enum drm_pvr_object_type - Arguments for
 * &drm_pvr_ioctl_create_object_args.type
 */
enum drm_pvr_object_type {
	/**
	 * @DRM_PVR_OBJECT_TYPE_FREE_LIST: Free list object. Use &struct
	 * drm_pvr_ioctl_create_free_list_args for object creation arguments.
	 */
	DRM_PVR_OBJECT_TYPE_FREE_LIST = 0,
	/**
	 * @DRM_PVR_OBJECT_TYPE_HWRT_DATASET: HWRT data set. Use &struct
	 * drm_pvr_ioctl_create_hwrt_dataset_args for object creation arguments.
	 */
	DRM_PVR_OBJECT_TYPE_HWRT_DATASET,
};

/* clang-format on */

/**
 * struct drm_pvr_ioctl_create_free_list_args - Arguments for
 * %DRM_PVR_OBJECT_TYPE_FREE_LIST
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
};

struct create_hwrt_geom_data_args {
	/** @tail_ptrs_dev_addr: [IN] Tail pointers GPU virtual address. */
	__u64 tail_ptrs_dev_addr;

	/** @vheap_table_dev_addr: [IN] VHEAP table GPU virtual address. */
	__u64 vheap_table_dev_addr;

	/** @rtc_dev_addr: [IN] Render Target Cache virtual address. */
	__u64 rtc_dev_addr;
};

struct create_hwrt_rt_data_args {
	/** @pm_mlist_dev_addr: [IN] PM MLIST GPU virtual address. */
	__u64 pm_mlist_dev_addr;

	/** @macrotile_array_dev_addr: [IN] Macrotile array GPU virtual address. */
	__u64 macrotile_array_dev_addr;

	/** @region_header_dev_addr: [IN] Region header GPU virtual address. */
	__u64 region_header_dev_addr;
};

struct create_hwrt_free_list_args {
	/** @free_list_handle: [IN] Free list object handle. */
	__u32 free_list_handle;
};

struct drm_pvr_ioctl_create_hwrt_dataset_args {
	/**
	 * @geom_data_args: [IN] User pointer to one or more &struct
	 *                       create_hwrt_geom_data_args.
	 *
	 * Number of &struct create_hwrt_geom_data_args must be equal to
	 * &num_geom_datas.
	 */
	__u64 geom_data_args;

	/**
	 * @rt_data_args: [IN] User pointer to one or more &struct
	 *                     create_hwrt_rt_data_args.
	 *
	 * Number of &struct create_hwrt_rt_data_args must be equal to
	 * &num_rt_datas.
	 */
	__u64 rt_data_args;

	/**
	 * @free_list_args: [IN] User pointer to one or more &struct
	 *                       create_hwrt_free_list_args.
	 *
	 * Number of &struct create_hwrt_free_list_args must be equal to
	 * &num_free_lists.
	 */
	__u64 free_list_args;

	/**
	 * @num_geom_datas: [IN] Number of geom data arguments.
	 *
	 * This should be equal to the value returned for
	 * %DRM_PVR_PARAM_HWRT_NUM_GEOMDATAS by %DRM_IOCTL_PVR_GET_PARAM.
	 */
	__u32 num_geom_datas;

	/**
	 * @num_rt_datas: [IN] Number of rt data arguments.
	 *
	 * This should be equal to the value returned for
	 * %DRM_PVR_PARAM_HWRT_NUM_RTDATAS by %DRM_IOCTL_PVR_GET_PARAM.
	 */
	__u32 num_rt_datas;

	/**
	 * @num_free_lists: [IN] Number of free list arguments.
	 *
	 * This should be equal to the value returned for
	 * %DRM_PVR_PARAM_HWRT_NUM_FREELISTS by %DRM_IOCTL_PVR_GET_PARAM.
	 */
	__u32 num_free_lists;

	/** @mtile_stride: [IN] Macrotile stride. */
	__u32 mtile_stride;

	/** @region_header_size: [IN] Region header size. */
	__u64 region_header_size;

	/** @flipped_multi_sample_control: [IN] Flipped multi sample control. */
	__u64 flipped_multi_sample_control;

	/** @multi_sample_control: [IN] Multi sample control. */
	__u64 multi_sample_control;

	/** @screen_pixel_max: [IN] Maximum screen size. */
	__u32 screen_pixel_max;

	/** @te_aa: [IN] TE anti-aliasing. */
	__u32 te_aa;

	/** @te_mtile: [IN] TE macrotile boundaries. */
	__u32 te_mtile[2];

	/** @te_screen_size: [IN] TE screen size. */
	__u32 te_screen_size;

	/** @tpc_size: [IN] Tail Pointer Cache size */
	__u32 tpc_size;

	/** @tpc_stride: [IN] Tail Pointer Cache stride */
	__u32 tpc_stride;

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

	/** @isp_mtile_size: [IN] Macrotile size. */
	__u32 isp_mtile_size;

	/** @max_rts: [IN] Maximum Render Targets. */
	__u16 max_rts;

	/** @_padding_7a: Reserved. This field must be zeroed. */
	__u16 _padding_7a;

	/** @_padding_7c: Reserved. This field must be zeroed. */
	__u32 _padding_7c;
};

/**
 * struct drm_pvr_ioctl_create_object_args - Arguments for
 * %DRM_IOCTL_PVR_CREATE_OBJECT
 */
struct drm_pvr_ioctl_create_object_args {
	/**
	 * @type: [IN] Type of object to create.
	 *
	 * This must be one of the values defined by &enum drm_pvr_object_type.
	 */
	__u32 type;

	/**
	 * @handle: [OUT] Handle for created object.
	 */
	__u32 handle;

	/** @data: [IN] User pointer to object type specific arguments. */
	__u64 data;
};

/**
 * DOC: IOCTL DESTROY_OBJECT
 *
 * TODO
 */

/**
 * struct drm_pvr_ioctl_destroy_object_args - Arguments for
 * %DRM_IOCTL_PVR_DESTROY_OBJECT
 */
struct drm_pvr_ioctl_destroy_object_args {
	/**
	 * @handle: [IN] Handle for object to be destroyed.
	 */
	__u32 handle;
};

/**
 * DOC: IOCTL GET_HEAP_INFO
 *
 * TODO
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
};

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
	 * @reserved_base: Base address of reserved area.
	 *
	 * The reserved area must be located at the beginning or end of the heap. Any other location
	 * is invalid and should be rejected by the caller.
	 */
	__u64 reserved_base;

	/**
	 * @reserved_size: Size of reserved area, in bytes. May be 0 if this
	 *                 heap has no reserved area.
	 */
	__u64 reserved_size;

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
	DRM_PVR_STATIC_DATA_AREA_EOT = 0,
	DRM_PVR_STATIC_DATA_AREA_FENCE,
	DRM_PVR_STATIC_DATA_AREA_VDM_SYNC,
	DRM_PVR_STATIC_DATA_AREA_YUV_CSC,
};

struct drm_pvr_static_data_area {
	/** @id: ID of static data area. */
	__u32 id;

	/** @size: Size of static data area. */
	__u32 size;

	/** @offset: Offset of static data area from start of reserved area. */
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
	 *             May be zero, in which case this ioctl will not write any heap information.
	 */
	__u64 data;

	/** @heap_nr: [IN] Number of heap to get information for. Not used if @data is 0. */
	__u32 heap_nr;

	/**
	 * @nr_heaps: [OUT] Number of heaps provided by the driver.
	 */
	__u32 nr_heaps;
};

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
 * enum drm_pvr_cmd_geom_format - Arguments for
 * &drm_pvr_cmd_geom.format
 */
enum drm_pvr_cmd_geom_format {
	DRM_PVR_CMD_GEOM_FORMAT_1 = 0,
};

/**
 * struct drm_pvr_geom_regs_format_1 - Configuration registers which need to be loaded by the
 *                                     firmware before the VDM can be started
 *
 * Valid for %DRM_PVR_CMD_GEOM_FORMAT_1.
 */
struct drm_pvr_geom_regs_format_1 {
	__u64 vdm_ctrl_stream_base;
	__u64 tpu_border_colour_table;
	__u32 ppp_ctrl;
	__u32 te_psg;
	__u32 tpu;
	__u32 vdm_context_resume_task0_size;
	__u32 pds_ctrl;
	__u32 view_idx;
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
#define DRM_PVR_SUBMIT_JOB_GEOM_CMD_SINGLE_CORE _BITULL(3)
#define DRM_PVR_SUBMIT_JOB_GEOM_CMD_FLAGS_MASK                                 \
	(DRM_PVR_SUBMIT_JOB_GEOM_CMD_FIRST |                                   \
	 DRM_PVR_SUBMIT_JOB_GEOM_CMD_LAST |                                    \
	 DRM_PVR_SUBMIT_JOB_GEOM_CMD_SINGLE_CORE)

/**
 * struct drm_pvr_cmd_geom_format_1 - structure representing a geometry command, for format
 *                                    %DRM_PVR_CMD_GEOM_FORMAT_1
 */
struct drm_pvr_cmd_geom_format_1 {
	/** @frame_num: Associated frame number. */
	__u32 frame_num;

	/** @flags: command control flags. */
	__u32 flags;

	/**
	 * @regs: Configuration registers which need to be loaded by the
	 *        firmware before the VDM can be started.
	 */
	struct drm_pvr_geom_regs_format_1 geom_regs;
};

/**
 * struct drm_pvr_cmd_geom - structure representing a geometry command
 */
struct drm_pvr_cmd_geom {
	/**
	 * @format: [IN] Format of @data.
	 *
	 * This must be one of the values defined by
	 * &enum drm_pvr_cmd_geom_format.
	 *
	 * For firmware version 1.14, this is %DRM_PVR_CMD_GEOM_FORMAT_1.
	 */
	__u32 format;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;

	/** @data: [IN] Geometry job data. */
	union {
		/**
		 * @cmd_frag_format_1: Command data when @format ==
		 *                     %DRM_PVR_CMD_GEOM_FORMAT_1.
		 */
		struct drm_pvr_cmd_geom_format_1 cmd_geom_format_1;
	} data;
};

/**
 * enum drm_pvr_cmd_frag_format - Arguments for
 * &drm_pvr_cmd_frag.format
 */
enum drm_pvr_cmd_frag_format {
	DRM_PVR_CMD_FRAG_FORMAT_1 = 0,
};

/**
 * struct drm_pvr_frag_regs_format_1 - Configuration registers which need to be loaded by the
 *                                     firmware before the ISP can be started
 *
 * Valid for format %DRM_PVR_CMD_FRAG_FORMAT_1.
 */
struct drm_pvr_frag_regs_format_1 {
#define PVR_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL 8U
	__u32 usc_clear_register[PVR_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL];
	__u32 usc_pixel_output_ctrl;
	__u32 isp_bgobjdepth;
	__u32 isp_bgobjvals;
	__u32 isp_aa;
	__u32 isp_ctl;
	__u32 tpu;
	__u32 event_pixel_pds_info;
	__u32 pixel_phantom;
	__u32 view_idx;
	__u32 event_pixel_pds_data;
	__u64 isp_scissor_base;
	__u64 isp_dbias_base;
	__u64 isp_oclqry_base;
	__u64 isp_zlsctl;
	__u64 isp_zload_store_base;
	__u64 isp_stencil_load_store_base;
	__u64 isp_zls_pixels;
#define PVR_PBE_WORDS_REQUIRED_FOR_RENDERS 2U
	__u64 pbe_word[8][PVR_PBE_WORDS_REQUIRED_FOR_RENDERS];
	__u64 tpu_border_colour_table;
	__u64 pds_bgnd[3];
	__u64 pds_pr_bgnd[3];
};

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
 * .. c:macro:: DRM_PVR_SUBMIT_JOB_FRAG_CMD_FLAGS_MASK
 *
 *    Logical OR of all the fragment cmd flags.
 */
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_SINGLE_CORE _BITULL(3)
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_DEPTHBUFFER _BITULL(7)
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_STENCILBUFFER _BITULL(8)
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_PREVENT_CDM_OVERLAP _BITULL(26)
#define DRM_PVR_SUBMIT_JOB_FRAG_CMD_FLAGS_MASK                                 \
	(DRM_PVR_SUBMIT_JOB_FRAG_CMD_SINGLE_CORE |                             \
	 DRM_PVR_SUBMIT_JOB_FRAG_CMD_DEPTHBUFFER |                             \
	 DRM_PVR_SUBMIT_JOB_FRAG_CMD_STENCILBUFFER |                           \
	 DRM_PVR_SUBMIT_JOB_FRAG_CMD_PREVENT_CDM_OVERLAP)

/**
 * struct drm_pvr_cmd_frag_format_1 - structure representing a fragment command, for format
 *                                    %DRM_PVR_CMD_FRAG_FORMAT_1
 */
struct drm_pvr_cmd_frag_format_1 {
	/** @frame_num: Associated frame number. */
	__u32 frame_num;

	/** @flags: command control flags. */
	__u32 flags;

	/** @zls_stride: Stride IN BYTES for Z-Buffer in case of RTAs. */
	__u32 zls_stride;

	/** @sls_stride: Stride IN BYTES for S-Buffer in case of RTAs. */
	__u32 sls_stride;

	/**
	 * @regs: Configuration registers which need to be loaded by the
	 *        firmware before the ISP can be started.
	 */
	struct drm_pvr_frag_regs_format_1 regs;
};

/**
 * struct drm_pvr_cmd_frag - structure representing a fragment command
 */
struct drm_pvr_cmd_frag {
	/**
	 * @format: [IN] Format of @data.
	 *
	 * This must be one of the values defined by
	 * &enum drm_pvr_cmd_frag_format.
	 *
	 * For firmware version 1.14, this is %DRM_PVR_CMD_FRAG_FORMAT_1.
	 */
	__u32 format;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;

	/** @data: [IN] Fragment job data. */
	union {
		/**
		 * @cmd_frag_format_1: Command data when @format ==
		 *                     %DRM_PVR_CMD_FRAG_FORMAT_1.
		 */
		struct drm_pvr_cmd_frag_format_1 cmd_frag_format_1;
	} data;
};

/**
 * enum drm_pvr_cmd_compute_format - Arguments for
 * &drm_pvr_cmd_compute.format
 */
enum drm_pvr_cmd_compute_format {
	DRM_PVR_CMD_COMPUTE_FORMAT_1 = 0,
};

/**
 * struct drm_pvr_compute_regs_format_1 - Configuration registers which need to be loaded by the
 *                                        firmware before the CDM can be started
 *
 * Valid for format %DRM_PVR_CMD_COMPUTE_FORMAT_1.
 */
struct drm_pvr_compute_regs_format_1 {
	__u64 tpu_border_colour_table;
	__u64 cdm_item;
	__u64 compute_cluster;
	__u64 cdm_ctrl_stream_base;
	__u32 tpu;
	__u32 cdm_resume_pds1;
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
#define DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP _BITULL(1)
#define DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE _BITULL(5)
#define DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_FLAGS_MASK         \
	(DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP | \
	 DRM_PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE)

/**
 * struct drm_pvr_cmd_compute_format_1 - structure representing a compute command, for format
 *                                       %DRM_PVR_CMD_COMPUTE_FORMAT_1
 */
struct drm_pvr_cmd_compute_format_1 {
	/** @frame_num: Associated frame number. */
	__u32 frame_num;

	/** @flags: command control flags. */
	__u32 flags;

	/**
	 * @regs: Configuration registers which need to be loaded by the
	 *        firmware before the CDM can be started.
	 */
	struct drm_pvr_compute_regs_format_1 regs;
};

/**
 * struct drm_pvr_cmd_compute - structure representing a compute command
 */
struct drm_pvr_cmd_compute {
	/**
	 * @format: [IN] Format of @data.
	 *
	 * This must be one of the values defined by
	 * &enum drm_pvr_cmd_compute_format.
	 *
	 * For firmware version 1.14, this is %DRM_PVR_CMD_COMPUTE_FORMAT_1.
	 */
	__u32 format;

	/** @_padding_4: Reserved. This field must be zeroed. */
	__u32 _padding_4;

	/** @data: [IN] Compute job data. */
	union {
		/**
		 * @cmd_compute_format_1: Command data when @format ==
		 *                        %DRM_PVR_CMD_COMPUTE_FORMAT_1.
		 */
		struct drm_pvr_cmd_compute_format_1 cmd_compute_format_1;
	} data;
};

/**
 * DOC: Flags for &struct drm_pvr_bo_ref.flags
 *
 * DRM_PVR_BO_REF_READ - This buffer object will be read by the device.
 *
 * DRM_PVR_BO_REF_WRITE - This buffer object will be written to by the device.
 */
#define DRM_PVR_BO_REF_READ _BITUL(0)
#define DRM_PVR_BO_REF_WRITE _BITUL(1)

/**
 * struct drm_pvr_bo_ref - structure representing a DRM buffer object
 */
struct drm_pvr_bo_ref {
	/** @handle: DRM buffer object handle. */
	__u32 handle;

	/** @flags: Flags for this buffer object. Must be a combination of %DRM_PVR_BO_REF_*. */
	__u32 flags;
};

/*
 * struct drm_pvr_job_render_args - Arguments for %DRM_PVR_JOB_TYPE_RENDER
 */
struct drm_pvr_job_render_args {
	/**
	 * @cmd_geom: [IN] Pointer to &struct drm_pvr_cmd_geom, representing geometry command.
	 *                 May be zero.
	 */
	__u64 cmd_geom;

	/**
	 * @cmd_frag: [IN] Pointer to &struct drm_pvr_cmd_frag, representing fragment command.
	 *                 May be zero.
	 */
	__u64 cmd_frag;

	/**
	 * @cmd_frag_pr: [IN] Pointer to &struct drm_pvr_cmd_frag, representing fragment PR command.
	 *                    May be zero.
	 */
	__u64 cmd_frag_pr;

	/**
	 * @in_syncobj_handles_geom: [IN] Pointer to array of drm_syncobj handles for
	 *                                input fences for geometry job.
	 *
	 * This array must be &num_in_syncobj_handles_geom entries large.
	 */
	__u64 in_syncobj_handles_geom;

	/**
	 * @in_syncobj_handles_frag: [IN] Pointer to array of drm_syncobj handles for
	 *                                input fences for fragment job.
	 *
	 * This array must be &num_in_syncobj_handles_frag entries large.
	 */
	__u64 in_syncobj_handles_frag;

	/**
	 * @in_syncobj_handles_frag_pr: [IN] Pointer to array of drm_syncobj handles for
	 *                                   input fences for fragment PR job.
	 *
	 * This array must be &num_in_syncobj_handles_frag_pr entries large.
	 */
	__u64 in_syncobj_handles_frag_pr;

	/**
	 * @bo_handles: [IN] Pointer to array of struct drm_pvr_bo_ref.
	 *
	 * This array must be &num_bo_handles entries large.
	 */
	__u64 bo_handles;

	/**
	 * @num_in_syncobj_handles_geom: [IN] Number of input syncobj handles for geometry job.
	 */
	__u32 num_in_syncobj_handles_geom;

	/**
	 * @num_in_syncobj_handles_frag: [IN] Number of input syncobj handles for fragment job.
	 */
	__u32 num_in_syncobj_handles_frag;

	/**
	 * @num_in_syncobj_handles_frag_pr: [IN] Number of input syncobj handles for fragment PR
	 *                                       job.
	 */
	__u32 num_in_syncobj_handles_frag_pr;

	/**
	 * @num_bo_handles: [IN] Number of DRM Buffer Objects.
	 *
	 * This detemines the size of the array &bo_handles points to.
	 */
	__u32 num_bo_handles;

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
	 * @msaa_scratch_buffer_handle: [IN] Handle for MSAA scratch buffer.
	 */
	__u32 msaa_scratch_buffer_handle;

	/**
	 * @zs_buffer_handle: [IN] Handle for Z/stencil buffer.
	 */
	__u32 zs_buffer_handle;
};

/*
 * struct drm_pvr_job_compute_args - Arguments for %DRM_PVR_JOB_TYPE_COMPUTE
 */
struct drm_pvr_job_compute_args {
	/**
	 * @cmd: [IN] Pointer to &struct drm_pvr_cmd_compute, representing compute command.
	 */
	__u64 cmd;

	/**
	 * @in_syncobj_handles: [IN] Pointer to array of drm_syncobj handles for input fences.
	 *
	 * This array must be &num_in_syncobj_handles entries large.
	 */
	__u64 in_syncobj_handles;

	/**
	 * @bo_handles: [IN] Pointer to array of struct drm_pvr_bo_ref.
	 *
	 * This array must be &num_bo_handles entries large.
	 */
	__u64 bo_handles;

	/**
	 * @num_in_syncobj_handles: [IN] Number of input syncobj handles.
	 */
	__u32 num_in_syncobj_handles;

	/**
	 * @num_bo_handles: [IN] Number of DRM Buffer Objects.
	 *
	 * This detemines the size of the array &bo_handles points to.
	 */
	__u32 num_bo_handles;

	/**
	 * @out_syncobj: [OUT] drm_syncobj handle for output fence
	 */
	__u32 out_syncobj;

	/** @_padding_24: Reserved. This field must be zeroed. */
	__u32 _padding_24;
};

/**
 * enum drm_pvr_job_type - Arguments for &drm_pvr_ioctl_submit_job_args.job_type
 */
enum drm_pvr_job_type {
	DRM_PVR_JOB_TYPE_RENDER = 0,
	DRM_PVR_JOB_TYPE_COMPUTE,
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
	 * This must be a valid handle returned by %DRM_IOCTL_PVR_CREATE_CONTEXT. The type of
	 * context must be compatible with the type of job being submitted.
	 */
	__u32 context_handle;

	/**
	 * @ext_job_ref: [IN] Job reference.
	 */
	__u32 ext_job_ref;

	/** @_padding_c: Reserved. This field must be zeroed. */
	__u32 _padding_c;

	/** @data: [IN] User pointer to job type specific arguments. */
	__u64 data;
};

#if defined(__cplusplus)
}
#endif

#endif /* __PVR_DRM_H__ */
