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

/* Forward declaration of opaque handle from "pvr_vm.c" */
struct pvr_vm_context;

/* Forward declaration from uapi/drm/pvr_drm.h. */
struct drm_pvr_ioctl_get_heap_info_args;

/**
 * DOC: Public API
 *
 * The public-facing API of our virtual memory management is exposed as 7
 * functions (well, 6 and a helper) along with an opaque handle type.
 *
 * The opaque handle is &struct pvr_vm_context. This holds a "global state",
 * including a complete page table tree structure. You do not need to consider
 * this internal structure (or anything else in &struct pvr_vm_context) when
 * using this API; it is designed to operate as a black box.
 *
 * Usage
 * -----
 * Begin by calling pvr_vm_create_context() to obtain a VM context. It is bound
 * to a PowerVR device (&struct pvr_device) and holds a reference to it. This
 * binding is immutable.
 *
 * Once you're finished with a VM context, call pvr_vm_destroy_context() to
 * release it. This should be done before freeing or otherwise releasing the
 * PowerVR device to which the VM context is bound.
 *
 * It is an error to destroy a VM context while it still contains valid
 * allocation ranges (and by extension, memory mappings). The operation will
 * succeed, but memory will be leaked and kernel warnings will be printed.
 *
 * Mappings
 * ~~~~~~~~
 * Physical memory is exposed to the device via **mappings**. Mappings may
 * never overlap, although any given region of physical memory may be
 * referenced by multiple mappings.
 *
 * Use pvr_vm_map() to create a mapping, providing a &struct pvr_gem_object
 * holding the physical memory to be mapped. The physical memory behind the
 * object does not have to be contiguous (it may be backed by a scatter-gather
 * table) but each contiguous "section" must be device page-aligned. This
 * restriction is checked by pvr_vm_map(), which returns -%EINVAL if the check
 * fails.
 *
 * If only part of the &struct pvr_gem_object must be mapped, use
 * pvr_vm_map_partial() instead. In addition to the parameters accepted by
 * pvr_vm_map(), this also takes an offset into the the object and the size of
 * the mapping to be created. The restrictions regarding alignment on
 * pvr_vm_map() also apply here, with the exception that only the region of the
 * object within the bounds specified by the offset and size must satisfy them.
 * These are checked by pvr_vm_map_partial(), along with the offset and size
 * values to ensure that the region they specify falls entirely within the
 * bounds of the provided object.
 *
 * Both of these mapping functions call pvr_gem_object_get() to ensure the
 * underlying physical memory is not freed until *after* the mapping is
 * released.
 *
 * Mappings are tracked internally so that it is theoretically impossible to
 * accidentally create overlapping mappings. No handle is returned after a
 * mapping operation succeeds; callers should instead use the start device
 * virtual address of the mapping as its handle.
 *
 * When mapped memory is no longer required by the device, it should be
 * unmapped using pvr_vm_unmap(). In addition to making the memory inaccessible
 * to the device, this will call pvr_gem_object_put() to release the
 * underlying physical memory. If the mapping held the last reference, the
 * physical memory will automatically be freed. Attempting to unmap an invalid
 * mapping (or one that has already been unmapped) will result in an -%ENOENT
 * error.
 */
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
 * .. c:macro:: PVR_DEVICE_PAGE_MASK
 *
 *    Mask used to round a value down to the nearest multiple of
 *    %PVR_DEVICE_PAGE_SIZE. When bitwise negated, it will indicate whether a
 *    value is already a multiple of %PVR_DEVICE_PAGE_SIZE.
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

	/** @reserved_base: Base address of reserved area. */
	u64 reserved_base;

	/**
	 * @reserved_size: Size of reserved area, in bytes. May be 0 if this
	 *                 heap has no reserved area.
	 */
	u64 reserved_size;

	/** @page_size_log2: Log2 of page size. */
	u32 page_size_log2;

	/** @nr_static_data_areas: Number of static data areas in this heap. */
	u32 nr_static_data_areas;

	/**
	 * @static_data_areas: Pointer to description of static data areas in this heap. May be
	 *                     %NULL if &nr_static_data_areas is 0.
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

int
pvr_get_heap_info(struct pvr_device *pvr_dev, struct drm_pvr_ioctl_get_heap_info_args *args);

const struct pvr_heap *
pvr_find_heap_containing(struct pvr_device *pvr_dev, u64 addr, u64 size);

struct pvr_gem_object *
pvr_vm_find_gem_object(struct pvr_vm_context *vm_ctx, u64 device_addr,
		       u64 *mapped_offset_out, u64 *mapped_size_out);

int
pvr_vm_mmu_flush(struct pvr_device *pvr_dev);

#endif /* __PVR_VM_H__ */
