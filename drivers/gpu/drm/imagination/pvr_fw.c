// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_ccb.h"
#include "pvr_device.h"
#include "pvr_fw.h"
#include "pvr_fw_info.h"
#include "pvr_fw_trace.h"
#include "pvr_gem.h"
#include "pvr_rogue_heap_config.h"

#include <drm/drm_mm.h>
#include <linux/firmware.h>
#include <linux/minmax.h>
#include <linux/sizes.h>

#define FW_BOOT_TIMEOUT_USEC 5000000

/* Config heap occupies top 192k of the firmware heap. */
#define PVR_ROGUE_FW_CONFIG_HEAP_GRANULARITY SZ_64K
#define PVR_ROGUE_FW_CONFIG_HEAP_SIZE (3 * PVR_ROGUE_FW_CONFIG_HEAP_GRANULARITY)

/* Main firmware allocations should come from the remainder of the heap. */
#define PVR_ROGUE_FW_MAIN_HEAP_BASE ROGUE_FW_HEAP_BASE

/* Offsets from start of configuration area of FW heap. */
#define PVR_ROGUE_FWIF_CONNECTION_CTL_OFFSET 0
#define PVR_ROGUE_FWIF_OSINIT_OFFSET \
	(PVR_ROGUE_FWIF_CONNECTION_CTL_OFFSET + PVR_ROGUE_FW_CONFIG_HEAP_GRANULARITY)
#define PVR_ROGUE_FWIF_SYSINIT_OFFSET \
	(PVR_ROGUE_FWIF_OSINIT_OFFSET + PVR_ROGUE_FW_CONFIG_HEAP_GRANULARITY)

#define PVR_ROGUE_FAULT_PAGE_SIZE SZ_4K

#define PVR_SYNC_OBJ_SIZE sizeof(u32)

const struct pvr_fw_layout_entry *
pvr_fw_find_layout_entry(const struct pvr_fw_layout_entry *layout_entries, u32 num_layout_entries,
			 enum pvr_fw_section_id id)
{
	u32 entry;

	for (entry = 0; entry < num_layout_entries; entry++) {
		if (layout_entries[entry].id == id)
			return &layout_entries[entry];
	}

	return NULL;
}

/**
 * pvr_fw_validate() - Parse firmware header and check compatibility
 * @pvr_dev: Device pointer.
 * @header_out: Pointer to location to write firmware header pointer.
 * @layout_entries_out: Pointer to location to write layout table pointer.
 *
 * Returns:
 *  * 0 on success, or
 *  * -EINVAL if firmware is incompatible.
 */
static int
pvr_fw_validate(struct pvr_device *pvr_dev,
		const struct pvr_fw_info_header **header_out,
		const struct pvr_fw_layout_entry **layout_entries_out)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	const u8 *fw = pvr_dev->fw->data;
	u32 fw_offset = pvr_dev->fw->size - SZ_4K;
	const struct pvr_fw_layout_entry *layout_entries;
	const struct pvr_fw_info_header *header;
	u32 layout_table_size;
	u32 entry;
	int err;

	if ((pvr_dev->fw->size < SZ_4K) ||
	    (pvr_dev->fw->size % FW_BLOCK_SIZE)) {
		err = -EINVAL;
		goto err_out;
	}

	header = (const struct pvr_fw_info_header *)&fw[fw_offset];

	if (header->info_version != PVR_FW_INFO_VERSION) {
		drm_err(drm_dev, "Unsupported fw info version %u\n",
			header->info_version);
		err = -EINVAL;
		goto err_out;
	}

	if (header->header_len != sizeof(struct pvr_fw_info_header) ||
	    header->layout_entry_size != sizeof(struct pvr_fw_layout_entry) ||
	    header->layout_entry_num > PVR_FW_INFO_MAX_NUM_ENTRIES) {
		drm_err(drm_dev, "FW info format mismatch\n");
		err = -EINVAL;
		goto err_out;
	}

	if (pvr_version_to_packed_bvnc(&pvr_dev->version) != header->bvnc) {
		struct pvr_version fw_version;

		packed_bvnc_to_pvr_version(header->bvnc, &fw_version);
		drm_err(drm_dev, "Unsupported fw version %i.%i.%i.%i\n",
			fw_version.b, fw_version.v, fw_version.n, fw_version.c);
		err = -EINVAL;
		goto err_out;
	}

	fw_offset += header->header_len;
	layout_table_size =
		header->layout_entry_size * header->layout_entry_num;
	if ((fw_offset + layout_table_size) > pvr_dev->fw->size) {
		err = -EINVAL;
		goto err_out;
	}

	layout_entries = (const struct pvr_fw_layout_entry *)&fw[fw_offset];
	for (entry = 0; entry < header->layout_entry_num; entry++) {
		u32 start_addr = layout_entries[entry].base_addr;
		u32 end_addr = start_addr + layout_entries[entry].alloc_size;

		if (start_addr >= end_addr) {
			err = -EINVAL;
			goto err_out;
		}
	}

	*header_out = header;
	*layout_entries_out = layout_entries;

	return 0;

