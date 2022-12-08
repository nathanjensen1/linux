/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_JOB_H__
#define __PVR_JOB_H__

#include <uapi/drm/pvr_drm.h>

#include <linux/kref.h>
#include <linux/types.h>

#include <drm/drm_gem.h>

/* Forward declaration from "pvr_context.h". */
struct pvr_context;

/* Forward declarations from "pvr_device.h". */
struct pvr_device;
struct pvr_file;

enum pvr_job_type {
	PVR_JOB_TYPE_GEOMETRY,
	PVR_JOB_TYPE_FRAGMENT,
	PVR_JOB_TYPE_COMPUTE,
	PVR_JOB_TYPE_TRANSFER
};

struct pvr_job {
	/** @ref_count: Refcount for job. */
	struct kref ref_count;

	/** @type: Type of job. */
	enum pvr_job_type type;

	/** @id: Job ID number. */
	u32 id;

	/** @pvr_dev: Device pointer. */
	struct pvr_device *pvr_dev;

	/** @ctx: Pointer to owning context. */
	struct pvr_context *ctx;

	/** @cmd: Command data. Format depends on @type. */
	void *cmd;

	/** @cmd_len: Length of command data, in bytes. */
	u32 cmd_len;

	/**
	 * @fw_ccb_cmd_type: Firmware CCB command type. Must be one of %ROGUE_FWIF_CCB_CMD_TYPE_*.
	 */
	u32 fw_ccb_cmd_type;
};

/**
 * pvr_job_get() - Take additional reference on job.
 * @job: Job pointer.
 *
 * Call pvr_job_put() to release.
 *
 * Returns:
 *  * The requested job on success, or
 *  * %NULL if no job pointer passed.
 */
static __always_inline struct pvr_job *
pvr_job_get(struct pvr_job *job)
{
	if (job)
		kref_get(&job->ref_count);

	return job;
}

void pvr_job_put(struct pvr_job *job);

int pvr_submit_job(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
		   struct drm_pvr_ioctl_submit_job_args *args);

#endif /* __PVR_JOB_H__ */
