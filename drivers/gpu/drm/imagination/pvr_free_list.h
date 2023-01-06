/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_FREE_LIST_H__
#define __PVR_FREE_LIST_H__

#include <linux/compiler_attributes.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/xarray.h>
#include <uapi/drm/pvr_drm.h>

#include "pvr_device.h"
#include "pvr_object.h"

/* Forward declaration from pvr_gem.h. */
struct pvr_fw_object;

/* Forward declaration from pvr_gem.h. */
struct pvr_gem_object;

/* Forward declaration from pvr_hwrt.h. */
struct pvr_hwrt_data;

/**
 * struct pvr_free_list_node - structure representing an allocation in the free
 *                             list
 */
struct pvr_free_list_node {
	/** @node: List node for &pvr_free_list.mem_block_list. */
	struct list_head node;

	/** @free_list: Pointer to owning free list. */
	struct pvr_free_list *free_list;

	/** @num_pages: Number of pages in this node. */
	u32 num_pages;

	/** @mem_obj: GEM object representing the pages in this node. */
	struct pvr_gem_object *mem_obj;
};

/**
 * struct pvr_free_list - structure representing a free list
 */
struct pvr_free_list {
	/** @base: Object base structure. */
	struct pvr_object base;

	/** @pvr_dev: Pointer to owning device. */
	struct pvr_device *pvr_dev;

	/** @obj: GEM object representing the free list. */
	struct pvr_gem_object *obj;

	/** @fw_obj: FW object representing the FW-side structure. */
	struct pvr_fw_object *fw_obj;

	/** &fw_data: Pointer to CPU mapping of the FW-side structure. */
	struct rogue_fwif_freelist *fw_data;

	/**
	 * @lock: Mutex protecting modification of the free list. Must be held when accessing any
	 *        of the members below.
	 */
	struct mutex lock;

	/** @current_pages: Current number of pages in free list. */
	u32 current_pages;

	/** @max_pages: Maximum number of pages in free list. */
	u32 max_pages;

	/** @grow_pages: Pages to grow free list by per request. */
	u32 grow_pages;

	/**
	 * @grow_threshold: Percentage of FL memory used that should trigger a
	 *                  new grow request.
	 */
	u32 grow_threshold;

	/**
	 * @ready_pages: Number of pages reserved for FW to use while a grow
	 *               request is being processed.
	 */
	u32 ready_pages;

	/** @mem_block_list: List of memory blocks in this free list. */
	struct list_head mem_block_list;

	/** @hwrt_list: List of HWRTs using this free list. */
	struct list_head hwrt_list;
};

static __always_inline struct pvr_object *
from_pvr_free_list(struct pvr_free_list *free_list)
{
	return &free_list->base;
};

static __always_inline struct pvr_free_list *
to_pvr_free_list(struct pvr_object *obj)
{
	return container_of(obj, struct pvr_free_list, base);
}

struct pvr_free_list *
pvr_free_list_create(struct pvr_file *pvr_file,
		     struct drm_pvr_ioctl_create_free_list_args *args);

void pvr_free_list_destroy(struct pvr_free_list *free_list);

u32
pvr_get_free_list_min_pages(struct pvr_device *pvr_dev);

/**
 * pvr_free_list_lookup() - Lookup free list pointer from handle and file
 * @pvr_file: Pointer to pvr_file structure.
 * @handle: Object handle.
 *
 * Takes reference on free list object. Call pvr_free_list_put() to release.
 *
 * Returns:
 *  * The requested object on success, or
 *  * %NULL on failure (object does not exist in list, is not a free list, or
 *    does not belong to @pvr_file)
 */
static __always_inline struct pvr_free_list *
pvr_free_list_lookup(struct pvr_file *pvr_file, u32 handle)
{
	struct pvr_object *obj = pvr_object_lookup(pvr_file, handle);

	if (obj) {
		if (obj->type == PVR_OBJECT_TYPE_FREE_LIST)
			return to_pvr_free_list(obj);

		pvr_object_put(obj);
	}

	return NULL;
}

/**
 * pvr_free_list_lookup_id() - Lookup free list pointer from FW ID
 * @pvr_device: Device pointer.
 * @id: FW object ID.
 *
 * Takes reference on free list object. Call pvr_free_list_put() to release.
 *
 * Returns:
 *  * The requested object on success, or
 *  * %NULL on failure (object does not exist in list, or is not a free list)
 */
static __always_inline struct pvr_free_list *
pvr_free_list_lookup_id(struct pvr_device *pvr_dev, u32 id)
{
	struct pvr_object *obj = pvr_object_lookup_id(pvr_dev, id);

	if (obj) {
		if (obj->type == PVR_OBJECT_TYPE_FREE_LIST)
			return to_pvr_free_list(obj);

		pvr_object_put(obj);
	}

	return NULL;
}

/**
 * pvr_free_list_put() - Release reference on free list
 * @free_list: Pointer to list to release reference on
 */
static __always_inline void
pvr_free_list_put(struct pvr_free_list *free_list)
{
	pvr_object_put(&free_list->base);
}

void
pvr_free_list_add_hwrt(struct pvr_free_list *free_list, struct pvr_hwrt_data *hwrt_data);
void
pvr_free_list_remove_hwrt(struct pvr_free_list *free_list, struct pvr_hwrt_data *hwrt_data);

void
pvr_free_list_reconstruct(struct pvr_device *pvr_dev, u32 freelist_id);

#endif /* __PVR_FREE_LIST_H__ */