err_out:
	return err;
}

static void
layout_get_sizes(const struct pvr_fw_layout_entry *layout_entries,
		 u32 num_layout_entries, u32 *code_alloc_size,
		 u32 *data_alloc_size, u32 *core_code_alloc_size,
		 u32 *core_data_alloc_size)
{
	u32 entry;

	*code_alloc_size = 0;
	*data_alloc_size = 0;
	*core_code_alloc_size = 0;
	*core_data_alloc_size = 0;

	/* Extract section sizes from FW layout table. */
	for (entry = 0; entry < num_layout_entries; entry++) {
		switch (layout_entries[entry].type) {
		case FW_CODE:
			(*code_alloc_size) += layout_entries[entry].alloc_size;
			break;
		case FW_DATA:
			(*data_alloc_size) += layout_entries[entry].alloc_size;
			break;
		case FW_COREMEM_CODE:
			(*core_code_alloc_size) +=
				layout_entries[entry].alloc_size;
			break;
		case FW_COREMEM_DATA:
			(*core_data_alloc_size) +=
				layout_entries[entry].alloc_size;
			break;
		case NONE:
			break;
		}
	}
}

int
pvr_fw_find_mmu_segment(u32 addr, u32 size, const struct pvr_fw_layout_entry *layout_entries,
			u32 num_layout_entries, void *fw_code_ptr, void *fw_data_ptr,
			void *fw_core_code_ptr, void *fw_core_data_ptr,
			void **host_addr_out)
{
	u32 end_addr = addr + size;
	int entry = 0;
	int err;

	/* Ensure requested range is not zero, and size is not causing addr to overflow. */
	if (end_addr <= addr) {
		err = -EINVAL;
		goto err_out;
	}

	for (entry = 0; entry < num_layout_entries; entry++) {
		u32 entry_start_addr = layout_entries[entry].base_addr;
		u32 entry_end_addr = entry_start_addr + layout_entries[entry].alloc_size;

		if (addr >= entry_start_addr && addr < entry_end_addr &&
		    end_addr > entry_start_addr && end_addr <= entry_end_addr) {
			switch (layout_entries[entry].type) {
			case FW_CODE:
				*host_addr_out = fw_code_ptr;
				break;

			case FW_DATA:
				*host_addr_out = fw_data_ptr;
				break;

			case FW_COREMEM_CODE:
				*host_addr_out = fw_core_code_ptr;
				break;

			case FW_COREMEM_DATA:
				*host_addr_out = fw_core_data_ptr;
				break;

			default:
				err = -EINVAL;
				goto err_out;
			}
			/* Direct Mem write to mapped memory */
			addr -= layout_entries[entry].base_addr;
			addr += layout_entries[entry].alloc_offset;

			/*
			 * Add offset to pointer to FW allocation only if that
			 * allocation is available
			 */
			*(u8 **)host_addr_out += addr;
			return 0;
		}
	}

	err = -EINVAL;

err_out:
	return err;
}

static int
pvr_fw_create_fwif_connection_ctl(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	int err;

	pvr_dev->fwif_connection_ctl = pvr_gem_create_and_map_fw_object_offset(pvr_dev,
		pvr_dev->fw_heap_info.config_offset + PVR_ROGUE_FWIF_CONNECTION_CTL_OFFSET,
		sizeof(*pvr_dev->fwif_connection_ctl),
		PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED,
		&pvr_dev->fwif_connection_ctl_obj);
	if (IS_ERR(pvr_dev->fwif_connection_ctl)) {
		drm_err(drm_dev,
			"Unable to allocate FWIF connection control memory\n");
		err = PTR_ERR(pvr_dev->fwif_connection_ctl);
		goto err_out;
	}

	return 0;

err_out:
	return err;
}

static void
pvr_fw_fini_fwif_connection_ctl(struct pvr_device *pvr_dev)
{
	pvr_fw_object_vunmap(pvr_dev->fwif_connection_ctl_obj,
			     pvr_dev->fwif_connection_ctl, false);
	pvr_fw_object_release(pvr_dev->fwif_connection_ctl_obj);
}

