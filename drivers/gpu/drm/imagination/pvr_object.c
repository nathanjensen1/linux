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
