// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_device.h"
#include "pvr_fw.h"
#include "pvr_fw_mips.h"
#include "pvr_gem.h"
#include "pvr_rogue_mips.h"
#include "pvr_vm_mips.h"

#include <linux/elf.h>
#include <linux/err.h>
#include <linux/types.h>

#define ROGUE_FW_HEAP_MIPS_BASE 0xC0000000
#define ROGUE_FW_HEAP_MIPS_SHIFT 24 /* 16 MB */
#define ROGUE_FW_HEAP_MIPS_RESERVED_SIZE SZ_1M

/**
 * process_elf_command_stream() - Process ELF firmware image and populate
 *                                firmware sections
 * @pvr_dev: Device pointer.
 * @fw: Pointer to firmware image.
 * @layout_entries: Pointer to layout table.
 * @num_layout_entries: Number of entries in layout table.
 * @fw_code_ptr: Pointer to FW code section.
 * @fw_data_ptr: Pointer to FW data section.
 * @fw_core_code_ptr: Pointer to FW coremem code section.
 * @fw_core_data_ptr: Pointer to FW coremem data section.
 *
 * Returns :
 *  * 0 on success, or
 *  * -EINVAL on any error in ELF command stream.
 */
static int
process_elf_command_stream(struct pvr_device *pvr_dev, const u8 *fw,
			   const struct pvr_fw_layout_entry *layout_entries,
			   u32 num_layout_entries, u8 *fw_code_ptr,
			   u8 *fw_data_ptr, u8 *fw_core_code_ptr,
			   u8 *fw_core_data_ptr)
{
	struct elf32_hdr *header = (struct elf32_hdr *)fw;
	struct elf32_phdr *program_header = (struct elf32_phdr *)(fw + header->e_phoff);
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	u32 entry;
	int err;

	for (entry = 0; entry < header->e_phnum; entry++, program_header++) {
		void *write_addr;

		/* Only consider loadable entries in the ELF segment table */
		if (program_header->p_type != PT_LOAD)
			continue;

		err = pvr_fw_find_mmu_segment(program_header->p_vaddr, program_header->p_memsz,
				       layout_entries, num_layout_entries, fw_code_ptr, fw_data_ptr,
				       fw_core_code_ptr, fw_core_data_ptr, &write_addr);
		if (err) {
			drm_err(drm_dev,
				"Addr 0x%x (size: %d) not found in any firmware segment",
				program_header->p_vaddr, program_header->p_memsz);
			goto err_out;
		}

		/* Write to FW allocation only if available */
		if (write_addr) {
			memcpy(write_addr, fw + program_header->p_offset,
			       program_header->p_filesz);

			memset((u8 *)write_addr + program_header->p_filesz, 0,
			       program_header->p_memsz - program_header->p_filesz);
		}
	}

	return 0;

err_out:
	return err;
}

static int
pvr_mips_init(struct pvr_device *pvr_dev)
{
	pvr_fw_heap_info_init(pvr_dev, ROGUE_FW_HEAP_MIPS_SHIFT, ROGUE_FW_HEAP_MIPS_RESERVED_SIZE);

	return pvr_vm_mips_init(pvr_dev);
}

static void
pvr_mips_fini(struct pvr_device *pvr_dev)
{
	pvr_vm_mips_fini(pvr_dev);
}

static int
pvr_mips_fw_process(struct pvr_device *pvr_dev, const u8 *fw,
		    const struct pvr_fw_layout_entry *layout_entries, u32 num_layout_entries,
		    u8 *fw_code_ptr, u8 *fw_data_ptr, u8 *fw_core_code_ptr, u8 *fw_core_data_ptr,
		    u32 core_code_alloc_size)
{
	struct pvr_fw_mips_data *mips_data = pvr_dev->fw_data.mips_data;
	const struct pvr_fw_layout_entry *boot_data_entry;
	const struct pvr_fw_layout_entry *stack_entry;
	struct rogue_mipsfw_boot_data *boot_data;
	dma_addr_t dma_addr;
	u32 page_nr;
	int err;

	err = process_elf_command_stream(pvr_dev, fw, layout_entries, num_layout_entries,
					 fw_code_ptr, fw_data_ptr, fw_core_code_ptr,
					 fw_core_data_ptr);
	if (err)
		goto err_out;

	boot_data_entry = pvr_fw_find_layout_entry(layout_entries, num_layout_entries,
						   MIPS_BOOT_DATA);
	if (!boot_data_entry) {
		err = -EINVAL;
		goto err_out;
	}

	stack_entry = pvr_fw_find_layout_entry(layout_entries, num_layout_entries, MIPS_STACK);
	if (!stack_entry) {
		err = -EINVAL;
		goto err_out;
	}

	boot_data = (struct rogue_mipsfw_boot_data *)(fw_data_ptr + boot_data_entry->alloc_offset +
						      ROGUE_MIPSFW_BOOTLDR_CONF_OFFSET);

	WARN_ON(pvr_fw_get_dma_addr(pvr_dev->fw_data_obj, stack_entry->alloc_offset, &dma_addr));
	boot_data->stack_phys_addr = dma_addr;

	boot_data->reg_base = pvr_dev->regs_resource->start;

	for (page_nr = 0; page_nr < ARRAY_SIZE(boot_data->pt_phys_addr); page_nr++) {
		WARN_ON(pvr_gem_get_dma_addr(mips_data->pt_obj,
					     page_nr << ROGUE_MIPSFW_LOG2_PAGE_SIZE_4K, &dma_addr));

		boot_data->pt_phys_addr[page_nr] = dma_addr;
	}

	boot_data->pt_log2_page_size = ROGUE_MIPSFW_LOG2_PAGE_SIZE_4K;
	boot_data->pt_num_pages = ROGUE_MIPSFW_MAX_NUM_PAGETABLE_PAGES;
	boot_data->reserved1 = 0;
	boot_data->reserved2 = 0;

	return 0;

err_out:
	return err;
}

static int
pvr_mips_start(struct pvr_device *pvr_dev)
{
	return -ENODEV;
}

static int
pvr_mips_stop(struct pvr_device *pvr_dev)
{
	return -ENODEV;
}

static u32
pvr_mips_get_fw_addr_with_offset(struct pvr_fw_object *fw_obj, u32 offset)
{
	struct pvr_device *pvr_dev = fw_obj->base.pvr_dev;

	/* MIPS cacheability is determined by page table. */
	return ((fw_obj->fw_addr_offset + offset) & pvr_dev->fw_heap_info.offset_mask) |
	       ROGUE_FW_HEAP_MIPS_BASE;
}

const struct pvr_fw_funcs pvr_fw_funcs_mips = {
	.init = pvr_mips_init,
	.fini = pvr_mips_fini,
	.fw_process = pvr_mips_fw_process,
	.vm_map = pvr_vm_mips_map,
	.vm_unmap = pvr_vm_mips_unmap,
	.get_fw_addr_with_offset = pvr_mips_get_fw_addr_with_offset,
	.start = pvr_mips_start,
	.stop = pvr_mips_stop,
};
