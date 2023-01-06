/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_OBJECT_H__
#define __PVR_OBJECT_H__

#include <linux/compiler_attributes.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/xarray.h>
#include <uapi/drm/pvr_drm.h>

#include "pvr_device.h"

/**
 * enum pvr_object_type - Valid object types
 */
enum pvr_object_type {
	/** @PVR_OBJECT_TYPE_FREE_LIST: Free list object. */
	PVR_OBJECT_TYPE_FREE_LIST = 0,
	/** @PVR_OBJECT_TYPE_HWRT_DATASET: HWRT data set. */
	PVR_OBJECT_TYPE_HWRT_DATASET,
};

/**
 * struct pvr_object - Common object structure
 */
struct pvr_object {
	/** @type: Type of object. */
	enum pvr_object_type type;

	/** @ref_count: Reference count of object. */
	struct kref ref_count;

	/** @pvr_dev: Pointer to device that owns this object. */
	struct pvr_device *pvr_dev;

	/** @fw_id: Firmware ID for this object. */
	u32 fw_id;
};

/**
 * pvr_object_lookup() - Lookup object pointer from handle and file
 * @pvr_file: Pointer to pvr_file structure.
 * @handle: Object handle.
 *
 * Takes reference on object. Call pvr_object_put() to release.
 *
 * Returns:
 *  * The requested object on success, or
 *  * %NULL on failure (object is not in object list, or does not belong to @pvr_file)
 */
static __always_inline struct pvr_object *
pvr_object_lookup(struct pvr_file *pvr_file, u32 handle)
{
	struct pvr_object *obj = xa_load(&pvr_file->obj_handles, handle);

	if (obj) {
		kref_get(&obj->ref_count);

		return obj;
	}

	return NULL;
}

/**
 * pvr_object_lookup_id() - Lookup object pointer from firmware ID
 * @pvr_dev: Device pointer.
 * @id: FW object ID.
 *
 * Takes reference on object. Call pvr_object_put() to release.
 *
 * Returns:
 *  * The requested object on success, or
 *  * %NULL if object is not in object list
 */
static __always_inline struct pvr_object *
pvr_object_lookup_id(struct pvr_device *pvr_dev, u32 id)
{
	struct pvr_object *obj = xa_load(&pvr_dev->obj_ids, id);

	if (obj) {
		kref_get(&obj->ref_count);

		return obj;
	}

	return NULL;
}

void pvr_object_put(struct pvr_object *obj);

int pvr_object_common_init(struct pvr_file *pvr_file, struct pvr_object *obj);

void pvr_object_common_fini(struct pvr_object *obj);

int pvr_object_destroy(struct pvr_file *pvr_file, u32 handle, enum pvr_object_type type);

int
pvr_object_cleanup(struct pvr_device *pvr_dev, u32 type, struct pvr_fw_object *fw_obj, u32 offset);

void pvr_destroy_objects_for_file(struct pvr_file *pvr_file);

#endif /* __PVR_OBJECT_H__ */