static int
pvr_fw_create_os_structures(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct rogue_fwif_hwrinfobuf *hwrinfobuf;
	int err;

	pvr_dev->fw_osinit = pvr_gem_create_and_map_fw_object_offset(pvr_dev,
		pvr_dev->fw_heap_info.config_offset + PVR_ROGUE_FWIF_OSINIT_OFFSET,
		sizeof(*pvr_dev->fw_osinit), PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED,
		&pvr_dev->fw_osinit_obj);
	if (IS_ERR(pvr_dev->fw_osinit)) {
		drm_err(drm_dev, "Unable to allocate FW OSINIT structure\n");
		err = PTR_ERR(pvr_dev->fw_osinit);
		goto err_out;
	}

	pvr_dev->fw_osdata = pvr_gem_create_and_map_fw_object(
		pvr_dev, sizeof(*pvr_dev->fw_osdata),
		PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED,
		&pvr_dev->fw_osdata_obj);
	if (IS_ERR(pvr_dev->fw_osdata)) {
		drm_err(drm_dev, "Unable to allocate FW OSDATA structure\n");
		err = PTR_ERR(pvr_dev->fw_osdata);
		goto err_release_osinit;
	}

	hwrinfobuf = pvr_gem_create_and_map_fw_object(
		pvr_dev, sizeof(*hwrinfobuf),
		PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED,
		&pvr_dev->fw_hwrinfobuf_obj);
	if (IS_ERR(hwrinfobuf)) {
		drm_err(drm_dev,
			"Unable to allocate FW hwrinfobuf structure\n");
		err = PTR_ERR(hwrinfobuf);
		goto err_release_osdata;
	}

	err = pvr_gem_create_fw_object(pvr_dev, PVR_SYNC_OBJ_SIZE,
				       PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
				       DRM_PVR_BO_CREATE_ZEROED,
				       &pvr_dev->fw_mmucache_sync_obj);
	if (err) {
		drm_err(drm_dev,
			"Unable to allocate MMU cache sync object\n");
		goto err_release_hwrinfobuf;
	}

	pvr_dev->fw_osinit->kernel_ccbctl_fw_addr =
		pvr_dev->kccb.ctrl_fw_addr;
	pvr_dev->fw_osinit->kernel_ccb_fw_addr = pvr_dev->kccb.ccb_fw_addr;
	WARN_ON(!pvr_gem_get_fw_addr(pvr_dev->kccb_rtn_obj,
				     &pvr_dev->fw_osinit->kernel_ccb_rtn_slots_fw_addr));

	pvr_dev->fw_osinit->firmware_ccbctl_fw_addr =
		pvr_dev->fwccb.ctrl_fw_addr;
	pvr_dev->fw_osinit->firmware_ccb_fw_addr = pvr_dev->fwccb.ccb_fw_addr;

	pvr_dev->fw_osinit->work_est_firmware_ccbctl_fw_addr = 0;
	pvr_dev->fw_osinit->work_est_firmware_ccb_fw_addr = 0;

	WARN_ON(!pvr_gem_get_fw_addr(
		pvr_dev->fw_hwrinfobuf_obj,
		&pvr_dev->fw_osinit->rogue_fwif_hwr_info_buf_ctl_fw_addr));
	WARN_ON(!pvr_gem_get_fw_addr(pvr_dev->fw_osdata_obj,
				&pvr_dev->fw_osinit->fw_os_data_fw_addr));

	pvr_dev->fw_osinit->hwr_debug_dump_limit = 0;

	ROGUE_FWIF_COMPCHECKS_BVNC_INIT(
		pvr_dev->fw_osinit->rogue_comp_checks.hw_bvnc);
	ROGUE_FWIF_COMPCHECKS_BVNC_INIT(
		pvr_dev->fw_osinit->rogue_comp_checks.fw_bvnc);

	pvr_fw_object_vunmap(pvr_dev->fw_hwrinfobuf_obj, hwrinfobuf, false);
	/* fw_osinit_obj and fw_osdata_obj remain mapped on the CPU. */
	return 0;

err_release_hwrinfobuf:
	pvr_fw_object_vunmap(pvr_dev->fw_hwrinfobuf_obj, hwrinfobuf, false);
	pvr_fw_object_release(pvr_dev->fw_hwrinfobuf_obj);

err_release_osdata:
	pvr_fw_object_vunmap(pvr_dev->fw_osdata_obj, pvr_dev->fw_osdata, false);
	pvr_fw_object_release(pvr_dev->fw_osdata_obj);

err_release_osinit:
	pvr_fw_object_vunmap(pvr_dev->fw_osinit_obj, pvr_dev->fw_osinit, false);
	pvr_fw_object_release(pvr_dev->fw_osinit_obj);

err_out:
	return err;
}

