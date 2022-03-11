/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_VM_H__
#define __PVR_VM_H__

#include "pvr_rogue_mmu_defs.h"

#include <uapi/drm/pvr_drm.h>

#include <linux/bitops.h>
#include <linux/types.h>

/* Forward declaration from "pvr_device.h" */
struct pvr_device;
struct pvr_file;

/* Forward declaration from "pvr_gem.h" */
struct pvr_gem_object;

/* Forward declaration from "pvr_vm.c" */
struct pvr_vm_context;

/* Forward declaration from <uapi/drm/pvr_drm.h> */
struct drm_pvr_ioctl_get_heap_info_args;

/**
 * DOC: Public API (constants)
 *
 * .. c:macro:: PVR_DEVICE_PAGE_SIZE
 *
 *    Fixed page size referenced by leaf nodes in the page table tree
 *    structure.
 *
 *    .. admonition:: Future work
 *
 *       The PowerVR device MMU supports multiple page sizes (6 of them!).
 *       While we currently only support 4KiB pages (the smallest), this
 *       constant (as well as the two derived values %PVR_DEVICE_PAGE_SHIFT and
 *       %PVR_DEVICE_PAGE_MASK) may have to become a lookup table at some point
 *       to support some or all of the additional page sizes.
 *
 * .. c:macro:: PVR_DEVICE_PAGE_SHIFT
 *
 *    Shift value used to efficiently multiply or divide by
 *    %PVR_DEVICE_PAGE_SIZE.
 *
 *    This value is derived from %PVR_DEVICE_PAGE_SIZE.
 *
 * .. c:macro:: PVR_DEVICE_PAGE_MASK
 *
 *    Mask used to round a value down to the nearest multiple of
 *    %PVR_DEVICE_PAGE_SIZE. When bitwise negated, it will indicate whether a
 *    value is already a multiple of %PVR_DEVICE_PAGE_SIZE.
 *
 *    This value is derived from %PVR_DEVICE_PAGE_SIZE.
 */

/* PVR_DEVICE_PAGE_SIZE determines the page size */
#define PVR_DEVICE_PAGE_SIZE (SZ_4K)
#define PVR_DEVICE_PAGE_SHIFT (__ffs(PVR_DEVICE_PAGE_SIZE))
#define PVR_DEVICE_PAGE_MASK (~(PVR_DEVICE_PAGE_SIZE - 1))

struct pvr_heap {
	/** @id: Heap ID. */
	enum drm_pvr_heap_id id;

	/** @flags: Flags for this heap. Currently always 0. */
	u32 flags;

	/** @base: Base address of heap. */
	u64 base;

	/** @size: Size of heap, in bytes. */
	u64 size;

	/**
	 * @reserved_base: Base address of the reserved area if present, or
	 * zero otherwise.
	 */
	u64 reserved_base;

	/**
	 * @reserved_size: Size of the reserved area in bytes if present, or
	 * zero otherwise.
	 */
	u64 reserved_size;

	/** @page_size_log2: Log2 of page size. */
	u32 page_size_log2;

	/** @nr_static_data_areas: Number of static data areas in this heap. */
	u32 nr_static_data_areas;

	/**
	 * @static_data_areas: Pointer to description of static data areas in
	 * this heap. If @nr_static_data_areas is zero, this should be %NULL.
	 */
	const struct drm_pvr_static_data_area *static_data_areas;
};

/* Functions defined in pvr_vm.c */

bool pvr_device_addr_is_valid(u64 device_addr);
bool pvr_device_addr_and_size_are_valid(u64 device_addr, u64 size);

struct pvr_vm_context *pvr_vm_create_context(struct pvr_device *pvr_dev);
void pvr_vm_destroy_context(struct pvr_vm_context *vm_ctx, bool enable_warnings);

int pvr_vm_map(struct pvr_vm_context *vm_ctx, struct pvr_gem_object *pvr_obj,
	       u64 device_addr);
int pvr_vm_map_partial(struct pvr_vm_context *vm_ctx,
		       struct pvr_gem_object *pvr_obj, u64 pvr_obj_offset,
		       u64 device_addr, u64 size);
int pvr_vm_unmap(struct pvr_vm_context *vm_ctx, u64 device_addr);

dma_addr_t pvr_vm_get_page_table_root_addr(struct pvr_vm_context *vm_ctx);

int pvr_get_heap_info(struct pvr_device *pvr_dev,
		      struct drm_pvr_ioctl_get_heap_info_args *args);
const struct pvr_heap *pvr_find_heap_containing(struct pvr_device *pvr_dev,
						u64 addr, u64 size);

struct pvr_gem_object *pvr_vm_find_gem_object(struct pvr_vm_context *vm_ctx,
					      u64 device_addr,
					      u64 *mapped_offset_out,
					      u64 *mapped_size_out);

int
pvr_vm_mmu_flush(struct pvr_device *pvr_dev);

#endif /* __PVR_VM_H__ */
