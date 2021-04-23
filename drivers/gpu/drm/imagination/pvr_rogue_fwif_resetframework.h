/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_ROGUE_FWIF_RESETFRAMEWORK_H__
#define __PVR_ROGUE_FWIF_RESETFRAMEWORK_H__

#include <linux/bits.h>
#include <linux/types.h>

#include "pvr_rogue_fwif_shared.h"

struct rogue_fwif_rf_registers {
	u64 cdmreg_cdm_ctrl_stream_base;
};

/* enables the reset framework in the firmware */
#define ROGUE_FWIF_RF_FLAG_ENABLE BIT(0)

struct rogue_fwif_rf_cmd {
	u32 flags;

	/* THIS MUST BE THE LAST MEMBER OF THE CONTAINING STRUCTURE */
	struct rogue_fwif_rf_registers fw_registers __aligned(8);
};

#define ROGUE_FWIF_RF_CMD_SIZE sizeof(struct rogue_fwif_rf_cmd)

#endif /* __PVR_ROGUE_FWIF_RESETFRAMEWORK_H__ */
