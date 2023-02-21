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

int
pvr_object_cleanup(struct pvr_device *pvr_dev, u32 type, struct pvr_fw_object *fw_obj, u32 offset);

#endif /* __PVR_OBJECT_H__ */