static void
pvr_fw_destroy_os_structures(struct pvr_device *pvr_dev)
{
	pvr_fw_object_release(pvr_dev->fw_mmucache_sync_obj);
	pvr_fw_object_release(pvr_dev->fw_hwrinfobuf_obj);
	pvr_fw_object_vunmap(pvr_dev->fw_osdata_obj, pvr_dev->fw_osdata, false);
	pvr_fw_object_release(pvr_dev->fw_osdata_obj);
	pvr_fw_object_vunmap(pvr_dev->fw_osinit_obj, pvr_dev->fw_osinit, false);
	pvr_fw_object_release(pvr_dev->fw_osinit_obj);
}

static int
pvr_fw_create_dev_structures(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct rogue_fwif_sysdata *sysdata;
	struct rogue_fwif_gpu_util_fwcb *gpu_util_fwcb;
	struct rogue_fwif_runtime_cfg *runtime_cfg;
	u32 clock_speed_hz;
	u32 *fault_page;
	dma_addr_t fault_dma_addr;
	int i;
	int err;

	pvr_dev->fw_sysinit = pvr_gem_create_and_map_fw_object_offset(pvr_dev,
		pvr_dev->fw_heap_info.config_offset + PVR_ROGUE_FWIF_SYSINIT_OFFSET,
		sizeof(*pvr_dev->fw_sysinit),
		PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED,
		&pvr_dev->fw_sysinit_obj);
	if (IS_ERR(pvr_dev->fw_sysinit)) {
		drm_err(drm_dev, "Unable to allocate FW SYSINIT structure\n");
		err = PTR_ERR(pvr_dev->fw_sysinit);
		goto err_out;
	}

	sysdata = pvr_gem_create_and_map_fw_object(pvr_dev, sizeof(*sysdata),
						   PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
						   DRM_PVR_BO_CREATE_ZEROED,
						   &pvr_dev->fw_sysdata_obj);
	if (IS_ERR(sysdata)) {
		drm_err(drm_dev, "Unable to allocate FW SYSDATA structure\n");
		err = PTR_ERR(sysdata);
		goto err_release_sysinit;
	}
	sysdata->config_flags = 0;
	sysdata->config_flags_ext = 0;
	pvr_fw_object_vunmap(pvr_dev->fw_sysdata_obj, sysdata, false);

	fault_page = pvr_gem_create_and_map_fw_object(pvr_dev, PVR_ROGUE_FAULT_PAGE_SIZE,
						      PVR_BO_FW_FLAGS_DEVICE_UNCACHED,
						      &pvr_dev->fw_fault_page_obj);
	if (IS_ERR(fault_page)) {
		drm_err(drm_dev, "Unable to allocate FW fault page\n");
		err = PTR_ERR(fault_page);
		goto err_release_sysdata;
	}
	for (i = 0; i < PVR_ROGUE_FAULT_PAGE_SIZE / sizeof(*fault_page); i++)
		fault_page[i] = 0xdeadbee0;
	pvr_fw_object_vunmap(pvr_dev->fw_fault_page_obj, fault_page, false);

	gpu_util_fwcb = pvr_gem_create_and_map_fw_object(pvr_dev, sizeof(*gpu_util_fwcb),
							 PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
							 DRM_PVR_BO_CREATE_ZEROED,
							 &pvr_dev->fw_gpu_util_fwcb_obj);
	if (IS_ERR(gpu_util_fwcb)) {
		drm_err(drm_dev, "Unable to allocate GPU util FWCB\n");
		err = PTR_ERR(gpu_util_fwcb);
		goto err_release_fault_page;
	}
	/* TODO : add timestamp. */
	gpu_util_fwcb->last_word = PVR_FWIF_GPU_UTIL_STATE_IDLE;
	pvr_fw_object_vunmap(pvr_dev->fw_gpu_util_fwcb_obj, gpu_util_fwcb, false);

	err = pvr_device_clk_core_get_freq(pvr_dev, &clock_speed_hz);
	if (err) {
		drm_err(drm_dev, "Unable to determine core clock frequency\n");
		goto err_release_gpu_util_fwcb;
	}

	runtime_cfg = pvr_gem_create_and_map_fw_object(pvr_dev, sizeof(*runtime_cfg),
						       PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
						       DRM_PVR_BO_CREATE_ZEROED,
						       &pvr_dev->fw_runtime_cfg_obj);
	if (IS_ERR(runtime_cfg)) {
		drm_err(drm_dev, "Unable to allocate FW runtime config\n");
		err = PTR_ERR(runtime_cfg);
		goto err_release_gpu_util_fwcb;
	}
	runtime_cfg->core_clock_speed = clock_speed_hz;
	runtime_cfg->active_pm_latency_ms = 0;
	runtime_cfg->active_pm_latency_persistant = true;
	WARN_ON(PVR_FEATURE_VALUE(pvr_dev, num_clusters,
				  &runtime_cfg->default_dusts_num_init) != 0);
	pvr_fw_object_vunmap(pvr_dev->fw_runtime_cfg_obj, runtime_cfg, false);

	err = pvr_fw_trace_init(pvr_dev);
	if (err)
		goto err_release_runtime_cfg;

	err = pvr_fw_get_dma_addr(pvr_dev->fw_fault_page_obj, 0, &fault_dma_addr);
	if (err) {
		drm_err(drm_dev,
			"Unable to get FW fault page physical address\n");
		goto err_trace_fini;
	}
	pvr_dev->fw_sysinit->fault_phys_addr = (u64)fault_dma_addr;

	pvr_dev->fw_sysinit->pds_exec_base = ROGUE_PDSCODEDATA_HEAP_BASE;
	pvr_dev->fw_sysinit->usc_exec_base = ROGUE_USCCODE_HEAP_BASE;

	WARN_ON(!pvr_gem_get_fw_addr(
		pvr_dev->fw_runtime_cfg_obj,
		&pvr_dev->fw_sysinit->runtime_cfg_fw_addr));
	WARN_ON(!pvr_gem_get_fw_addr(
		pvr_dev->fw_trace.tracebuf_ctrl_obj,
		&pvr_dev->fw_sysinit->trace_buf_ctl_fw_addr));
	WARN_ON(!pvr_gem_get_fw_addr(
		pvr_dev->fw_sysdata_obj,
		&pvr_dev->fw_sysinit->fw_sys_data_fw_addr));
	WARN_ON(!pvr_gem_get_fw_addr(
		pvr_dev->fw_gpu_util_fwcb_obj,
		&pvr_dev->fw_sysinit->gpu_util_fw_cb_ctl_fw_addr));
	if (pvr_dev->fw_core_data_obj) {
		WARN_ON(!pvr_gem_get_fw_addr(
			pvr_dev->fw_core_data_obj,
			&pvr_dev->fw_sysinit->coremem_data_store.fw_addr));
	}

	/* Currently unsupported. */
	pvr_dev->fw_sysinit->counter_dump_ctl.buffer_fw_addr = 0;
	pvr_dev->fw_sysinit->counter_dump_ctl.size_in_dwords = 0;

	/* Skip alignment checks. */
	pvr_dev->fw_sysinit->align_checks = 0;

	pvr_dev->fw_sysinit->filter_flags = 0;
	pvr_dev->fw_sysinit->hw_perf_filter = 0;
	pvr_dev->fw_sysinit->firmware_perf = FW_PERF_CONF_NONE;
	pvr_dev->fw_sysinit->initial_core_clock_speed = clock_speed_hz;
	pvr_dev->fw_sysinit->active_pm_latency_ms = 0;
	pvr_dev->fw_sysinit->gpio_validation_mode = ROGUE_FWIF_GPIO_VAL_OFF;
	pvr_dev->fw_sysinit->firmware_started = false;
	pvr_dev->fw_sysinit->marker_val = 1;

	memset(&pvr_dev->fw_sysinit->bvnc_km_feature_flags, 0,
	       sizeof(pvr_dev->fw_sysinit->bvnc_km_feature_flags));

	return 0;

err_trace_fini:
	pvr_fw_trace_fini(pvr_dev);

err_release_runtime_cfg:
	pvr_fw_object_release(pvr_dev->fw_runtime_cfg_obj);

err_release_gpu_util_fwcb:
	pvr_fw_object_release(pvr_dev->fw_gpu_util_fwcb_obj);

err_release_fault_page:
	pvr_fw_object_release(pvr_dev->fw_fault_page_obj);

err_release_sysdata:
	pvr_fw_object_release(pvr_dev->fw_sysdata_obj);

err_release_sysinit:
	pvr_fw_object_vunmap(pvr_dev->fw_sysinit_obj, pvr_dev->fw_sysinit, false);
	pvr_fw_object_release(pvr_dev->fw_sysinit_obj);

err_out:
	return err;
}

