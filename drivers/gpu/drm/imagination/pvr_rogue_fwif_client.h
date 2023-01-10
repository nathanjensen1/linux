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

	/* Only used when feature VDM_DRAWINDIRECT present. */
	u64 vdm_draw_indirect0;
	/* Only used when feature VDM_DRAWINDIRECT present. */
	u32 vdm_draw_indirect1;

	u32 ppp_ctrl;
	u32 te_psg;
	/* Only used when BRN 49927 present. */
	u32 tpu;

	u32 vdm_context_resume_task0_size;
	/* Only used when feature VDM_OBJECT_LEVEL_LLS present. */
	u32 vdm_context_resume_task3_size;

	/* Only used when BRN 56279 or BRN 67381 present. */
	u32 pds_ctrl;

	u32 view_idx;

	/* Only used when feature TESSELLATION present */
	u32 pds_coeff_free_prog;

	u32 padding;
};

/* Only used when BRN 44455 or BRN 63027 present. */
struct rogue_fwif_dummy_rgnhdr_init_geom_regs {
	u64 te_psgregion_addr;
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

	struct rogue_fwif_geom_regs regs __aligned(8);
	u32 flags __aligned(8);

	/*
	 * Holds the geometry/fragment fence value to allow the fragment partial render command
	 * to go through.
	 */
	struct rogue_fwif_ufo partial_render_geom_frag_fence;

	/* Only used when BRN 44455 or BRN 63027 present. */
	struct rogue_fwif_dummy_rgnhdr_init_geom_regs dummy_rgnhdr_init_geom_regs __aligned(8);

	/* Only used when BRN 61484 or BRN 66333 present. */
	u32 brn61484_66333_live_rt;

	u32 padding;
};

/*
 * Configuration registers which need to be loaded by the firmware before ISP
 * can be started.
 */
struct rogue_fwif_frag_regs {
	u32 usc_pixel_output_ctrl;

#define ROGUE_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL 8U
	u32 usc_clear_register[ROGUE_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL];

	u32 isp_bgobjdepth;
	u32 isp_bgobjvals;
	u32 isp_aa;
	/* Only used when feature S7_TOP_INFRASTRUCTURE present. */
	u32 isp_xtp_pipe_enable;

	u32 isp_ctl;

	/* Only used when BRN 49927 present. */
	u32 tpu;

	u32 event_pixel_pds_info;

	/* Only used when feature CLUSTER_GROUPING present. */
	u32 pixel_phantom;

	u32 view_idx;

	u32 event_pixel_pds_data;

	/* Only used when BRN 65101 present. */
	u32 brn65101_event_pixel_pds_data;

	/* Only used when feature GPU_MULTICORE_SUPPORT or BRN 47217 present. */
	u32 isp_oclqry_stride;

	/* Only used when feature ZLS_SUBTILE present. */
	u32 isp_zls_pixels;

	/* Only used when feature ISP_ZLS_D24_S8_PACKING_OGL_MODE present. */
	u32 rgx_cr_blackpearl_fix;

	/* All values below the ALIGN(8) must be 64 bit. */
	aligned_u64 isp_scissor_base;
	u64 isp_dbias_base;
	u64 isp_oclqry_base;
	u64 isp_zlsctl;
	u64 isp_zload_store_base;
	u64 isp_stencil_load_store_base;

	/*
	 * Only used when feature FBCDC_ALGORITHM present and value < 3 or feature
	 * FB_CDC_V4 present. Additionally, BRNs 48754, 60227, 72310 and 72311 must
	 * not be present.
	 */
	u64 fb_cdc_zls;

#define ROGUE_PBE_WORDS_REQUIRED_FOR_RENDERS 3U
	u64 pbe_word[8U][ROGUE_PBE_WORDS_REQUIRED_FOR_RENDERS];
	u64 tpu_border_colour_table;
	u64 pds_bgnd[3U];

	/* Only used when BRN 65101 present. */
	u64 pds_bgnd_brn65101[3U];

	u64 pds_pr_bgnd[3U];

	/* Only used when BRN 62850 or 62865 present. */
	u64 isp_dummy_stencil_store_base;

	/* Only used when BRN 66193 present. */
	u64 isp_dummy_depth_store_base;

	/* Only used when BRN 67182 present. */
	u32 rgnhdr_single_rt_size;
	/* Only used when BRN 67182 present. */
	u32 rgnhdr_scratch_offset;
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

	/* Only used if feature GPU_MULTICORE_SUPPORT present. */
	u32 execute_count;
};

/*
 * Configuration registers which need to be loaded by the firmware before CDM
 * can be started.
 */
struct rogue_fwif_compute_regs {
	u64 tpu_border_colour_table;

	/* Only used when feature CDM_USER_MODE_QUEUE present. */
	u64 cdm_cb_queue;

	/* Only used when feature CDM_USER_MODE_QUEUE present. */
	u64 cdm_cb_base;
	/* Only used when feature CDM_USER_MODE_QUEUE present. */
	u64 cdm_cb;

	/* Only used when feature CDM_USER_MODE_QUEUE is not present. */
	u64 cdm_ctrl_stream_base;

	u64 cdm_context_state_base_addr;

	/* Only used when BRN 49927 is present. */
	u32 tpu;
	u32 cdm_resume_pds1;

	/* Only used when feature COMPUTE_MORTON_CAPABLE present. */
	u32 cdm_item;

	/* Only used when feature CLUSTER_GROUPING present. */
	u32 compute_cluster;

	/* Only used when feature TPU_DM_GLOBAL_REGISTERS present. */
	u32 tpu_tag_cdm_ctrl;

	u32 padding;
};

struct rogue_fwif_cmd_compute {
	/* Common command attributes */
	struct rogue_fwif_cmd_common common __aligned(8);

	/* CDM registers */
	struct rogue_fwif_compute_regs regs;

	/* Control flags */
	u32 flags __aligned(8);

	/* Only used when feature UNIFIED_STORE_VIRTUAL_PARTITIONING present. */
	u32 num_temp_regions;

	/* Only used when feature CDM_USER_MODE_QUEUE present. */
	u32 stream_start_offset;

	/* Only used when feature GPU_MULTICORE_SUPPORT present. */
	u32 execute_count;
};

#include "pvr_rogue_fwif_client_check.h"

#endif /* __PVR_ROGUE_FWIF_CLIENT_H__ */
