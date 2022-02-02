/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_DEVICE_INFO_H__
#define __PVR_DEVICE_INFO_H__

#include <linux/types.h>

struct pvr_device;

/**
 * struct pvr_device_features - Hardware feature information
 */
struct pvr_device_features {
	bool has_meta : 1;
	bool has_meta_coremem_size : 1;
	bool has_mips : 1;
	bool has_num_clusters : 1;
	bool has_phys_bus_width : 1;
	bool has_riscv_fw_processor : 1;
	bool has_slc_cache_line_size_in_bits : 1;
	bool has_sys_bus_secure_reset : 1;
	bool has_virtual_address_space_bits : 1;
	bool has_xt_top_infrastructure : 1;

	bool meta;
	u32 meta_coremem_size;
	bool mips;
	u16 num_clusters;
	u16 phys_bus_width;
	bool riscv_fw_processor;
	u16 slc_cache_line_size_in_bits;
	u16 virtual_address_space_bits;
};

/**
 * struct pvr_device_quirks - Hardware quirk information
 */
struct pvr_device_quirks {
	bool has_brn63142 : 1;
	bool has_brn63553 : 1;
};

int pvr_device_info_init(struct pvr_device *pvr_dev);

/*
 * Meta cores
 *
 * These are the values for the 'meta' feature when the feature is present
 * (as per @pvr_device_features)/
 */
#define PVR_META_MTP218 (1)
#define PVR_META_MTP219 (2)
#define PVR_META_LTP218 (3)
#define PVR_META_LTP217 (4)

#endif /* __PVR_DEVICE_INFO_H__ */