static void
pvr_fw_destroy_dev_structures(struct pvr_device *pvr_dev)
{
	pvr_fw_trace_fini(pvr_dev);
	pvr_fw_object_release(pvr_dev->fw_runtime_cfg_obj);
	pvr_fw_object_release(pvr_dev->fw_gpu_util_fwcb_obj);
	pvr_fw_object_release(pvr_dev->fw_fault_page_obj);
	pvr_fw_object_release(pvr_dev->fw_sysdata_obj);
	pvr_fw_object_vunmap(pvr_dev->fw_sysinit_obj, pvr_dev->fw_sysinit, false);
	pvr_fw_object_release(pvr_dev->fw_sysinit_obj);
}

/**
 * pvr_fw_process() - Process firmware image, allocate FW memory and create boot
 *                    arguments
 * @pvr_dev: Device pointer.
 *
 * Returns:
 *  * 0 on success, or
 *  * Any error returned by pvr_gem_create_and_map_fw_object_offset(), or
 *  * Any error returned by pvr_gem_create_and_map_fw_object().
 */
static int
pvr_fw_process(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	const u8 *fw = pvr_dev->fw->data;
	const struct pvr_fw_info_header *header;
	const struct pvr_fw_layout_entry *layout_entries;
	u32 code_alloc_size;
	u32 data_alloc_size;
	u32 core_code_alloc_size;
	u32 core_data_alloc_size;
	u8 *fw_code_ptr;
	u8 *fw_data_ptr;
	u8 *fw_core_code_ptr;
	u8 *fw_core_data_ptr;
	int err;

	err = pvr_fw_validate(pvr_dev, &header, &layout_entries);
	if (err)
		goto err_out;

	layout_get_sizes(layout_entries, header->layout_entry_num,
			 &code_alloc_size, &data_alloc_size,
			 &core_code_alloc_size, &core_data_alloc_size);

	/* Allocate and map memory for firmware sections. */

	/*
	 * Code allocation must be at the start of the firmware heap, otherwise
	 * firmware processor will be unable to boot.
	 *
	 * This has the useful side-effect that for every other object in the
	 * driver, a firmware address of 0 is invalid.
	 */
	fw_code_ptr = pvr_gem_create_and_map_fw_object_offset(pvr_dev, 0, code_alloc_size,
		PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED, &pvr_dev->fw_code_obj);
	if (IS_ERR(fw_code_ptr)) {
		drm_err(drm_dev, "Unable to allocate FW code memory\n");
		err = PTR_ERR(fw_code_ptr);
		goto err_out;
	}

	fw_data_ptr = pvr_gem_create_and_map_fw_object(pvr_dev, data_alloc_size,
		PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED, &pvr_dev->fw_data_obj);
	if (IS_ERR(fw_data_ptr)) {
		drm_err(drm_dev, "Unable to allocate FW data memory\n");
		err = PTR_ERR(fw_data_ptr);
		goto err_free_fw_code_obj;
	}

	/* Core code and data sections are optional. */
	if (core_code_alloc_size) {
		fw_core_code_ptr = pvr_gem_create_and_map_fw_object(pvr_dev, core_code_alloc_size,
			PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED,
			&pvr_dev->fw_core_code_obj);
		if (IS_ERR(fw_core_code_ptr)) {
			drm_err(drm_dev,
				"Unable to allocate FW core code memory\n");
			err = PTR_ERR(fw_core_code_ptr);
			goto err_free_fw_data_obj;
		}
	} else {
		fw_core_code_ptr = NULL;
	}

	if (core_data_alloc_size) {
		fw_core_data_ptr = pvr_gem_create_and_map_fw_object(pvr_dev, core_data_alloc_size,
			PVR_BO_FW_FLAGS_DEVICE_UNCACHED | DRM_PVR_BO_CREATE_ZEROED,
			&pvr_dev->fw_core_data_obj);
		if (IS_ERR(fw_core_data_ptr)) {
			drm_err(drm_dev,
				"Unable to allocate FW core data memory\n");
			err = PTR_ERR(fw_core_data_ptr);
			goto err_free_fw_core_code_obj;
		}
	} else {
		fw_core_data_ptr = NULL;
	}

	err = pvr_dev->fw_funcs->fw_process(pvr_dev, fw, layout_entries, header->layout_entry_num,
					    fw_code_ptr, fw_data_ptr, fw_core_code_ptr,
					    fw_core_data_ptr, core_code_alloc_size);

	if (err)
		goto err_free_fw_core_data_obj;

	/* We're finished with the firmware section memory on the CPU, unmap. */
	if (fw_core_data_ptr)
		pvr_fw_object_vunmap(pvr_dev->fw_core_data_obj, fw_core_data_ptr, false);
	if (fw_core_code_ptr)
		pvr_fw_object_vunmap(pvr_dev->fw_core_code_obj, fw_core_code_ptr, false);
	pvr_fw_object_vunmap(pvr_dev->fw_data_obj, fw_data_ptr, false);
	fw_data_ptr = NULL;
	pvr_fw_object_vunmap(pvr_dev->fw_code_obj, fw_code_ptr, false);
	fw_code_ptr = NULL;

	err = pvr_fw_create_fwif_connection_ctl(pvr_dev);
	if (err)
		goto err_free_fw_core_data_obj;

	return 0;

err_free_fw_core_data_obj:
	if (fw_core_data_ptr) {
		pvr_fw_object_vunmap(pvr_dev->fw_core_data_obj, fw_core_data_ptr, false);
		pvr_fw_object_release(pvr_dev->fw_core_data_obj);
	}

err_free_fw_core_code_obj:
	if (fw_core_code_ptr) {
		pvr_fw_object_vunmap(pvr_dev->fw_core_code_obj, fw_core_code_ptr, false);
		pvr_fw_object_release(pvr_dev->fw_core_code_obj);
	}

err_free_fw_data_obj:
	if (fw_data_ptr)
		pvr_fw_object_vunmap(pvr_dev->fw_data_obj, fw_data_ptr, false);
	pvr_fw_object_release(pvr_dev->fw_data_obj);

err_free_fw_code_obj:
	if (fw_code_ptr)
		pvr_fw_object_vunmap(pvr_dev->fw_code_obj, fw_code_ptr, false);
	pvr_fw_object_release(pvr_dev->fw_code_obj);

err_out:
	return err;
}

