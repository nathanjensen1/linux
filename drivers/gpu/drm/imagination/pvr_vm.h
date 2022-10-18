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
 *    structure. In the current implementation, this value is pegged to the
 *    CPU page size (%PAGE_SIZE). It is therefore an error to specify a CPU
 *    page size which is not also a supported device page size. The supported
 *    device page sizes are: 4KiB, 16KiB, 64KiB, 256KiB, 1MiB and 2MiB.
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

#define PVR_SHIFT_FROM_SIZE(size_) (__builtin_ctzll(size_))
#define PVR_MASK_FROM_SIZE(size_) (~((size_) - U64_C(1)))

/* PVR_DEVICE_PAGE_SIZE determines the page size */
#define PVR_DEVICE_PAGE_SIZE (PAGE_SIZE)
#define PVR_DEVICE_PAGE_SHIFT (PAGE_SHIFT)
#define PVR_DEVICE_PAGE_MASK (PAGE_MASK)

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
	 * @static_data_carveout_base: Base address of the static data carveout if present, or
	 * zero otherwise.
	 */
	u64 static_data_carveout_base;

	/**
	 * @static_data_carveout_size: Size of the static data carveout in bytes if present, or
	 * zero otherwise.
	 */
	u64 static_data_carveout_size;

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

struct pvr_vm_context *pvr_vm_create_context(struct pvr_device *pvr_dev,
					     bool create_fw_mem_ctx);

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
u32 pvr_get_num_heaps(struct pvr_device *pvr_dev);

struct pvr_gem_object *pvr_vm_find_gem_object(struct pvr_vm_context *vm_ctx,
					      u64 device_addr,
					      u64 *mapped_offset_out,
					      u64 *mapped_size_out);

int
pvr_vm_mmu_flush(struct pvr_device *pvr_dev);

struct pvr_fw_object *
pvr_vm_get_fw_mem_context(struct pvr_vm_context *vm_ctx);

struct pvr_vm_context *
pvr_vm_context_get(struct pvr_vm_context *vm_ctx);
bool pvr_vm_context_put(struct pvr_vm_context *vm_ctx);
void pvr_vm_context_teardown_mappings(struct pvr_vm_context *vm_ctx, bool enable_warnings);

#endif /* __PVR_VM_H__ */
