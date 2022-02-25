/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#ifndef __PVR_FW_H__
#define __PVR_FW_H__

#include "pvr_fw_info.h"

#include <linux/types.h>

/* Forward declaration from pvr_device.h. */
struct pvr_device;

#define ROGUE_FWIF_FWCCB_NUMCMDS_LOG2 5

#define ROGUE_FWIF_KCCB_NUMCMDS_LOG2_DEFAULT 7

/**
 * struct pvr_fw_funcs - FW processor function table
 */
struct pvr_fw_funcs {
	/**
	 * @init:
	 *
	 * FW processor specific initialisation.
	 * @pvr_dev: Target PowerVR device.
	 *
	 * This function must call pvr_fw_heap_calculate() to initialise the firmware heap for this
	 * FW processor.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * 0 on success, or
	 *  * Any appropriate error on failure.
	 */
	int (*init)(struct pvr_device *pvr_dev);

	/**
	 * @fini:
	 *
	 * FW processor specific finalisation.
	 * @pvr_dev: Target PowerVR device.
	 *
	 * This function is optional.
	 */
	void (*fini)(struct pvr_device *pvr_dev);

	/**
	 * @fw_process:
	 *
	 * Load and process firmware image.
	 * @pvr_dev: Target PowerVR device.
	 * @fw: Pointer to firmware image.
	 * @layout_entries: Layout of firmware memory.
	 * @num_layout_entries: Number of entries in @layout_entries.
	 * @fw_code_ptr: Pointer to firmware code section.
	 * @fw_data_ptr: Pointer to firmware data section.
	 * @fw_core_code_ptr: Pointer to firmware core code section. May be %NULL.
	 * @fw_core_data_ptr: Pointer to firmware core data section. May be %NULL.
	 * @core_code_alloc_size: Total allocation size of core code section.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * 0 on success, or
	 *  * Any appropriate error on failure.
	 */
	int (*fw_process)(struct pvr_device *pvr_dev, const u8 *fw,
			  const struct pvr_fw_layout_entry *layout_entries, u32 num_layout_entries,
			  u8 *fw_code_ptr, u8 *fw_data_ptr, u8 *fw_core_code_ptr,
			  u8 *fw_core_data_ptr, u32 core_code_alloc_size);

	/**
	 * @vm_map:
	 *
	 * Map FW object into FW processor address space.
	 * @pvr_dev: Target PowerVR device.
	 * @fw_obj: FW object to map.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * 0 on success, or
	 *  * Any appropriate error on failure.
	 */
	int (*vm_map)(struct pvr_device *pvr_dev, struct pvr_fw_object *fw_obj);

	/**
	 * @vm_unmap:
	 *
	 * Unmap FW object from FW processor address space.
	 * @pvr_dev: Target PowerVR device.
	 * @fw_obj: FW object to map.
	 *
	 * This function is mandatory.
	 */
	void (*vm_unmap)(struct pvr_device *pvr_dev, struct pvr_fw_object *fw_obj);

	/**
	 * @get_fw_addr_with_offset:
	 *
	 * Called to get address of object in firmware address space, with offset.
	 * @fw_obj: Pointer to object.
	 * @offset: Desired offset from start of object.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * Address in firmware address space.
	 */
	u32 (*get_fw_addr_with_offset)(struct pvr_fw_object *fw_obj, u32 offset);

	/**
	 * @start:
	 *
	 * Called to start FW processor and boot firmware.
	 * @pvr_dev: Target PowerVR device.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * 0 on success.
	 *  * Any appropriate error on failure.
	 */
	int (*start)(struct pvr_device *pvr_dev);

	/**
	 * @stop:
	 *
	 * Called to stop FW processor execution.
	 * @pvr_dev: Target PowerVR device.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * 0 on success.
	 *  * Any appropriate error on failure.
	 */
	int (*stop)(struct pvr_device *pvr_dev);
};

extern const struct pvr_fw_funcs pvr_fw_funcs_meta;
extern const struct pvr_fw_funcs pvr_fw_funcs_mips;

int pvr_fw_init(struct pvr_device *pvr_dev);
void pvr_fw_fini(struct pvr_device *pvr_dev);

int pvr_wait_for_fw_boot(struct pvr_device *pvr_dev);

void pvr_fw_mts_schedule(struct pvr_device *pvr_dev, u32 val);

void
pvr_fw_heap_info_init(struct pvr_device *pvr_dev, u32 log2_size, u32 reserved_size);

const struct pvr_fw_layout_entry *
pvr_fw_find_layout_entry(const struct pvr_fw_layout_entry *layout_entries, u32 num_layout_entries,
			 enum pvr_fw_section_id id);
int
pvr_fw_find_mmu_segment(u32 addr, u32 size, const struct pvr_fw_layout_entry *layout_entries,
			u32 num_layout_entries, void *fw_code_ptr, void *fw_data_ptr,
			void *fw_core_code_ptr, void *fw_core_data_ptr,
			void **host_addr_out);

#endif /* __PVR_FW_H__ */