static void
pvr_fw_cleanup(struct pvr_device *pvr_dev)
{
	pvr_fw_fini_fwif_connection_ctl(pvr_dev);

	if (pvr_dev->fw_core_code_obj)
		pvr_fw_object_release(pvr_dev->fw_core_code_obj);
	if (pvr_dev->fw_core_data_obj)
		pvr_fw_object_release(pvr_dev->fw_core_data_obj);
	pvr_fw_object_release(pvr_dev->fw_code_obj);
	pvr_fw_object_release(pvr_dev->fw_data_obj);
}

/**
 * pvr_wait_for_fw_boot() - Wait for firmware to finish booting
 * @pvr_dev: Target PowerVR device.
 *
 * Returns:
 *  * 0 on success, or
 *  * -%ETIMEDOUT if firmware fails to boot within timeout.
 */
int
pvr_wait_for_fw_boot(struct pvr_device *pvr_dev)
{
	ktime_t deadline = ktime_add_us(ktime_get(), FW_BOOT_TIMEOUT_USEC);

	while (ktime_to_ns(ktime_sub(deadline, ktime_get())) > 0) {
		if (READ_ONCE(pvr_dev->fw_sysinit->firmware_started))
			return 0;
	}

	return -ETIMEDOUT;
}

/*
 * pvr_fw_heap_info_init() - Calculate size and masks for FW heap
 * @pvr_dev: Target PowerVR device.
 * @log2_size: Log2 of raw heap size.
 * @reserved_size: Size of reserved area of heap, in bytes. May be zero.
 */
