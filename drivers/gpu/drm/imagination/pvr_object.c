// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_device.h"
#include "pvr_free_list.h"
#include "pvr_gem.h"
#include "pvr_hwrt.h"
#include "pvr_object.h"

#include <linux/xarray.h>
#include <uapi/drm/pvr_drm.h>

/**
 * pvr_object_common_init() - Initialise common object structure
 * @pvr_dev: Device pointer.
 * @obj: Pointer to target object.
 * @fw_id: Firmware ID of object.
 */
void
pvr_object_common_init(struct pvr_device *pvr_dev, struct pvr_object *obj, u32 fw_id)
{
	kref_init(&obj->ref_count);
	obj->pvr_dev = pvr_dev;
	obj->fw_id = fw_id;
}

static void
destroy_object(struct pvr_object *obj)
{
	switch (obj->type) {
	case DRM_PVR_OBJECT_TYPE_FREE_LIST: {
		struct pvr_free_list *free_list = to_pvr_free_list(obj);

		pvr_free_list_destroy(free_list);
		break;
	}
	case DRM_PVR_OBJECT_TYPE_HWRT_DATASET: {
		struct pvr_hwrt_dataset *hwrt = to_pvr_hwrt_dataset(obj);

		pvr_hwrt_dataset_destroy(hwrt);
		break;
	}
	default:
		WARN_ON(1);
		break;
	}
}

/**
 * pvr_object_create() - Create an object from parameters from userspace
 * @pvr_file: Pointer to pvr_file structure.
 * @args: Creation arguments from userspace.
 * @handle_out: Output handle pointer.
 *
 * The context is initialised with refcount of 1.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EINVAL on invalid arguments, or
 *  * -%ENOMEM on out-of-memory, or
 *  * -%EFAULT if arguments can't be copied from userspace, or
 *  * Any error returned by pvr_free_list_create().
 */
int
pvr_object_create(struct pvr_file *pvr_file,
		  struct drm_pvr_ioctl_create_object_args *args,
		  u32 *handle_out)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct pvr_object *obj;
	u32 handle;
	void *old;
	int err;
	u32 id;

	/*
	 * Allocate global object ID for firmware. We will update this with the object once it is
	 * created.
	 */
	err = xa_alloc(&pvr_dev->obj_ids, &id, NULL, xa_limit_32b, GFP_KERNEL);
	if (err < 0)
		goto err_out;

	/*
	 * Allocate object handle for userspace. We will update this with the object once it
	 * is created.
	 */
	err = xa_alloc(&pvr_file->obj_handles, &handle, NULL, xa_limit_32b, GFP_KERNEL);
	if (err < 0)
		goto err_id_xa_free;

	switch (args->type) {
	case DRM_PVR_OBJECT_TYPE_FREE_LIST: {
		struct drm_pvr_ioctl_create_free_list_args free_list_args;
		struct pvr_free_list *free_list;

		if (copy_from_user(&free_list_args, u64_to_user_ptr(args->data),
				   sizeof(free_list_args))) {
			err = -EFAULT;
			goto err_handle_xa_free;
		}

		free_list = pvr_free_list_create(pvr_file, &free_list_args, id);
		if (IS_ERR(free_list)) {
			err = PTR_ERR(free_list);
			goto err_handle_xa_free;
		}
		obj = from_pvr_free_list(free_list);
		break;
	}

	case DRM_PVR_OBJECT_TYPE_HWRT_DATASET: {
		struct drm_pvr_ioctl_create_hwrt_dataset_args hwrt_args;
		struct pvr_hwrt_dataset *hwrt;

		if (copy_from_user(&hwrt_args, u64_to_user_ptr(args->data),
				   sizeof(hwrt_args))) {
			err = -EFAULT;
			goto err_handle_xa_free;
		}

		hwrt = pvr_hwrt_dataset_create(pvr_file, &hwrt_args, id);
		if (IS_ERR(hwrt)) {
			err = PTR_ERR(hwrt);
			goto err_handle_xa_free;
		}
		obj = from_pvr_hwrt_dataset(hwrt);
		break;
	}

	default:
		err = -EINVAL;
		goto err_handle_xa_free;
	}
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_handle_xa_free;
	}

	old = xa_store(&pvr_dev->obj_ids, id, obj, GFP_KERNEL);
	if (xa_is_err(old)) {
		err = xa_err(old);
		goto err_destroy_object;
	}

	old = xa_store(&pvr_file->obj_handles, handle, obj, GFP_KERNEL);
	if (xa_is_err(old)) {
		err = xa_err(old);
		goto err_destroy_object;
	}

	*handle_out = handle;
	return 0;

