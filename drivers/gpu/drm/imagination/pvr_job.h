/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_JOB_H__
#define __PVR_JOB_H__

#include <uapi/drm/pvr_drm.h>

#include <linux/types.h>

#include <drm/drm_gem.h>

/* Forward declaration from "pvr_context.h". */
struct pvr_context;

/* Forward declarations from "pvr_device.h". */
struct pvr_device;
struct pvr_file;

struct pvr_job {
	enum drm_pvr_job_type type;

	struct pvr_context *ctx;

	u32 num_bos;
	struct drm_gem_object **bos;

	struct drm_pvr_bo_ref *bo_refs;
};

int pvr_submit_job(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
		   struct drm_pvr_ioctl_submit_job_args *args);

#endif /* __PVR_JOB_H__ */