void
pvr_fw_heap_info_init(struct pvr_device *pvr_dev, u32 log2_size, u32 reserved_size)
{
	pvr_dev->fw_heap_info.gpu_addr = PVR_ROGUE_FW_MAIN_HEAP_BASE;
	pvr_dev->fw_heap_info.log2_size = log2_size;
	pvr_dev->fw_heap_info.reserved_size = reserved_size;
	pvr_dev->fw_heap_info.raw_size = 1 << pvr_dev->fw_heap_info.log2_size;
	pvr_dev->fw_heap_info.offset_mask = pvr_dev->fw_heap_info.raw_size - 1;
	pvr_dev->fw_heap_info.config_offset = pvr_dev->fw_heap_info.raw_size -
					      PVR_ROGUE_FW_CONFIG_HEAP_SIZE;
	pvr_dev->fw_heap_info.size = pvr_dev->fw_heap_info.raw_size -
				     (PVR_ROGUE_FW_CONFIG_HEAP_SIZE + reserved_size);
}

/**
 * pvr_fw_init() - Initialise and boot firmware
 * @pvr_dev: Target PowerVR device
 *
 * On successful completion of the function the PowerVR device will be
 * initialised and ready to use.
 *
 * Returns:
 *  * 0 on success,
 *  * -%EINVAL on invalid firmware image,
 *  * -%ENOMEM on out of memory, or
 *  * -%ETIMEDOUT if firmware processor fails to boot or on register poll timeout.
 */