err_destroy_object:
	destroy_object(obj);

err_handle_xa_free:
	xa_erase(&pvr_file->obj_handles, handle);

err_id_xa_free:
	xa_erase(&pvr_dev->obj_ids, id);

err_out:
	return err;
}

static void
pvr_object_release(struct kref *ref_count)
{
	struct pvr_object *obj =
		container_of(ref_count, struct pvr_object, ref_count);
	struct pvr_device *pvr_dev = obj->pvr_dev;

	xa_erase(&pvr_dev->obj_ids, obj->fw_id);

	destroy_object(obj);
}

/**
 * pvr_object_put() - Release reference on object
 * @obj: Target object.
 */
void
pvr_object_put(struct pvr_object *obj)
{
	kref_put(&obj->ref_count, pvr_object_release);
}

/**
 * pvr_object_destroy() - Destroy object
 * @pvr_file: Pointer to pvr_file structure.
 * @handle: Object handle.
 *
 * Removes object from list and drops initial reference. Object will then be
 * destroyed once all outstanding references are dropped.
 *
 * Returns:
 *  * 0 on success, or
 *  * -%EINVAL if object not in object list.
 */
int
pvr_object_destroy(struct pvr_file *pvr_file, u32 handle)
{
	struct pvr_object *obj = xa_erase(&pvr_file->obj_handles, handle);

	if (!obj)
		return -EINVAL;

	pvr_object_put(obj);

	return 0;
}

/**
 * pvr_destroy_objects_for_file: Destroy any objects associated with the given file
 * @pvr_file: Pointer to pvr_file structure.
 *
 * Removes all objects associated with @pvr_file from the device object list and drops initial
 * references. Objects will then be destroyed once all outstanding references are dropped.
 */
void pvr_destroy_objects_for_file(struct pvr_file *pvr_file)
{
	struct pvr_object *obj;
	unsigned long handle;

	xa_for_each(&pvr_file->obj_handles, handle, obj) {
		xa_erase(&pvr_file->obj_handles, handle);
		pvr_object_put(obj);
	}
}

/**
 * pvr_object_cleanup() - Send FW cleanup request for an object
 * @pvr_dev: Target PowerVR device.
 * @type: Type of object to cleanup. Must be one of &enum rogue_fwif_cleanup_type.
 * @fw_obj: Pointer to FW object containing object to cleanup.
 * @offset: Offset within FW object of object to cleanup.
 *
 * Returns:
 *  * 0 on success,
 *  * -EBUSY if object is busy, or
 *  * -ETIMEDOUT on timeout.
 */
int
pvr_object_cleanup(struct pvr_device *pvr_dev, u32 type, struct pvr_fw_object *fw_obj, u32 offset)
{
	struct rogue_fwif_kccb_cmd cmd;
	int slot_nr;
	int err;
	u32 rtn;

	struct rogue_fwif_cleanup_request *cleanup_req = &cmd.cmd_data.cleanup_data;

	cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_CLEANUP;
	cmd.kccb_flags = 0;
	cleanup_req->cleanup_type = type;

	switch (type) {
	case ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT:
		pvr_gem_get_fw_addr_offset(fw_obj, offset,
					   &cleanup_req->cleanup_data.context_fw_addr);
		break;
	case ROGUE_FWIF_CLEANUP_HWRTDATA:
		pvr_gem_get_fw_addr_offset(fw_obj, offset,
					   &cleanup_req->cleanup_data.hwrt_data_fw_addr);
		break;
	case ROGUE_FWIF_CLEANUP_FREELIST:
		pvr_gem_get_fw_addr_offset(fw_obj, offset,
					   &cleanup_req->cleanup_data.freelist_fw_addr);
		break;
	default:
		err = -EINVAL;
		goto err_out;
	}

	err = pvr_kccb_send_cmd(pvr_dev, &cmd, &slot_nr);
	if (err)
		goto err_out;

	err = pvr_kccb_wait_for_completion(pvr_dev, slot_nr, HZ, &rtn);
	if (err)
		goto err_out;

	if (rtn & ROGUE_FWIF_KCCB_RTN_SLOT_CLEANUP_BUSY)
		err = -EBUSY;

err_out:
	return err;
}
