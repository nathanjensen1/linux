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
 * struct pvr_object - Common object structure
 */
struct pvr_object {
	/** @type: Type of object. Must be one of &enum drm_pvr_object_type. */
	enum drm_pvr_object_type type;

	/** @ref_count: Reference count of object. */
	struct kref ref_count;
};

int pvr_object_create(struct pvr_file *pvr_file,
		      struct drm_pvr_ioctl_create_object_args *args,
		      u32 *handle_out);

/**
 * pvr_object_get() - Get object pointer from handle
 * @pvr_file: Pointer to pvr_file structure.
 * @handle: Object handle.
 *
 * Takes reference on object. Call pvr_object_put() to release.
 *
 * Returns:
 *  * The requested object on success, or
 *  * %NULL on failure (object is not in object list)
 */
static __always_inline struct pvr_object *
pvr_object_get(struct pvr_file *pvr_file, u32 handle)
{
	struct pvr_object *obj = xa_load(&pvr_file->objects, handle);

	if (obj)
		kref_get(&obj->ref_count);

	return obj;
}

void pvr_object_put(struct pvr_object *obj);

int pvr_object_destroy(struct pvr_file *pvr_file, u32 handle);

int
pvr_object_cleanup(struct pvr_device *pvr_dev, u32 type, struct pvr_fw_object *fw_obj, u32 offset);

#endif /* __PVR_OBJECT_H__ */