int
pvr_fw_init(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	u32 kccb_size_log2 = ROGUE_FWIF_KCCB_NUMCMDS_LOG2_DEFAULT;
	u32 kccb_rtn_size = (1 << kccb_size_log2) * sizeof(*pvr_dev->kccb_rtn);
	u32 ddk_version;
	int err;

	if (pvr_dev->fw_processor_type == PVR_FW_PROCESSOR_TYPE_META) {
		pvr_dev->fw_funcs = &pvr_fw_funcs_meta;
	} else if (pvr_dev->fw_processor_type == PVR_FW_PROCESSOR_TYPE_MIPS) {
		pvr_dev->fw_funcs = &pvr_fw_funcs_mips;
	} else {
		err = -EINVAL;
		goto err_out;
	}

	err = pvr_dev->fw_funcs->init(pvr_dev);
	if (err)
		goto err_out;

	drm_mm_init(&pvr_dev->fw_mm, ROGUE_FW_HEAP_BASE, pvr_dev->fw_heap_info.raw_size);
	pvr_dev->fw_mm_base = ROGUE_FW_HEAP_BASE;
	spin_lock_init(&pvr_dev->fw_mm_lock);

	err = pvr_fw_process(pvr_dev);
	if (err)
		goto err_mm_takedown;

	/* Initialise KCCB and FWCCB. */
	err = pvr_kccb_init(pvr_dev);
	if (err)
		goto err_fw_cleanup;

	err = pvr_fwccb_init(pvr_dev);
	if (err)
		goto err_kccb_fini;

	/* Allocate memory for KCCB return slots. */
	pvr_dev->kccb_rtn = pvr_gem_create_and_map_fw_object(pvr_dev, kccb_rtn_size,
							     PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
							     DRM_PVR_BO_CREATE_ZEROED,
							     &pvr_dev->kccb_rtn_obj);
	if (IS_ERR(pvr_dev->kccb_rtn)) {
		err = PTR_ERR(pvr_dev->kccb_rtn);
		goto err_fwccb_fini;
	}

	err = pvr_fw_create_os_structures(pvr_dev);
	if (err)
		goto err_kccb_rtn_release;

	err = pvr_fw_create_dev_structures(pvr_dev);
	if (err)
		goto err_destroy_os_structures;

	err = pvr_dev->fw_funcs->start(pvr_dev);
	if (err)
		goto err_destroy_dev_structures;

	err = pvr_wait_for_fw_boot(pvr_dev);
	if (err) {
		drm_err(drm_dev, "Firmware failed to boot\n");
		goto err_fw_stop;
	}

	pvr_dev->fw_booted = true;

	/* Now that firmware has booted, we can get the firmware version. */
	ddk_version = pvr_dev->fw_osinit->rogue_comp_checks.ddk_version;
	pvr_dev->fw_version.major = ddk_version >> 16;
	pvr_dev->fw_version.minor = ddk_version & 0xffff;

	return 0;

err_fw_stop:
	pvr_dev->fw_funcs->stop(pvr_dev);

err_destroy_dev_structures:
	pvr_fw_destroy_dev_structures(pvr_dev);

err_destroy_os_structures:
	pvr_fw_destroy_os_structures(pvr_dev);

err_kccb_rtn_release:
	pvr_fw_object_vunmap(pvr_dev->kccb_rtn_obj, pvr_dev->kccb_rtn, false);
	pvr_fw_object_release(pvr_dev->kccb_rtn_obj);

err_fwccb_fini:
	pvr_ccb_fini(&pvr_dev->fwccb);

err_kccb_fini:
	pvr_ccb_fini(&pvr_dev->kccb);

err_fw_cleanup:
	pvr_fw_cleanup(pvr_dev);

err_mm_takedown:
	drm_mm_takedown(&pvr_dev->fw_mm);

	if (pvr_dev->fw_funcs->fini)
		pvr_dev->fw_funcs->fini(pvr_dev);

err_out:
	return err;
}

/**
 * pvr_fw_fini() - Shutdown firmware processor and free associated memory
 * @pvr_dev: Target PowerVR device
 */
void
pvr_fw_fini(struct pvr_device *pvr_dev)
{
	pvr_dev->fw_funcs->stop(pvr_dev);
	pvr_dev->fw_booted = false;
	pvr_fw_destroy_dev_structures(pvr_dev);
	pvr_fw_destroy_os_structures(pvr_dev);
	pvr_fw_object_vunmap(pvr_dev->kccb_rtn_obj, (void *)pvr_dev->kccb_rtn, false);
	pvr_fw_object_release(pvr_dev->kccb_rtn_obj);
	/*
	 * Ensure FWCCB worker has finished executing before destroying FWCCB. The IRQ handler has
	 * been unregistered at this point so no new work should be being submitted.
	 */
	flush_work(&pvr_dev->fwccb_work);
	pvr_ccb_fini(&pvr_dev->fwccb);
	pvr_ccb_fini(&pvr_dev->kccb);
	pvr_fw_cleanup(pvr_dev);

	drm_mm_takedown(&pvr_dev->fw_mm);

	if (pvr_dev->fw_funcs->fini)
		pvr_dev->fw_funcs->fini(pvr_dev);
}

/**
 * pvr_fw_mts_schedule() - Schedule work via an MTS kick
 * @pvr_dev: Target PowerVR device
 * @val: Kick mask. Should be a combination of %ROGUE_CR_MTS_SCHEDULE_*
 */
void
pvr_fw_mts_schedule(struct pvr_device *pvr_dev, u32 val)
{
	/* Ensure memory is flushed before kicking MTS. */
	wmb();

	PVR_CR_WRITE32(pvr_dev, MTS_SCHEDULE, val);

	/* Ensure the MTS kick goes through before continuing. */
	mb();
}
