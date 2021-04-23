/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_ROGUE_FWIF_CLIENT_H__
#define __PVR_ROGUE_FWIF_CLIENT_H__

#include <linux/kernel.h>
#include <linux/types.h>

#include "pvr_rogue_fwif_shared.h"

/*
 ************************************************
 * Parameter/HWRTData control structures.
 ************************************************
 */

/*
 * Configuration registers which need to be loaded by the firmware before a geometry
 * job can be started.
 */
struct rogue_fwif_geom_regs {
	u64 vdm_ctrl_stream_base;
	u64 tpu_border_colour_table;

	u32 ppp_ctrl;
	u32 te_psg;
	u32 tpu;

	u32 vdm_context_resume_task0_size;

	/* FIXME: HIGH: FIX_HW_BRN_56279 changes the structure's layout, given we
	 * are supporting Features/ERNs/BRNs at runtime, we need to look into this
	 * and find a solution to keep layout intact.
	 */
	/* Available if FIX_HW_BRN_56279 is present. */
	u32 pds_ctrl;

	u32 view_idx;
};

/*
 * Represents a geometry command that can be used to tile a whole scene's objects as
 * per TA behavior.
 */
struct rogue_fwif_cmd_geom {
	/*
	 * rogue_fwif_cmd_geom_frag_shared field must always be at the beginning of the
	 * struct.
	 *
	 * The command struct (rogue_fwif_cmd_geom) is shared between Client and
	 * Firmware. Kernel is unable to perform read/write operations on the
	 * command struct, the SHARED region is the only exception from this rule.
	 * This region must be the first member so that Kernel can easily access it.
	 * For more info, see rogue_fwif_cmd_geom_frag_shared definition.
	 */
	struct rogue_fwif_cmd_geom_frag_shared cmd_shared;

	struct rogue_fwif_geom_regs geom_regs __aligned(8);
	u32 flags __aligned(8);

	/*
	 * Holds the geometry/fragment fence value to allow the fragment partial render command
	 * to go through.
	 */
	struct rogue_fwif_ufo partial_render_geom_frag_fence;
};

/*
 * Configuration registers which need to be loaded by the firmware before ISP
 * can be started.
 */
struct rogue_fwif_frag_regs {
	u32 usc_pixel_output_ctrl;

	/* FIXME: HIGH: ROGUE_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL changes the structure's layout. */
#define ROGUE_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL 8U
	u32 usc_clear_register[ROGUE_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL];

	u32 isp_bgobjdepth;
	u32 isp_bgobjvals;
	u32 isp_aa;
	u32 isp_ctl;

	u32 tpu;

	u32 event_pixel_pds_info;

	/* FIXME: HIGH: RGX_FEATURE_CLUSTER_GROUPING changes the structure's layout. */
	u32 pixel_phantom;

	u32 view_idx;

	u32 event_pixel_pds_data;
	/*
	 * FIXME: HIGH: MULTIBUFFER_OCLQRY changes the structure's layout.
	 * Commenting out for now as it's not supported by 4.V.2.51.
	 */
	/* uint32_t isp_oclqry_stride; */

	/* All values below the ALIGN(8) must be 64 bit. */
	aligned_u64 isp_scissor_base;
	u64 isp_dbias_base;
	u64 isp_oclqry_base;
	u64 isp_zlsctl;
	u64 isp_zload_store_base;
	u64 isp_stencil_load_store_base;
	/* FIXME: HIGH: RGX_FEATURE_ZLS_SUBTILE changes the structure's layout. */
	u64 isp_zls_pixels;

	/* FIXME: HIGH: RGX_HW_REQUIRES_FB_CDC_ZLS_SETUP changes the structure's layout. */
	u64 deprecated;

	/* FIXME: HIGH: RGX_PBE_WORDS_REQUIRED_FOR_RENDERS changes the structure's layout. */
#define ROGUE_PBE_WORDS_REQUIRED_FOR_RENDERS 2U
	u64 pbe_word[8U][ROGUE_PBE_WORDS_REQUIRED_FOR_RENDERS];
	u64 tpu_border_colour_table;
	u64 pds_bgnd[3U];
	u64 pds_pr_bgnd[3U];
};

struct rogue_fwif_cmd_frag {
	struct rogue_fwif_cmd_geom_frag_shared cmd_shared __aligned(8);

	struct rogue_fwif_frag_regs regs __aligned(8);
	/* command control flags. */
	u32 flags;
	/* Stride IN BYTES for Z-Buffer in case of RTAs. */
	u32 zls_stride;
	/* Stride IN BYTES for S-Buffer in case of RTAs. */
	u32 sls_stride;
};

/*
 * Configuration registers which need to be loaded by the firmware before CDM
 * can be started.
 */
struct rogue_fwif_compute_regs {
	u64 tpu_border_colour_table;
	u64 cdm_item;
	u64 compute_cluster;
	u64 cdm_ctrl_stream_base;
	u32 tpu;
	u32 cdm_resume_pds1;
};

struct rogue_fwif_cmd_compute {
	/* Common command attributes */
	struct rogue_fwif_cmd_common common __aligned(8);

	/* CDM registers */
	struct rogue_fwif_compute_regs cmd_regs;

	/* Control flags */
	u32 flags __aligned(8);
};

#endif /* __PVR_ROGUE_FWIF_CLIENT_H__ */
