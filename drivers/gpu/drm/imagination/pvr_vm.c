// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_vm.h"

#include "pvr_device.h"
#include "pvr_gem.h"
#include "pvr_rogue_heap_config.h"
#include "pvr_rogue_mmu_defs.h"

#include <drm/drm_gem.h>

#include <linux/bitops.h>
#include <linux/compiler_attributes.h>
#include <linux/container_of.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/interval_tree_generic.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/lockdep.h>
#include <linux/math.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/rbtree.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>

struct pvr_vm_mapping;

static int
pvr_vm_mapping_unmap(struct pvr_vm_context *vm_ctx, struct pvr_vm_mapping *mapping);
static void
pvr_vm_mapping_fini(struct pvr_vm_mapping *mapping);

/**
 * pvr_mmu_flush() - Request flush of MMU caches
 * @pvr_dev: Target PowerVR device
 *
 * This must be called following any possible change to the MMU page tables.
 *
 * Returns:
 *  * 0 on success, or
 *  * Any error returned by pvr_kccb_send_cmd(), or
 *  * Any error returned by pvr_kccb_wait_for_completion().
 */
int
pvr_vm_mmu_flush(struct pvr_device *pvr_dev)
{
	struct rogue_fwif_kccb_cmd cmd_mmu_cache;
	struct rogue_fwif_mmucachedata *cmd_mmu_cache_data =
		&cmd_mmu_cache.cmd_data.mmu_cache_data;
	u32 slot;
	int err;

	/* Can't flush MMU if the firmware hasn't booted yet. */
	if (!pvr_dev->fw_booted) {
		err = 0;
		goto err_out;
	}

	cmd_mmu_cache.cmd_type = ROGUE_FWIF_KCCB_CMD_MMUCACHE;
	/* Request a complete MMU flush, across all pagetable levels, TLBs and contexts. */
	cmd_mmu_cache_data->cache_flags = ROGUE_FWIF_MMUCACHEDATA_FLAGS_PT |
					  ROGUE_FWIF_MMUCACHEDATA_FLAGS_PD |
					  ROGUE_FWIF_MMUCACHEDATA_FLAGS_PC |
					  ROGUE_FWIF_MMUCACHEDATA_FLAGS_TLB |
					  ROGUE_FWIF_MMUCACHEDATA_FLAGS_INTERRUPT;
	WARN_ON(!pvr_gem_get_fw_addr(pvr_dev->fw_mmucache_sync_obj,
		&cmd_mmu_cache_data->mmu_cache_sync_fw_addr));
	cmd_mmu_cache_data->mmu_cache_sync_update_value = 0;

	err = pvr_kccb_send_cmd(pvr_dev, &cmd_mmu_cache, &slot);
	if (err)
		goto err_out;

	err = pvr_kccb_wait_for_completion(pvr_dev, slot, HZ);
	if (err)
		goto err_out;

err_out:
	return err;
}

/**
 * DOC: PowerVR Virtual Memory Handling
 *
 * There's a lot going on in this section... TODO
 *
 * .. admonition:: Note on page table naming
 *
 *    This file uses a different naming convention for page table levels than
 *    the original DDK materials. Since page table implementation details are
 *    not exposed outside this file, the description of the name mapping exists
 *    here in its normative form:
 *
 *    * L0 page table => DDK page table
 *    * L1 page table => DDK page directory
 *    * L2 page table => DDK page catalog
 *
 *    The variable/function naming convention in this file is
 *    ``page_table_lx_*``  where x is either ``0``, ``1`` or ``2`` to represent
 *    the level of the page table structure. The name ``page_table_*`` without
 *    the ``_lx`` suffix is used for references to the entire tree structure
 *    including all three levels, or an operation or value which is
 *    level-agnostic.
 */

/**
 * PVR_IDX_INVALID - Default value for a u16-based index.
 *
 * This value cannot be zero, since zero is a valid index value.
 */
#define PVR_IDX_INVALID ((u16)(-1))

/**
 * DOC: VM backing pages
 *
 * While the page tables hold memory accessible to the rest of the driver,
 * the page tables themselves must have memory allocated to back them. We
 * call this memory "VM backing pages". Conveniently, each page table is
 * (currently) exactly 4KiB, as defined by %PVR_VM_BACKING_PAGE_SIZE. We
 * currently support any CPU page size of this size or greater by ignoring
 * any space after the first 4KiB of a CPU page allocated as a VM backing page.
 *
 * .. admonition:: Future work
 *
 *    We should be able to efficiently support CPU page sizes greater than 4KiB
 *    by allocating multiple page tables to each CPU page. It is unlikely
 *    that we will ever need to support CPU page sizes less than 4KiB.
 *
 * Usage
 * -----
 * To add this functionality to a structure which wraps a raw page table, embed
 * an instance of &struct pvr_vm_backing_page in the wrapper struct. Call
 * pvr_vm_backing_page_init() to allocate and map the backing page, and
 * pvr_vm_backing_page_fini() to perform the reverse operations when you're
 * finished with it. Use pvr_vm_backing_page_sync() to flush the memory from
 * the host to the device. As this is an expensive operation (calling out to
 * dma_sync_single_for_device()), be sure to perform all necessary changes to
 * the backing memory before calling it.
 *
 * Between calls to pvr_vm_backing_page_init() and pvr_vm_backing_page_fini(),
 * the public fields of &struct pvr_vm_backing_page can be used to access the
 * allocated page. To access the memory from the CPU, use
 * &pvr_vm_backing_page.host_ptr. For an address which can be passed to the
 * device, use &pvr_vm_backing_page.dma_addr.
 *
 * It is expected that the embedded &struct pvr_vm_backing_page will be zeroed
 * before calling pvr_vm_backing_page_init(). In return,
 * pvr_vm_backing_page_fini() will re-zero it before returning. You can
 * therefore compare the value of either &pvr_vm_backing_page.dma_addr or
 * &pvr_vm_backing_page.host_ptr to zero or %NULL to check if the backing
 * page is ready for use.
 *
 * .. note:: This API is not expected to be exposed outside ``pvr_vm.c``.
 */
/**
 * DOC: VM backing pages (constants)
 *
 * .. c:macro:: PVR_VM_BACKING_PAGE_SIZE
 *
 *    Page size of a PowerVR device's integrated MMU. The CPU page size must be
 *    at least as large as this value for the current implementation; this is
 *    checked at compile-time.
 */

/**
 * PVR_VM_BACKING_PAGE_SIZE - Page size of a PowerVR device's integrated MMU.
 *
 * *This doc comment is not rendered - see DOC: VM backing pages (constants).*
 */
#define PVR_VM_BACKING_PAGE_SIZE SZ_4K
static_assert(PAGE_SIZE >= PVR_VM_BACKING_PAGE_SIZE);

/**
 * struct pvr_vm_backing_page - Represents a single page used to back a page
 *                              table of any level.
 */
struct pvr_vm_backing_page {
	/** @dma_addr: DMA (quasi-physical) address of this page. */
	dma_addr_t dma_addr;
	/** @host_ptr: CPU address of this page. */
	void *host_ptr;

	/**
	 * @pvr_dev: The PowerVR device to which this page is associated.
	 * **For internal use only.**
	 */
	struct pvr_device *pvr_dev;
};

/**
 * pvr_vm_backing_page_init() - Initialize a VM backing page.
 * @page: Target backing page.
 * @pvr_dev: Target PowerVR device.
 *
 * This function performs three distinct operations:
 *
 * 1. Allocate a single page,
 * 2. Map the page to the CPU, and
 * 3. Map the page to DMA-space.
 *
 * It is expected that @page be zeroed (e.g. from kzalloc()) before calling
 * this function.
 *
 * Return:
 *  * 0 on success, or
 *  * -%ENOMEM if allocation of the backing page or mapping of the backing
 *    page to DMA fails.
 */
static int
pvr_vm_backing_page_init(struct pvr_vm_backing_page *page,
			 struct pvr_device *pvr_dev)
{
	struct device *dev = from_pvr_device(pvr_dev)->dev;

	struct page *raw_page;
	int err;

	dma_addr_t dma_addr;
	void *host_ptr;

	raw_page = alloc_page(__GFP_ZERO | GFP_KERNEL);
	if (!raw_page) {
		err = -ENOMEM;
		goto err_out;
	}

	host_ptr = kmap(raw_page);

	dma_addr = dma_map_page(dev, raw_page, 0, PVR_VM_BACKING_PAGE_SIZE,
				DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma_addr)) {
		err = -ENOMEM;
		goto err_unmap_free_page;
	}

	page->dma_addr = dma_addr;
	page->host_ptr = host_ptr;
	page->pvr_dev = pvr_dev;

	return 0;

err_unmap_free_page:
	kunmap(raw_page);
	__free_page(raw_page);

err_out:
	return err;
}

/**
 * pvr_vm_backing_page_fini() - Teardown a VM backing page.
 * @page: Target backing page.
 *
 * This function performs the mirror operations to pvr_vm_backing_page_init(),
 * in reverse order:
 *
 * 1. Unmap the page from DMA-space,
 * 2. Unmap the page from the CPU, and
 * 3. Free the page.
 *
 * It also zeros @page.
 *
 * It is a no-op to call this function a second (or further) time on any @page.
 */
static void
pvr_vm_backing_page_fini(struct pvr_vm_backing_page *page)
{
	struct device *dev = from_pvr_device(page->pvr_dev)->dev;
	struct page *raw_page = kmap_to_page(page->host_ptr);

	/* Do nothing if no allocation is present. */
	if (!page->pvr_dev)
		return;

	dma_unmap_page(dev, page->dma_addr, PVR_VM_BACKING_PAGE_SIZE,
		       DMA_TO_DEVICE);

	kunmap(raw_page);

	__free_page(raw_page);

	memset(page, 0, sizeof(*page));
}

/**
 * pvr_vm_backing_page_sync() - Flush a VM backing page from the CPU to the
 *                              device.
 * @page: Target backing page.
 *
 * .. caution::
 *
 *    **This is an expensive function call.** Only call
 *    pvr_vm_backing_page_sync() once you're sure you have no more changes to
 *    make to the backing page in the immediate future.
 */
static void
pvr_vm_backing_page_sync(struct pvr_vm_backing_page *page)
{
	struct device *dev;

	/*
	 * Do nothing if no allocation is present. This may be the case if
	 * we are unmapping pages.
	 */
	if (!page->pvr_dev)
		return;

	dev = from_pvr_device(page->pvr_dev)->dev;

	dma_sync_single_for_device(dev, page->dma_addr,
				   PVR_VM_BACKING_PAGE_SIZE, DMA_TO_DEVICE);
}

/**
 * DOC: Raw page tables
 *
 * These types define the lowest level representation of the page table
 * structure. This is the format which a PowerVR device's MMU can interpret
 * directly. As such, their definitions are taken directly from hardware
 * documentation.
 *
 * To store additional information required by the driver, we use
 * :ref:`mirror page tables<Mirror page tables>`. In most cases, the mirror
 * types are the ones you want to use for handles.
 */

#define PVR_PAGE_TABLE_TYPEOF_ENTRY(level_) \
	typeof_member(struct pvr_page_table_l##level_##_entry_raw, val)

#define PVR_PAGE_TABLE_FIELD_GET(level_, field_, entry_)           \
	(((entry_).val &                                           \
	  ~ROGUE_MMUCTRL_PT_L##level_##_DATA_##field_##_CLRMSK) >> \
	 ROGUE_MMUCTRL_PT_L##level_##_DATA_##field_##_SHIFT)

#define PVR_PAGE_TABLE_FIELD_PREP(level_, field_, val_)            \
	((((PVR_PAGE_TABLE_TYPEOF_ENTRY(level_))(val_))            \
	  << ROGUE_MMUCTRL_PT_L##level_##_DATA_##field_##_SHIFT) & \
	 ~ROGUE_MMUCTRL_PT_L##level_##_DATA_##field_##_CLRMSK)

/**
 * struct pvr_page_table_l2_entry_raw - A single entry in a level 2 page table.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %ROGUE_MMUCTRL_ENTRY_SIZE_PC_VALUE.
 *
 * The value stored in this structure can be decoded using the following bitmap:
 *
 * .. flat-table::
 *    :widths: 1 5
 *    :stub-columns: 1
 *
 *    * - 31..4
 *      - **Level 1 Page Table Base Address:** Bits 39..12 of the L1
 *        page table base address, which is 4KiB aligned.
 *
 *    * - 3..2
 *      - *(reserved)*
 *
 *    * - 1
 *      - **Pending:** When valid bit is not set, indicates that a valid
 *        entry is pending and the MMU should wait for the driver to map
 *        the entry. This is used to support page demand mapping of
 *        memory.
 *
 *    * - 0
 *      - **Valid:** Indicates that the entry contains a valid L1 page
 *        table. If the valid bit is not set, then an attempted use of
 *        the page would result in a page fault.
 */
struct pvr_page_table_l2_entry_raw {
	/** @val: The raw value of this entry. */
	u32 val;
} __packed;
static_assert(sizeof(struct pvr_page_table_l2_entry_raw) * 8 ==
	      ROGUE_MMUCTRL_ENTRY_SIZE_PT_L2_VALUE);

static __always_inline bool
pvr_page_table_l2_entry_raw_is_valid(struct pvr_page_table_l2_entry_raw entry)
{
	return PVR_PAGE_TABLE_FIELD_GET(2, VALID, entry);
}

static __always_inline void
pvr_page_table_l2_entry_raw_set(struct pvr_page_table_l2_entry_raw *entry,
				dma_addr_t child_table_dma_addr)
{
	child_table_dma_addr >>= ROGUE_MMUCTRL_PT_L2_DATA_PT_L1_BASE_ALIGNSHIFT;

	entry->val =
		PVR_PAGE_TABLE_FIELD_PREP(2, VALID, true) |
		PVR_PAGE_TABLE_FIELD_PREP(2, ENTRY_PENDING, false) |
		PVR_PAGE_TABLE_FIELD_PREP(2, PT_L1_BASE, child_table_dma_addr);
}

static __always_inline void
pvr_page_table_l2_entry_raw_clear(struct pvr_page_table_l2_entry_raw *entry)
{
	entry->val = 0;
}

/**
 * struct pvr_page_table_l1_entry_raw - A single entry in a level 1 page table.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %ROGUE_MMUCTRL_ENTRY_SIZE_PD_VALUE.
 *
 * The value stored in this structure can be decoded using the following bitmap:
 *
 * .. flat-table::
 *    :widths: 1 5
 *    :stub-columns: 1
 *
 *    * - 63..41
 *      - *(reserved)*
 *
 *    * - 40
 *      - **Pending:** When valid bit is not set, indicates that a valid entry
 *        is pending and the MMU should wait for the driver to map the entry.
 *        This is used to support page demand mapping of memory.
 *
 *    * - 39..5
 *      - **Level 0 Page Table Base Address:** The way this value is
 *        interpreted depends on the page size. Bits not specified in the
 *        table below (e.g. bits 11..5 for page size 4KiB) should be
 *        considered reserved.
 *
 *        This table shows the bits used in an L1 page table entry to
 *        represent the Physical Table Base Address for a given Page Size.
 *        Since each L1 page table entry covers 2MiB of address space, the
 *        maximum page size is 2MiB.
 *
 *        .. flat-table::
 *           :widths: 1 1 1 1
 *           :header-rows: 1
 *           :stub-columns: 1
 *
 *           * - Page size
 *             - L0 page table base address bits
 *             - Number of L0 page table entries
 *             - Size of L0 page table
 *
 *           * - 4KiB
 *             - 39..12
 *             - 512
 *             - 4KiB
 *
 *           * - 16KiB
 *             - 39..10
 *             - 128
 *             - 1KiB
 *
 *           * - 64KiB
 *             - 39..8
 *             - 32
 *             - 256B
 *
 *           * - 256KiB
 *             - 39..6
 *             - 8
 *             - 64B
 *
 *           * - 1MiB
 *             - 39..5 (4 = '0')
 *             - 2
 *             - 16B
 *
 *           * - 2MiB
 *             - 39..5 (4..3 = '00')
 *             - 1
 *             - 8B
 *
 *    * - 4
 *      - *(reserved)*
 *
 *    * - 3..1
 *      - **Page Size:** Sets the page size, from 4KiB to 2MiB.
 *
 *    * - 0
 *      - **Valid:** Indicates that the entry contains a valid L0 page table.
 *        If the valid bit is not set, then an attempted use of the page would
 *        result in a page fault.
 */
struct pvr_page_table_l1_entry_raw {
	/** @val: The raw value of this entry. */
	u64 val;
} __packed;
static_assert(sizeof(struct pvr_page_table_l1_entry_raw) * 8 ==
	      ROGUE_MMUCTRL_ENTRY_SIZE_PT_L1_VALUE);

static __always_inline bool
pvr_page_table_l1_entry_raw_is_valid(struct pvr_page_table_l1_entry_raw entry)
{
	return PVR_PAGE_TABLE_FIELD_GET(1, VALID, entry);
}

static void
pvr_page_table_l1_entry_raw_set(struct pvr_page_table_l1_entry_raw *entry,
				dma_addr_t child_table_dma_addr)
{
	entry->val = PVR_PAGE_TABLE_FIELD_PREP(1, VALID, true) |
		     PVR_PAGE_TABLE_FIELD_PREP(1, ENTRY_PENDING, false) |
		     PVR_PAGE_TABLE_FIELD_PREP(1, PAGE_SIZE,
					       ROGUE_MMUCTRL_PAGE_SIZE_4KB) |
		     (child_table_dma_addr &
		      ~ROGUE_MMUCTRL_PT_L0_BASE_4KB_RANGE_CLRMSK);
}

static __always_inline void
pvr_page_table_l1_entry_raw_clear(struct pvr_page_table_l1_entry_raw *entry)
{
	entry->val = 0;
}

/**
 * struct pvr_page_table_l0_entry_raw - A single entry in a level 0 page table.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %ROGUE_MMUCTRL_ENTRY_SIZE_PT_VALUE.
 *
 * The value stored in this structure can be decoded using the following bitmap:
 *
 * .. flat-table::
 *    :widths: 1 5
 *    :stub-columns: 1
 *
 *    * - 63
 *      - *(reserved)*
 *
 *    * - 62
 *      - **PM/FW Protect:** Indicates a protected region which only the
 *        Parameter Manager (PM) or firmware processor can write to.
 *
 *    * - 61..40
 *      - **VP Page (High):** Virtual-physical page used for Parameter Manager
 *        (PM) memory. This field is only used if the additional level of PB
 *        virtualization is enabled. The VP Page field is needed by the PM in
 *        order to correctly reconstitute the free lists after render
 *        completion. This (High) field holds bits 39..18 of the value; the
 *        Low field holds bits 17..12. Bits 11..0 are always zero because the
 *        value is always aligned to the 4KiB page size.
 *
 *    * - 39..12
 *      - **Physical Page Address:** he way this value is
 *        interpreted depends on the page size. Bits not specified in the
 *        table below (e.g. bits 20..12 for page size 2MiB) should be
 *        considered reserved.
 *
 *        This table shows the bits used in an L0 page table entry to represent
 *        the Physical Page Address for a given page size (as defined in the
 *        associated L1 page table entry).
 *
 *        .. flat-table::
 *           :widths: 1 1
 *           :header-rows: 1
 *           :stub-columns: 1
 *
 *           * - Page size
 *             - Physical address bits
 *
 *           * - 4KiB
 *             - 39..12
 *
 *           * - 16KiB
 *             - 39..14
 *
 *           * - 64KiB
 *             - 39..16
 *
 *           * - 256KiB
 *             - 39..18
 *
 *           * - 1MiB
 *             - 39..20
 *
 *           * - 2MiB
 *             - 39..21
 *
 *    * - 11..6
 *      - **VP Page (Low):** Continuation of VP Page (High).
 *
 *    * - 5
 *      - **Pending:** When valid bit is not set, indicates that a valid entry
 *        is pending and the MMU should wait for the driver to map the entry.
 *        This is used to support page demand mapping of memory.
 *
 *    * - 4
 *      - **PM Src:** Set on Parameter Manager (PM) allocated page table
 *        entries when indicated by the PM. Note that this bit will only be set
 *        by the PM, not by the device driver.
 *
 *    * - 3
 *      - **SLC Bypass Control:** Specifies requests to this page should bypass
 *        the System Level Cache (SLC), if enabled in SLC configuration.
 *
 *    * - 2
 *      - **Cache Coherency:** Indicates that the page is coherent (i.e. it
 *        does not require a cache flush between operations on the CPU and the
 *        device).
 *
 *    * - 1
 *      - **Read Only:** If set, this bit indicates that the page is read only.
 *        An attempted write to this page would result in a write-protection
 *        fault.
 *
 *    * - 0
 *      - **Valid:** Indicates that the entry contains a valid page. If the
 *        valid bit is not set, then an attempted use of the page would result
 *        in a page fault.
 */
struct pvr_page_table_l0_entry_raw {
	/** @val: The raw value of this entry. */
	u64 val;
} __packed;
static_assert(sizeof(struct pvr_page_table_l0_entry_raw) * 8 ==
	      ROGUE_MMUCTRL_ENTRY_SIZE_PT_L0_VALUE);

/**
 * struct pvr_page_flags_raw - The configurable flags from a single entry in a
 * level 0 page table.
 *
 * The flags stored in this type are: PM/FW Protect; SLC Bypass Control; Cache
 * Coherency, and Read Only (bits 62, 3, 2 and 1 respectively).
 */
struct pvr_page_flags_raw {
	/**
	 * @val: The raw value of these flags. Since these are a strict subset
	 * of struct pvr_page_table_l0_entry_raw; use that type for our member
	 * here.
	 */
	struct pvr_page_table_l0_entry_raw val;
} __packed;
static_assert(sizeof(struct pvr_page_flags_raw) ==
	      sizeof(struct pvr_page_table_l0_entry_raw));

static __always_inline bool
pvr_page_table_l0_entry_raw_is_valid(struct pvr_page_table_l0_entry_raw entry)
{
	return PVR_PAGE_TABLE_FIELD_GET(0, VALID, entry);
}

static void
pvr_page_table_l0_entry_raw_set(struct pvr_page_table_l0_entry_raw *entry,
				dma_addr_t dma_addr,
				struct pvr_page_flags_raw flags)
{
	entry->val = PVR_PAGE_TABLE_FIELD_PREP(0, VALID, true) |
		     PVR_PAGE_TABLE_FIELD_PREP(0, ENTRY_PENDING, false) |
		     (dma_addr & ~ROGUE_MMUCTRL_PAGE_4KB_RANGE_CLRMSK) |
		     flags.val.val;
}

static __always_inline void
pvr_page_table_l0_entry_raw_clear(struct pvr_page_table_l0_entry_raw *entry)
{
	entry->val = 0;
}

static struct pvr_page_flags_raw
pvr_page_flags_raw_create(bool read_only, bool cache_coherent, bool slc_bypass,
			  bool pm_fw_protect)
{
	struct pvr_page_flags_raw flags;

	flags.val.val =
		PVR_PAGE_TABLE_FIELD_PREP(0, READ_ONLY, read_only) |
		PVR_PAGE_TABLE_FIELD_PREP(0, CC, cache_coherent) |
		PVR_PAGE_TABLE_FIELD_PREP(0, SLC_BYPASS_CTRL, slc_bypass) |
		PVR_PAGE_TABLE_FIELD_PREP(0, PM_META_PROTECT, pm_fw_protect);

	return flags;
}

/**
 * struct pvr_page_table_l2_raw - The raw data of an L2 page table.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %PVR_VM_BACKING_PAGE_SIZE.
 */
struct pvr_page_table_l2_raw {
	/** @entries: The raw values of this table. */
	struct pvr_page_table_l2_entry_raw
		entries[ROGUE_MMUCTRL_ENTRIES_PT_L2_VALUE];
} __packed;
static_assert(sizeof(struct pvr_page_table_l2_raw) == PVR_VM_BACKING_PAGE_SIZE);

/**
 * struct pvr_page_table_l1_raw - The raw data of an L1 page table.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %PVR_VM_BACKING_PAGE_SIZE.
 */
struct pvr_page_table_l1_raw {
	/** @entries: The raw values of this table. */
	struct pvr_page_table_l1_entry_raw
		entries[ROGUE_MMUCTRL_ENTRIES_PT_L1_VALUE];
} __packed;
static_assert(sizeof(struct pvr_page_table_l1_raw) == PVR_VM_BACKING_PAGE_SIZE);

/**
 * struct pvr_page_table_l0_raw - The raw data of an L0 page table.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %PVR_VM_BACKING_PAGE_SIZE.
 *
 * .. caution::
 *
 *    The size of L0 page tables is variable depending on the page size
 *    specified in the associated L1 page table entry. For simplicity, this
 *    type is fixed to contain the maximum possible number of entries.
 *    **You should never read or write beyond the last supported entry.**
 */
struct pvr_page_table_l0_raw {
	/** @entries: The raw values of this table. */
	struct pvr_page_table_l0_entry_raw
		entries[ROGUE_MMUCTRL_ENTRIES_PT_L0_VALUE];
} __packed;
static_assert(sizeof(struct pvr_page_table_l0_raw) == PVR_VM_BACKING_PAGE_SIZE);

/**
 * DOC: Mirror page tables
 *
 * These structures hold additional information required by the driver that
 * cannot be stored in :ref:`raw page tables<Raw page tables>` (since those are
 * defined by the hardware).
 *
 * In most cases, you should hold a handle to these types instead of the raw
 * types directly.
 */

/*
 * We pre-declare these types because they cross-depend on pointers to each
 * other.
 */
struct pvr_page_table_l2;
struct pvr_page_table_l1;
struct pvr_page_table_l0;

/**
 * struct pvr_page_table_l2 - A wrapped L2 page table.
 *
 * To access the raw part of this table, use pvr_page_table_l2_get_raw().
 * Alternatively to access a raw entry directly, use
 * pvr_page_table_l2_get_entry_raw().
 *
 * An L2 page table forms the root of the page table tree structure, so this
 * type has no &parent or &parent_idx members.
 */
struct pvr_page_table_l2 {
	/**
	 * @entries: The children of this node in the page table tree
	 * structure. These are also mirror tables. The indexing of this array
	 * is identical to that of the raw equivalent
	 * (&pvr_page_table_l1_raw.entries).
	 */
	struct pvr_page_table_l1 *entries[ROGUE_MMUCTRL_ENTRIES_PT_L2_VALUE];

	/**
	 * @backing_page: A handle to the memory which holds the raw
	 * equivalent of this table. **For internal use only.**
	 */
	struct pvr_vm_backing_page backing_page;

	/**
	 * @entry_count: The current number of valid entries (that we know of)
	 * in this table. This value is essentially a refcount - the table is
	 * destroyed when this value is decremented to zero by
	 * pvr_page_table_l2_remove().
	 */
	u16 entry_count;
};

/**
 * pvr_page_table_l2_init() - Initialize an L2 page table.
 * @table: Target L2 page table.
 * @pvr_dev: Target PowerVR device
 *
 * It is expected that @table be zeroed (e.g. from kzalloc()) before calling
 * this function.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_vm_backing_page_init().
 */
static __always_inline int
pvr_page_table_l2_init(struct pvr_page_table_l2 *table,
		       struct pvr_device *pvr_dev)
{
	return pvr_vm_backing_page_init(&table->backing_page, pvr_dev);
}

/**
 * pvr_page_table_l2_fini() - Teardown an L2 page table.
 * @table: Target L2 page table.
 *
 * It is an error to attempt to use @table after calling this function.
 */
static __always_inline void
pvr_page_table_l2_fini(struct pvr_page_table_l2 *table)
{
	pvr_vm_backing_page_fini(&table->backing_page);
}

/**
 * pvr_page_table_l2_sync() - Flush an L2 page table from the CPU to the device.
 * @table: Target L2 page table.
 *
 * This is just a thin wrapper around pvr_vm_backing_page_sync(), so the
 * warning there applies here too: **Only call pvr_page_table_l2_sync() once
 * you're sure you have no more changes to make to** @table **in the immediate
 * future.**
 *
 * If child L1 page tables of @table also need to be flushed, this should be
 * done first using pvr_page_table_l1_sync() *before* calling this function.
 */
static __always_inline void
pvr_page_table_l2_sync(struct pvr_page_table_l2 *table)
{
	pvr_vm_backing_page_sync(&table->backing_page);
}

/**
 * pvr_page_table_l2_get_raw() - Access the raw equivalent of a mirror L2 page
 *                               table.
 * @table: Target L2 page table.
 *
 * Essentially returns the CPU address of the raw equivalent of @table, cast to
 * a &struct pvr_page_table_l2_raw pointer.
 *
 * You probably want to call pvr_page_table_l2_get_entry_raw() instead.
 *
 * Return:
 *  * The raw equivalent of @table.
 */
static __always_inline struct pvr_page_table_l2_raw *
pvr_page_table_l2_get_raw(struct pvr_page_table_l2 *table)
{
	return table->backing_page.host_ptr;
}

/**
 * pvr_page_table_l2_get_entry_raw() - Access an entry from the raw equivalent
 *                                     of a mirror L2 page table.
 * @table: Target L2 page table.
 * @idx: Index of the entry to access.
 *
 * Technically this function returns a pointer to a slot in a raw L2 page
 * table, since the returned "entry" is not guaranteed to be valid. The caller
 * must verify the validity of the entry at the returned address before reading
 * or overwriting it.
 *
 * Return:
 *  * A pointer to the requested raw L2 page table entry.
 */
static __always_inline struct pvr_page_table_l2_entry_raw *
pvr_page_table_l2_get_entry_raw(struct pvr_page_table_l2 *table, u16 idx)
{
	return &pvr_page_table_l2_get_raw(table)->entries[idx];
}

/**
 * pvr_page_table_l2_entry_is_valid() - Check if an L2 page table entry is
 *                                      marked as valid.
 * @table: Target L2 page table.
 * @idx: Index of the entry to check.
 *
 * Return:
 *  * %true if there is a valid entry at @idx in @table, or
 *  * %false otherwise.
 */
static __always_inline bool
pvr_page_table_l2_entry_is_valid(struct pvr_page_table_l2 *table, u16 idx)
{
	struct pvr_page_table_l2_entry_raw entry_raw =
		*pvr_page_table_l2_get_entry_raw(table, idx);

	return pvr_page_table_l2_entry_raw_is_valid(entry_raw);
}

/**
 * struct pvr_page_table_l1 - A wrapped L1 page table.
 *
 * To access the raw part of this table, use pvr_page_table_l1_get_raw().
 * Alternatively to access a raw entry directly, use
 * pvr_page_table_l1_get_entry_raw().
 */
struct pvr_page_table_l1 {
	/**
	 * @entries: The children of this node in the page table tree
	 * structure. These are also mirror tables. The indexing of this array
	 * is identical to that of the raw equivalent
	 * (&pvr_page_table_l0_raw.entries).
	 */
	struct pvr_page_table_l0 *entries[ROGUE_MMUCTRL_ENTRIES_PT_L1_VALUE];

	/**
	 * @backing_page: A handle to the memory which holds the raw
	 * equivalent of this table. **For internal use only.**
	 */
	struct pvr_vm_backing_page backing_page;

	/**
	 * @parent: The parent of this node in the page table tree structure.
	 * This is also a mirror table.
	 */
	struct pvr_page_table_l2 *parent;

	/**
	 * @parent_idx: The index of the entry in the parent table (see
	 * @parent) which corresponds to this table.
	 */
	u16 parent_idx;

	/**
	 * @entry_count: The current number of valid entries (that we know of)
	 * in this table. This value is essentially a refcount - the table is
	 * destroyed when this value is decremented to zero by
	 * pvr_page_table_l1_remove().
	 */
	u16 entry_count;
};

/**
 * pvr_page_table_l1_init() - Initialize an L1 page table.
 * @table: Target L1 page table.
 * @pvr_dev: Target PowerVR device
 *
 * When this function returns successfully, @table is still not considered
 * valid. It must be inserted into the page table tree structure with
 * pvr_page_table_l2_insert() before it is ready for use.
 *
 * It is expected that @table be zeroed (e.g. from kzalloc()) before calling
 * this function.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_vm_backing_page_init().
 */
static __always_inline int
pvr_page_table_l1_init(struct pvr_page_table_l1 *table,
		       struct pvr_device *pvr_dev)
{
	table->parent_idx = PVR_IDX_INVALID;

	return pvr_vm_backing_page_init(&table->backing_page, pvr_dev);
}

/**
 * pvr_page_table_l1_fini() - Teardown an L1 page table.
 * @table: Target L1 page table.
 *
 * It is an error to attempt to use @table after calling this function, even
 * indirectly. This includes calling pvr_page_table_l2_remove(), which must
 * be called *before* pvr_page_table_l1_fini().
 */
static __always_inline void
pvr_page_table_l1_fini(struct pvr_page_table_l1 *table)
{
	pvr_vm_backing_page_fini(&table->backing_page);
}

/**
 * pvr_page_table_l1_sync() - Flush an L1 page table from the CPU to the device.
 * @table: Target L1 page table.
 *
 * This is just a thin wrapper around pvr_vm_backing_page_sync(), so the
 * warning there applies here too: **Only call pvr_page_table_l1_sync() once
 * you're sure you have no more changes to make to** @table **in the immediate
 * future.**
 *
 * If child L0 page tables of @table also need to be flushed, this should be
 * done first using pvr_page_table_l0_sync() *before* calling this function.
 */
static __always_inline void
pvr_page_table_l1_sync(struct pvr_page_table_l1 *table)
{
	pvr_vm_backing_page_sync(&table->backing_page);
}

/**
 * pvr_page_table_l1_get_raw() - Access the raw equivalent of a mirror L1 page
 *                               table.
 * @table: Target L1 page table.
 *
 * Essentially returns the CPU address of the raw equivalent of @table, cast to
 * a &struct pvr_page_table_l1_raw pointer.
 *
 * You probably want to call pvr_page_table_l1_get_entry_raw() instead.
 *
 * Return:
 *  * The raw equivalent of @table.
 */
static __always_inline struct pvr_page_table_l1_raw *
pvr_page_table_l1_get_raw(struct pvr_page_table_l1 *table)
{
	return table->backing_page.host_ptr;
}

/**
 * pvr_page_table_l1_get_entry_raw() - Access an entry from the raw equivalent
 *                                     of a mirror L1 page table.
 * @table: Target L1 page table.
 * @idx: Index of the entry to access.
 *
 * Technically this function returns a pointer to a slot in a raw L1 page
 * table, since the returned "entry" is not guaranteed to be valid. The caller
 * must verify the validity of the entry at the returned address before reading
 * or overwriting it.
 *
 * Return:
 *  * A pointer to the requested raw L1 page table entry.
 */
static __always_inline struct pvr_page_table_l1_entry_raw *
pvr_page_table_l1_get_entry_raw(struct pvr_page_table_l1 *table, u16 idx)
{
	return &pvr_page_table_l1_get_raw(table)->entries[idx];
}

/**
 * pvr_page_table_l1_entry_is_valid() - Check if an L1 page table entry is
 *                                      marked as valid.
 * @table: Target L1 page table.
 * @idx: Index of the entry to check.
 *
 * Return:
 *  * %true if there is a valid entry at @idx in @table, or
 *  * %false otherwise.
 */
static __always_inline bool
pvr_page_table_l1_entry_is_valid(struct pvr_page_table_l1 *table, u16 idx)
{
	struct pvr_page_table_l1_entry_raw entry_raw =
		*pvr_page_table_l1_get_entry_raw(table, idx);

	return pvr_page_table_l1_entry_raw_is_valid(entry_raw);
}

/**
 * struct pvr_page_table_l0 - A wrapped L0 page table.
 *
 * To access the raw part of this table, use pvr_page_table_l0_get_raw().
 * Alternatively to access a raw entry directly, use
 * pvr_page_table_l0_get_entry_raw().
 *
 * There is no mirror representation of an individual page, so this type has no
 * &entries member.
 */
struct pvr_page_table_l0 {
	/**
	 * @backing_page: A handle to the memory which holds the raw
	 * equivalent of this table. **For internal use only.**
	 */
	struct pvr_vm_backing_page backing_page;

	/**
	 * @parent: The parent of this node in the page table tree structure.
	 * This is also a mirror table.
	 */
	struct pvr_page_table_l1 *parent;

	/**
	 * @parent_idx: The index of the entry in the parent table (see
	 * @parent) which corresponds to this table.
	 */
	u16 parent_idx;

	/**
	 * @entry_count: The current number of valid entries (that we know of)
	 * in this table. This value is essentially a refcount - the table is
	 * destroyed when this value is decremented to zero by
	 * pvr_page_table_l0_remove().
	 */
	u16 entry_count;
};

/**
 * pvr_page_table_l0_init() - Initialize an L0 page table.
 * @table: Target L0 page table.
 * @pvr_dev: Target PowerVR device
 *
 * When this function returns successfully, @table is still not considered
 * valid. It must be inserted into the page table tree structure with
 * pvr_page_table_l1_insert() before it is ready for use.
 *
 * It is expected that @table be zeroed (e.g. from kzalloc()) before calling
 * this function.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_vm_backing_page_init().
 */
static __always_inline int
pvr_page_table_l0_init(struct pvr_page_table_l0 *table,
		       struct pvr_device *pvr_dev)
{
	table->parent_idx = PVR_IDX_INVALID;

	return pvr_vm_backing_page_init(&table->backing_page, pvr_dev);
}

/**
 * pvr_page_table_l0_fini() - Teardown an L0 page table.
 * @table: Target L0 page table.
 *
 * It is an error to attempt to use @table after calling this function, even
 * indirectly. This includes calling pvr_page_table_l1_remove(), which must
 * be called *before* pvr_page_table_l0_fini().
 */
static __always_inline void
pvr_page_table_l0_fini(struct pvr_page_table_l0 *table)
{
	pvr_vm_backing_page_fini(&table->backing_page);
}

/**
 * pvr_page_table_l0_sync() - Flush an L0 page table from the CPU to the device.
 * @table: Target L0 page table.
 *
 * This is just a thin wrapper around pvr_vm_backing_page_sync(), so the
 * warning there applies here too: **Only call pvr_page_table_l0_sync() once
 * you're sure you have no more changes to make to** @table **in the immediate
 * future.**
 *
 * If child pages of @table also need to be flushed, this should be done first
 * using a DMA sync function (e.g. dma_sync_sg_for_device()) *before* calling
 * this function.
 */
static __always_inline void
pvr_page_table_l0_sync(struct pvr_page_table_l0 *table)
{
	pvr_vm_backing_page_sync(&table->backing_page);
}

/**
 * pvr_page_table_l0_get_raw() - Access the raw equivalent of a mirror L0 page
 *                               table.
 * @table: Target L0 page table.
 *
 * Essentially returns the CPU address of the raw equivalent of @table, cast to
 * a &struct pvr_page_table_l0_raw pointer.
 *
 * You probably want to call pvr_page_table_l0_get_entry_raw() instead.
 *
 * Return:
 *  * The raw equivalent of @table.
 */
static __always_inline struct pvr_page_table_l0_raw *
pvr_page_table_l0_get_raw(struct pvr_page_table_l0 *table)
{
	return table->backing_page.host_ptr;
}

/**
 * pvr_page_table_l0_get_entry_raw() - Access an entry from the raw equivalent
 *                                     of a mirror L0 page table.
 * @table: Target L0 page table.
 * @idx: Index of the entry to access.
 *
 * Technically this function returns a pointer to a slot in a raw L0 page
 * table, since the returned "entry" is not guaranteed to be valid. The caller
 * must verify the validity of the entry at the returned address before reading
 * or overwriting it.
 *
 * Return:
 *  * A pointer to the requested raw L0 page table entry.
 */
static __always_inline struct pvr_page_table_l0_entry_raw *
pvr_page_table_l0_get_entry_raw(struct pvr_page_table_l0 *table, u16 idx)
{
	return &pvr_page_table_l0_get_raw(table)->entries[idx];
}

/**
 * pvr_page_table_l0_entry_is_valid() - Check if an L0 page table entry is
 *                                      marked as valid.
 * @table: Target L0 page table.
 * @idx: Index of the entry to check.
 *
 * Return:
 *  * %true if there is a valid entry at @idx in @table, or
 *  * %false otherwise.
 */
static __always_inline bool
pvr_page_table_l0_entry_is_valid(struct pvr_page_table_l0 *table, u16 idx)
{
	struct pvr_page_table_l0_entry_raw entry_raw =
		*pvr_page_table_l0_get_entry_raw(table, idx);

	return pvr_page_table_l0_entry_raw_is_valid(entry_raw);
}

/**
 * pvr_page_table_l2_insert() - TODO
 * @table: Target L2 page table.
 * @idx: Index of the entry to write.
 * @child_table: Target L1 page table to be referenced by the new entry.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is not already a valid entry at @idx in @table.
 */
static void
pvr_page_table_l2_insert(struct pvr_page_table_l2 *table, u16 idx,
			 struct pvr_page_table_l1 *child_table)
{
	struct pvr_page_table_l2_entry_raw *entry_raw =
		pvr_page_table_l2_get_entry_raw(table, idx);

	pvr_page_table_l2_entry_raw_set(entry_raw,
					child_table->backing_page.dma_addr);

	child_table->parent = table;
	child_table->parent_idx = idx;

	table->entries[idx] = child_table;
	++table->entry_count;
}

/**
 * pvr_page_table_l2_remove() - TODO
 * @table: Target L2 page table.
 * @idx: Index of the entry to remove.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is a valid entry at @idx in @table.
 */
static void
pvr_page_table_l2_remove(struct pvr_page_table_l2 *table, u16 idx)
{
	struct pvr_page_table_l1 *child_table = table->entries[idx];
	struct pvr_page_table_l2_entry_raw *entry_raw =
		pvr_page_table_l2_get_entry_raw(table, idx);

	pvr_page_table_l2_entry_raw_clear(entry_raw);

	child_table->parent = NULL;
	child_table->parent_idx = PVR_IDX_INVALID;

	table->entries[idx] = NULL;
	--table->entry_count;
}

/**
 * pvr_page_table_l1_insert() - TODO
 * @table: Target L1 page table.
 * @idx: Index of the entry to write.
 * @child_table: Target L0 page table to be referenced by the new entry.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is not already a valid entry at @idx in @table.
 */
static void
pvr_page_table_l1_insert(struct pvr_page_table_l1 *table, u16 idx,
			 struct pvr_page_table_l0 *child_table)
{
	struct pvr_page_table_l1_entry_raw *entry_raw =
		pvr_page_table_l1_get_entry_raw(table, idx);

	pvr_page_table_l1_entry_raw_set(entry_raw,
					child_table->backing_page.dma_addr);

	child_table->parent = table;
	child_table->parent_idx = idx;

	table->entries[idx] = child_table;
	++table->entry_count;
}

/* Forward declaration from below. */
static void __pvr_page_table_l1_destroy(struct pvr_page_table_l1 *table);

/**
 * pvr_page_table_l1_remove() - TODO
 * @table: Target L1 page table.
 * @idx: Index of the entry to remove.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is a valid entry at @idx in @table.
 */
static void
pvr_page_table_l1_remove(struct pvr_page_table_l1 *table, u16 idx)
{
	struct pvr_page_table_l0 *child_table = table->entries[idx];
	struct pvr_page_table_l1_entry_raw *entry_raw =
		pvr_page_table_l1_get_entry_raw(table, idx);

	pvr_page_table_l1_entry_raw_clear(entry_raw);

	child_table->parent = NULL;
	child_table->parent_idx = PVR_IDX_INVALID;

	table->entries[idx] = NULL;
	if (--table->entry_count == 0)
		__pvr_page_table_l1_destroy(table);
}

/**
 * pvr_page_table_l0_insert() - TODO
 * @table: Target L0 page table.
 * @idx: Index of the entry to write.
 * @dma_addr: Target DMA address to be referenced by the new entry.
 * @flags: Page options to be stored in the new entry.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is not already a valid entry at @idx in @table.
 */
static void
pvr_page_table_l0_insert(struct pvr_page_table_l0 *table, u16 idx,
			 dma_addr_t dma_addr, struct pvr_page_flags_raw flags)
{
	struct pvr_page_table_l0_entry_raw *entry_raw =
		pvr_page_table_l0_get_entry_raw(table, idx);

	pvr_page_table_l0_entry_raw_set(entry_raw, dma_addr, flags);

	/*
	 * There is no entry to set here - we don't keep a mirror of
	 * individual pages.
	 */

	++table->entry_count;
}

/* Forward declaration from below. */
static void __pvr_page_table_l0_destroy(struct pvr_page_table_l0 *table);

/**
 * pvr_page_table_l0_remove() - TODO
 * @table: Target L0 page table.
 * @idx: Index of the entry to remove.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is a valid entry at @idx in @table.
 */
static void
pvr_page_table_l0_remove(struct pvr_page_table_l0 *table, u16 idx)
{
	struct pvr_page_table_l0_entry_raw *entry_raw =
		pvr_page_table_l0_get_entry_raw(table, idx);

	pvr_page_table_l0_entry_raw_clear(entry_raw);

	/*
	 * There is no entry to clear here - we don't keep a mirror of
	 * individual pages.
	 */

	if (--table->entry_count == 0)
		__pvr_page_table_l0_destroy(table);
}

/**
 * DOC: Page table index utilities
 *
 * These utilities are not tied to the raw or mirror page tables since they
 * operate only on virtual device addresses which are identical between the two
 * structures.
 */
/**
 * DOC: Page table index utilities (constants)
 *
 * .. c:macro:: PVR_PAGE_TABLE_ADDR_SPACE_SIZE
 *
 *    Size of device-virtual address space which can be represented in the page
 *    table structure.
 *
 *    This value is checked at runtime against
 *    &pvr_device_features.virtual_address_space_bits by
 *    pvr_vm_create_context(), which will return an error if the feature value
 *    does not match this constant.
 *
 *    .. admonition:: Future work
 *
 *       It should be possible to support other values of
 *       &pvr_device_features.virtual_address_space_bits, but so far no
 *       hardware has been created which advertises an unsupported value.
 *
 * .. c:macro:: PVR_PAGE_TABLE_ADDR_BITS
 *
 *    Number of bits needed to represent any value less than
 *    %PVR_PAGE_TABLE_ADDR_SPACE_SIZE exactly.
 *
 * .. c:macro:: PVR_PAGE_TABLE_ADDR_MASK
 *
 *    Bitmask of virtual devices addresses which are valid in the page table
 *    structure.
 *
 *    This value is derived from %PVR_PAGE_TABLE_ADDR_SPACE_SIZE, so the same
 *    notes on that constant apply here.
 */

/**
 * PVR_PAGE_TABLE_ADDR_SPACE_SIZE - Size of device-virtual address space which
 * can be represented in the page table structure.
 *
 * *This doc comment is not rendered - see DOC: Page table index utilities
 * (constants).*
 */
#define PVR_PAGE_TABLE_ADDR_SPACE_SIZE SZ_1T

/**
 * PVR_PAGE_TABLE_ADDR_BITS - Number of bits needed to represent any value less
 * than %PVR_PAGE_TABLE_ADDR_SPACE_SIZE exactly.
 *
 * *This doc comment is not rendered - see DOC: Page table index utilities
 * (constants).*
 */
#define PVR_PAGE_TABLE_ADDR_BITS __ffs(PVR_PAGE_TABLE_ADDR_SPACE_SIZE)

/**
 * PVR_PAGE_TABLE_ADDR_MASK - Bitmask of virtual devices addresses which are
 * valid in the page table structure.
 *
 * *This doc comment is not rendered - see DOC: Page table index utilities
 * (constants).*
 */
#define PVR_PAGE_TABLE_ADDR_MASK (PVR_PAGE_TABLE_ADDR_SPACE_SIZE - 1)

/**
 * pvr_page_table_l2_idx() - Calculate the L2 page table index for a virtual
 *                           device address.
 * @device_addr: Target virtual device address.
 *
 * This function does not perform any bounds checking - it is the caller's
 * responsibility to ensure that @device_addr is valid before interpreting
 * the result.
 *
 * Return:
 *  * The index into an L2 page table corresponding to @device_addr.
 */
static __always_inline u16
pvr_page_table_l2_idx(u64 device_addr)
{
	return (device_addr & ~ROGUE_MMUCTRL_VADDR_PT_L2_INDEX_CLRMSK) >>
	       ROGUE_MMUCTRL_VADDR_PT_L2_INDEX_SHIFT;
}

/**
 * pvr_page_table_l1_idx() - Calculate the L1 page table index for a virtual
 *                           device address.
 * @device_addr: Target virtual device address.
 *
 * This function does not perform any bounds checking - it is the caller's
 * responsibility to ensure that @device_addr is valid before interpreting
 * the result.
 *
 * Return:
 *  * The index into an L1 page table corresponding to @device_addr.
 */
static __always_inline u16
pvr_page_table_l1_idx(u64 device_addr)
{
	return (device_addr & ~ROGUE_MMUCTRL_VADDR_PT_L1_INDEX_CLRMSK) >>
	       ROGUE_MMUCTRL_VADDR_PT_L1_INDEX_SHIFT;
}

/**
 * pvr_page_table_l0_idx() - Calculate the L0 page table index for a virtual
 *                           device address.
 * @device_addr: Target virtual device address.
 *
 * This function does not perform any bounds checking - it is the caller's
 * responsibility to ensure that @device_addr is valid before interpreting
 * the result.
 *
 * Return:
 *  * The index into an L0 page table corresponding to @device_addr.
 */
static __always_inline u16
pvr_page_table_l0_idx(u64 device_addr)
{
	return (device_addr & ~ROGUE_MMUCTRL_VADDR_PT_L0_INDEX_CLRMSK) >>
	       ROGUE_MMUCTRL_VADDR_PT_L0_INDEX_SHIFT;
}

/**
 * DOC: High-level page table operations
 *
 * TODO
 */

/**
 * pvr_page_table_l1_create_unchecked() - Create an L1 page table and insert
 *                                        it into an L2 page table.
 * @pvr_dev: Target PowerVR device.
 * @parent_table: L2 page table into which the created L1 page table will be
 * inserted.
 * @idx: Index into @parent_table at which to insert the created L1 page table.
 *
 * This function is unchecked. By using it, the caller is asserting that slot
 * @idx in @parent_table does not contain a valid entry.
 *
 * Return:
 *  * The newly-minted L1 page table on success,
 *  * -%ENOMEM if allocation of a &struct pvr_page_table_l1 fails, or
 *  * Any error returned by pvr_page_table_l1_init().
 */
static struct pvr_page_table_l1 *
pvr_page_table_l1_create_unchecked(struct pvr_device *pvr_dev,
				   struct pvr_page_table_l2 *parent_table,
				   u16 idx)
{
	struct pvr_page_table_l1 *table;
	int err;

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table) {
		err = -ENOMEM;
		goto err_out;
	}

	err = pvr_page_table_l1_init(table, pvr_dev);
	if (err)
		goto err_free_table;

	/* Store the new L1 page table in the parent L2 page table. */
	pvr_page_table_l2_insert(parent_table, idx, table);

	return table;

err_free_table:
	kfree(table);

err_out:
	return ERR_PTR(err);
}

/**
 * __pvr_page_table_l1_destroy() - Destroy an L1 page table after removing it
 *                                 from its parent L2 page table.
 * @table: Target L1 page table.
 *
 * Although this function is defined in the "High-level page table operations"
 * section for symmetry, it should never be called directly (hence the ``__``
 * prefix). Instead, it's called automatically when pvr_page_table_l1_remove()
 * causes @table to become empty.
 */
static void
__pvr_page_table_l1_destroy(struct pvr_page_table_l1 *table)
{
	/* Clear the parent L2 page table entry. */
	if (table->parent_idx != PVR_IDX_INVALID)
		pvr_page_table_l2_remove(table->parent, table->parent_idx);

	pvr_page_table_l1_fini(table);
	kfree(table);
}

/**
 * pvr_page_table_l1_get_or_create() - Retrieves (optionally creating if
 *                                     necessary) an L1 page table from the
 *                                     specified L2 page table entry.
 * @pvr_dev: [IN] Target PowerVR device.
 * @parent_table: [IN] L2 page table which contains the target L1 page table.
 * @idx: [IN] Index into @parent_table of the entry to fetch.
 * @should_create: [IN] Specifies whether new page tables should be created
 * when empty page table entries are encountered during traversal.
 * @did_create: [OUT] Optional pointer to a flag which is set when
 * @should_create is %true and new page table entries are created. In any
 * other case, the value will not be modified.
 *
 * Return:
 *  * The selected L1 page table on success,
 *  * -%ENXIO if @should_create is %false and an L1 page table would have been
 *    created, or
 *  * Any error returned by pvr_page_table_l1_create_unchecked() if
 *    @should_create is %true and a new L1 page table needs to be created.
 */
static struct pvr_page_table_l1 *
pvr_page_table_l1_get_or_create(struct pvr_device *pvr_dev,
				struct pvr_page_table_l2 *parent_table, u16 idx,
				bool should_create, bool *did_create)
{
	struct pvr_page_table_l1 *table;

	if (pvr_page_table_l2_entry_is_valid(parent_table, idx))
		return parent_table->entries[idx];

	if (!should_create)
		return ERR_PTR(-ENXIO);

	/*
	 * Use the unchecked version of pvr_page_table_l1_create() because we
	 * just verified the entry does not exist yet.
	 */
	table = pvr_page_table_l1_create_unchecked(pvr_dev, parent_table, idx);
	if (!IS_ERR(table) && did_create)
		*did_create = true;

	return table;
}

/**
 * pvr_page_table_l0_create_unchecked() - Create an L0 page table and insert
 *                                        it into an L1 page table.
 * @pvr_dev: Target PowerVR device.
 * @parent_table: L1 page table into which the created L0 page table will be
 * inserted.
 * @idx: Index into @parent_table at which to insert the created L0 page table.
 *
 * This function is unchecked. By using it, the caller is asserting that slot
 * @idx in @parent_table does not contain a valid entry.
 *
 * Return:
 *  * The newly-minted L0 page table on success,
 *  * -%ENOMEM if allocation of a &struct pvr_page_table_l0 fails, or
 *  * Any error returned by pvr_page_table_l0_init().
 */
static struct pvr_page_table_l0 *
pvr_page_table_l0_create_unchecked(struct pvr_device *pvr_dev,
				   struct pvr_page_table_l1 *parent_table,
				   u16 idx)
{
	struct pvr_page_table_l0 *table;
	int err;

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table) {
		err = -ENOMEM;
		goto err_out;
	}

	err = pvr_page_table_l0_init(table, pvr_dev);
	if (err)
		goto err_free_table;

	/* Store the new L0 page table in the parent L1 page table. */
	pvr_page_table_l1_insert(parent_table, idx, table);

	return table;

err_free_table:
	kfree(table);

err_out:
	return ERR_PTR(err);
}

/**
 * __pvr_page_table_l0_destroy() - Destroy an L0 page table after removing it
 *                                 from its parent L1 page table.
 * @table: Target L0 page table.
 *
 * Although this function is defined in the "High-level page table operations"
 * section for symmetry, it should never be called directly (hence the ``__``
 * prefix). Instead, it's called automatically when pvr_page_table_l0_remove()
 * causes @table to become empty.
 */
static void
__pvr_page_table_l0_destroy(struct pvr_page_table_l0 *table)
{
	/* Clear the parent L1 page table entry. */
	if (table->parent_idx != PVR_IDX_INVALID)
		pvr_page_table_l1_remove(table->parent, table->parent_idx);

	pvr_page_table_l0_fini(table);
	kfree(table);
}

/**
 * pvr_page_table_l0_get_or_create() - Retrieves (optionally creating if
 *                                     necessary) an L0 page table from the
 *                                     specified L1 page table entry.
 * @pvr_dev: [IN] Target PowerVR device.
 * @parent_table: [IN] L1 page table which contains the target L0 page table.
 * @idx: [IN] Index into @parent_table of the entry to fetch.
 * @should_create: [IN] Specifies whether new page tables should be created
 * when empty page table entries are encountered during traversal.
 * @did_create: [OUT] Optional pointer to a flag which is set when
 * @should_create is %true and new page table entries are created. In any
 * other case, the value will not be modified.
 *
 * Return:
 *  * The selected L0 page table on success,
 *  * -%ENXIO if @should_create is %false and an L0 page table would have been
 *    created, or
 *  * Any error returned by pvr_page_table_l1_create_unchecked() if
 *    @should_create is %true and a new L0 page table needs to be created.
 */
static struct pvr_page_table_l0 *
pvr_page_table_l0_get_or_create(struct pvr_device *pvr_dev,
				struct pvr_page_table_l1 *parent_table, u16 idx,
				bool should_create, bool *did_create)
{
	struct pvr_page_table_l0 *table;

	if (pvr_page_table_l1_entry_is_valid(parent_table, idx))
		return parent_table->entries[idx];

	if (!should_create)
		return ERR_PTR(-ENXIO);

	/*
	 * Use the unchecked version of pvr_page_table_l0_create() because we
	 * just verified the entry does not exist yet.
	 */
	table = pvr_page_table_l0_create_unchecked(pvr_dev, parent_table, idx);
	if (!IS_ERR(table) && did_create)
		*did_create = true;

	return table;
}

/**
 * DOC: Page table pointer
 *
 * Traversing the page table tree structure is not a straightforward
 * operation since there are multiple layers, each with different properties.
 * To contain and attempt to reduce this complexity, it's mostly encompassed
 * in a "heavy pointer" type (&struct pvr_page_table_ptr) and its associated
 * functions.
 *
 * Usage
 * -----
 * To start using a &struct pvr_page_table_ptr instance, you must first
 * initialize it to the starting address of your traversal using
 * pvr_page_table_ptr_init().
 *
 * TODO
 */

/**
 * struct pvr_page_table_ptr - TODO
 */
struct pvr_page_table_ptr {
	/**
	 * @pvr_dev: The PowerVR device associated with the VM context the
	 * pointer is traversing.
	 */
	struct pvr_device *pvr_dev;

	/**
	 * @l2_table: A cached handle to the L2 page table the pointer is
	 * currently traversing.
	 */
	struct pvr_page_table_l2 *l2_table;

	/**
	 * @l1_table: A cached handle to the L1 page table the pointer is
	 * currently traversing.
	 */
	struct pvr_page_table_l1 *l1_table;

	/**
	 * @l0_table: A cached handle to the L0 page table the pointer is
	 * currently traversing.
	 */
	struct pvr_page_table_l0 *l0_table;

	/**
	 * @l2_idx: Index into the L2 page table the pointer is currently
	 * referencing.
	 */
	u16 l2_idx;

	/**
	 * @l1_idx: Index into the L1 page table the pointer is currently
	 * referencing.
	 */
	u16 l1_idx;

	/**
	 * @l0_idx: Index into the L0 page table the pointer is currently
	 * referencing.
	 */
	u16 l0_idx;

	/**
	 * @sync_level_required: The maximum level of the page table tree
	 * structure which has (possibly) been modified since it was last
	 * flushed to the device.
	 *
	 * This field should only be set with pvr_page_table_ptr_require_sync()
	 * or indirectly by pvr_page_table_ptr_sync_partial().
	 */
	s8 sync_level_required;
};

static __always_inline void
pvr_page_table_ptr_require_sync(struct pvr_page_table_ptr *ptr, s8 level)
{
	if (ptr->sync_level_required < level)
		ptr->sync_level_required = level;
}

/**
 * pvr_page_table_ptr_sync_manual() - TODO
 * @ptr: Target page table pointer.
 * @level: TODO
 *
 * Do not call this function directly. Instead use
 * pvr_page_table_ptr_sync_partial() which is checked against the current
 * value of &ptr->sync_level_required.
 */
static void
pvr_page_table_ptr_sync_manual(struct pvr_page_table_ptr *ptr, s8 level)
{
	/*
	 * We sync the pages table levels in ascending order (starting from
	 * the leaf node) to ensure consistency.
	 */

	if (level < 0)
		return;

	pvr_page_table_l0_sync(ptr->l0_table);

	if (level < 1)
		return;

	pvr_page_table_l1_sync(ptr->l1_table);

	if (level < 2)
		return;

	pvr_page_table_l2_sync(ptr->l2_table);
}

/**
 * pvr_page_table_ptr_sync_partial() - TODO
 * @ptr: Target page table pointer.
 * @level: TODO
 */
static void
pvr_page_table_ptr_sync_partial(struct pvr_page_table_ptr *ptr, s8 level)
{
	/*
	 * If the requested sync level is greater than or equal to the
	 * currently required sync level, we do two things:
	 *  * Don't waste time syncing levels we haven't previously marked as
	 *    requiring a sync, and
	 *  * Reset the required sync level since we are about to sync
	 *    everything that was previously marked as requiring a sync.
	 */
	if (level >= ptr->sync_level_required) {
		level = ptr->sync_level_required;
		ptr->sync_level_required = -1;
	}

	pvr_page_table_ptr_sync_manual(ptr, level);
}

/**
 * pvr_page_table_ptr_sync() - TODO
 * @ptr: Target page table pointer.
 */
static __always_inline void
pvr_page_table_ptr_sync(struct pvr_page_table_ptr *ptr)
{
	pvr_page_table_ptr_sync_manual(ptr, ptr->sync_level_required);
}

/**
 * pvr_page_table_ptr_load_tables() - TODO
 * @ptr: Target page table pointer.
 * @should_create: Specifies whether new page tables should be created when
 * empty page table entries are encountered during traversal.
 * @load_level_required: TODO
 *
 * Return:
 *  * 0 on success, or
 *  * ...
 */
static int
pvr_page_table_ptr_load_tables(struct pvr_page_table_ptr *ptr,
			       bool should_create, s8 load_level_required)
{
	bool did_create_l1;
	bool did_create_l0;
	int err;

	/* Clear tables we're about to fetch in case of error states. */
	if (load_level_required >= 1)
		ptr->l1_table = NULL;

	if (load_level_required >= 0)
		ptr->l0_table = NULL;

	/* Get or create L1 page table. */
	if (load_level_required >= 1) {
		ptr->l1_table = pvr_page_table_l1_get_or_create(ptr->pvr_dev,
								ptr->l2_table,
								ptr->l2_idx,
								should_create,
								&did_create_l1);
		if (IS_ERR(ptr->l1_table)) {
			err = PTR_ERR(ptr->l1_table);
			ptr->l1_table = NULL;

			/*
			 * If @should_create is %false and no L1 page table was
			 * found, return early but without an error. Since
			 * pvr_page_table_l1_get_or_create() can only return
			 * -%ENXIO if @should_create is %false, there is no
			 * need to check it here.
			 */
			if (err == -ENXIO)
				err = 0;

			goto err_out;
		}
	}

	/* Get or create L0 page table. */
	if (load_level_required >= 0) {
		ptr->l0_table = pvr_page_table_l0_get_or_create(ptr->pvr_dev,
								ptr->l1_table,
								ptr->l1_idx,
								should_create,
								&did_create_l0);
		if (IS_ERR(ptr->l0_table)) {
			err = PTR_ERR(ptr->l0_table);
			ptr->l0_table = NULL;

			/*
			 * If @should_create is %false and no L0 page table was
			 * found, return early but without an error. Since
			 * pvr_page_table_l0_get_or_create() can only return
			 * -%ENXIO if @should_create is %false, there is no
			 * need to check it here.
			 */
			if (err == -ENXIO)
				err = 0;

			/*
			 * At this point, an L1 page table could have been
			 * created but is now empty due to the failed attempt
			 * at creating an L0 page table. In this instance, we
			 * must destroy the empty L1 page table ourselves as
			 * pvr_page_table_l1_remove() is never called as part
			 * of the error path in
			 * pvr_page_table_l0_get_or_create().
			 */
			if (did_create_l1)
				__pvr_page_table_l1_destroy(ptr->l1_table);

			goto err_out;
		}
	}

	if (did_create_l1)
		pvr_page_table_ptr_require_sync(ptr, 2);
	else if (did_create_l0)
		pvr_page_table_ptr_require_sync(ptr, 1);

	return 0;

err_out:
	return err;
}

/**
 * pvr_page_table_ptr_set() - TODO
 * @ptr: Target page table pointer.
 * @device_addr: New pointer target.
 * @should_create: Specify whether new page tables should be created when
 * empty page table entries are encountered during traversal.
 *
 * Return:
 *  * 0 on success, or
 *  * ...
 */
static int
pvr_page_table_ptr_set(struct pvr_page_table_ptr *ptr, u64 device_addr,
		       bool should_create)
{
	pvr_page_table_ptr_sync(ptr);

	ptr->l2_idx = pvr_page_table_l2_idx(device_addr);
	ptr->l1_idx = pvr_page_table_l1_idx(device_addr);
	ptr->l0_idx = pvr_page_table_l0_idx(device_addr);

	return pvr_page_table_ptr_load_tables(ptr, should_create, 1);
}

/**
 * pvr_page_table_ptr_init() - Initialize a page table pointer.
 * @ptr: Target page table pointer.
 * @pvr_dev: TODO
 * @root_table: TODO
 * @device_addr: TODO
 * @should_create: Specify whether new page tables should be created when
 * empty page table entries are encountered during traversal.
 *
 * If @should_create is %true, the L0 page table will be flagged for
 *
 * It is expected that @ptr be zeroed (e.g. from kzalloc()) before calling this
 * function. Since &struct pvr_page_table_ptr is small-ish (4-5 words), it is
 * intended to be allocated on the stack. By default, this is zeroed memory
 * so this warning is essentially just a formality.
 *
 * Return:
 *  * 0 on success, or
 *  * ...
 */
static int
pvr_page_table_ptr_init(struct pvr_page_table_ptr *ptr,
			struct pvr_device *pvr_dev,
			struct pvr_page_table_l2 *root_table, u64 device_addr,
			bool should_create)
{
	ptr->pvr_dev = pvr_dev;
	ptr->l2_table = root_table;
	ptr->sync_level_required = -1;

	return pvr_page_table_ptr_set(ptr, device_addr, should_create);
}

/**
 * pvr_page_table_ptr_fini() - Teardown a page table pointer.
 * @ptr: Target page table pointer.
 */
static void
pvr_page_table_ptr_fini(struct pvr_page_table_ptr *ptr)
{
	pvr_page_table_ptr_sync(ptr);
}

/**
 * pvr_page_table_ptr_next_page() - Advance a page table pointer.
 * @ptr: Target page table pointer.
 * @should_create: Specify whether new page tables should be created when
 * empty page table entries are encountered during traversal.
 *
 * If @should_create is %false, it is the callers responsibility to verify that
 * the state of the table references in @ptr is valid on return. If -%ENXIO is
 * returned, at least one of the table references is invalid. It should be
 * noted that @ptr as a whole will be left in a valid state if -%ENXIO is
 * returned, unlike other error codes. The caller should check which references
 * are invalid by comparing them to %NULL. Only &@ptr->l2_table is guaranteed
 * to be valid, since it represents the root of the page table tree structure.
 *
 * Return:
 *  * 0 on success,
 *  * -%EPERM if the operation would wrap at the top of the page table
 *    hierarchy,
 *  * -%ENXIO if @should_create is %false and a page table of any level would
 *    have otherwise been created, or
 *  * Any error returned while attempting to create missing page tables if
 *    @should_create is %true.
 */
static int
pvr_page_table_ptr_next_page(struct pvr_page_table_ptr *ptr, bool should_create)
{
	s8 load_level_required = -1;

	if (++ptr->l0_idx != ROGUE_MMUCTRL_ENTRIES_PT_L0_VALUE)
		goto load_tables;

	ptr->l0_idx = 0;
	load_level_required = 0;

	if (++ptr->l1_idx != ROGUE_MMUCTRL_ENTRIES_PT_L1_VALUE)
		goto load_tables;

	ptr->l1_idx = 0;
	load_level_required = 1;

	if (++ptr->l2_idx != ROGUE_MMUCTRL_ENTRIES_PT_L2_VALUE)
		goto load_tables;

	/*
	 * If the pattern continued, we would set &ptr->l2_idx to zero here.
	 * However, that would wrap the top layer of the page table hierarchy
	 * which is not a valid operation. Instead, we warn and return an
	 * error.
	 */
	WARN(true,
	     "%s(%p) attempted to loop the top of the page table hierarchy",
	     __func__, ptr);
	return -EPERM;

	/* If indices have wrapped, we need to load new tables. */
load_tables:
	/* First, flush tables which will be unloaded. */
	pvr_page_table_ptr_sync_partial(ptr, load_level_required);

	/* Then load tables from the required level down. */
	return pvr_page_table_ptr_load_tables(ptr, should_create,
					      load_level_required);
}

/**
 * pvr_page_table_ptr_copy() - TODO
 * @dst: TODO
 * @src: TODO
 */
static void
pvr_page_table_ptr_copy(struct pvr_page_table_ptr *dst,
			const struct pvr_page_table_ptr *src)
{
	memcpy(dst, src, sizeof(*dst));

	/*
	 * Nothing currently in the clone requires a sync later on, since the
	 * original will handle it either when advancing or during teardown.
	 */
	dst->sync_level_required = -1;
}

/**
 * DOC: Single page operations
 *
 * TODO
 */

/**
 * pvr_page_create() - Create a device virtual memory page and insert it into
 * an L0 page table.
 * @ptr: TODO
 * @dma_addr: DMA (quasi-physical) address which the created page should point
 * to.
 * @flags: Page options saved on the L0 page table entry for reading by the
 * device.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EEXIST if the requested page already exists.
 */
static int
pvr_page_create(struct pvr_page_table_ptr *ptr, dma_addr_t dma_addr,
		struct pvr_page_flags_raw flags)
{
	/* Do not create a new page if one already exists. */
	if (pvr_page_table_l0_entry_is_valid(ptr->l0_table, ptr->l0_idx))
		return -EEXIST;

	pvr_page_table_l0_insert(ptr->l0_table, ptr->l0_idx, dma_addr, flags);

	pvr_page_table_ptr_require_sync(ptr, 0);

	return 0;
}

/**
 * pvr_page_destroy() - Destroy a device page after removing it from its
 *                      parent L0 page table.
 * @ptr: TODO
 */
static void
pvr_page_destroy(struct pvr_page_table_ptr *ptr)
{
	/* Do nothing if the page does not exist. */
	if (!pvr_page_table_l0_entry_is_valid(ptr->l0_table, ptr->l0_idx))
		return;

	/* Clear the parent L0 page table entry. */
	pvr_page_table_l0_remove(ptr->l0_table, ptr->l0_idx);

	pvr_page_table_ptr_require_sync(ptr, 0);
}

/**
 * DOC: Interval tree base implementation
 *
 * There is a note in <linux/interval_tree_generic.h> which says:
 *
 *    Note - before using this, please consider if generic version
 *    (interval_tree.h) would work for you...
 *
 * Here, then, is our justification for using the generic version, instead
 * of the generic version (naming is hard, okay!):
 *
 * The generic version of &struct interval_tree_node (from
 * <linux/interval_tree.h> uses unsigned long. We always need the elements to
 * be 64 bits wide, regardless of host pointer size. We could gate this
 * implementation on %BITS_PER_LONG, but it's better for us to store &start
 * and &size then derive &last rather than the way &struct interval_tree_node
 * does it, storing &start and &last] then deriving &size.
 */

/**
 * pvr_vm_interval_tree_compute_last() - Compute the inclusive last value of
 *                                       a range.
 * @start: Start of range.
 * @size: Size of range.
 *
 * This seemingly useless function was added to attempt to eliminate some
 * off-by-one errors which managed to creep in before it existed.
 *
 * Return:
 *  * The last (inclusive) value of the range specified by @start and @size.
 */
static __always_inline u64
pvr_vm_interval_tree_compute_last(u64 start, u64 size)
{
	if (!size)
		return start;

	return start + size - 1;
}

/**
 * struct pvr_vm_interval_tree_node - A node in our implementation of an
 *                                    interval tree.
 *
 * Unlike the generic implementation in <linux/interval_tree.h>, we store the
 * size of the interval instead of the last value. To access the last value (as
 * required by the implementation behind INTERVAL_TREE_DEFINE()) use
 * pvr_vm_internal_tree_node_last().
 */
struct pvr_vm_interval_tree_node {
	/** @rb: Base RB-tree node. **For internal use only.** */
	struct rb_node rb;

	/**
	 * @start: The start value of the range represented by this node.
	 * **For internal use only.**
	 *
	 * Do not access this member directly, instead call
	 * pvr_vm_interval_tree_node_start().
	 */
	u64 start;

	/**
	 * @size: The size of the range represented by this node.
	 * **For internal use only.**
	 *
	 * Do not access this member directly, instead call
	 * pvr_vm_interval_tree_node_size().
	 */
	u64 size;

	/**
	 * @__subtree_last: Required for the implementation generated by
	 * INTERVAL_TREE_DEFINE(). **For internal use only.**
	 */
	u64 __subtree_last;
};

static __always_inline bool
pvr_vm_interval_tree_node_is_inserted(struct pvr_vm_interval_tree_node *node)
{
	return !RB_EMPTY_NODE(&node->rb);
}

static __always_inline void
pvr_vm_interval_tree_node_mark_removed(struct pvr_vm_interval_tree_node *node)
{
	RB_CLEAR_NODE(&node->rb);
}

static __always_inline void
pvr_vm_interval_tree_node_init(struct pvr_vm_interval_tree_node *node,
			       u64 start, u64 size)
{
	pvr_vm_interval_tree_node_mark_removed(node);

	node->start = start;
	node->size = size;
}

static __always_inline void
pvr_vm_interval_tree_node_fini(struct pvr_vm_interval_tree_node *node)
{
	WARN(pvr_vm_interval_tree_node_is_inserted(node),
	     "%s(%p) called before removing node from tree", __func__, node);
}

/**
 * pvr_vm_interval_tree_node_start() - TODO
 * @node: Target VM interval tree node.
 *
 * Return:
 *  * The start value of @node.
 */
static __always_inline u64
pvr_vm_interval_tree_node_start(struct pvr_vm_interval_tree_node *node)
{
	return node->start;
}

/**
 * pvr_vm_interval_tree_node_size() - TODO
 * @node: Target VM interval tree node.
 *
 * Return:
 *  * The size of @node.
 */
static __always_inline u64
pvr_vm_interval_tree_node_size(struct pvr_vm_interval_tree_node *node)
{
	return node->size;
}

/**
 * pvr_vm_interval_tree_node_last() - TODO
 * @node: Target VM interval tree node.
 *
 * Return:
 *  * The last (inclusive) value of @node.
 */
static __always_inline u64
pvr_vm_interval_tree_node_last(struct pvr_vm_interval_tree_node *node)
{
	return pvr_vm_interval_tree_compute_last(node->start, node->size);
}

INTERVAL_TREE_DEFINE(struct pvr_vm_interval_tree_node, rb, u64, __subtree_last,
		     pvr_vm_interval_tree_node_start,
		     pvr_vm_interval_tree_node_last, static,
		     __pvr_vm_interval_tree)

/**
 * for_each_pvr_vm_interval_tree_node() - Helper macro to iterate a specific
 *                                        range of an interval tree.
 * @tree_:
 * @node_:
 * @start_:
 * @size_:
 *
 * Due to the way interval tree iteration works, formulating a ``for`` loop
 * around it is pretty verbose! We can encapsulate all that lengthiness in a
 * single macro (so we did).
 */
#define for_each_pvr_vm_interval_tree_node(tree_, node_, start_, size_)   \
	for ((node_) = pvr_vm_interval_tree_iter_first((tree_), (start_), \
						       (size_));          \
	     (node_);                                                     \
	     (node_) = pvr_vm_interval_tree_iter_next((node_), (start_),  \
						      (size_)))

/**
 * struct pvr_vm_interval_tree - TODO
 */
struct pvr_vm_interval_tree {
	/** @root: TODO */
	struct rb_root_cached root;
} __packed;

/**
 * pvr_vm_interval_tree_insert() - TODO
 * @tree: Target VM interval tree.
 * @node: Node to be inserted.
 */
static __always_inline void
pvr_vm_interval_tree_insert(struct pvr_vm_interval_tree *tree,
			    struct pvr_vm_interval_tree_node *node)
{
	WARN(pvr_vm_interval_tree_node_is_inserted(node),
	     "%s(%p,%p) called on a node which is already in a tree", __func__,
	     tree, node);

	/* This function is generated by INTERVAL_TREE_DEFINE(). */
	__pvr_vm_interval_tree_insert(node, &tree->root);
}

/**
 * pvr_vm_interval_tree_remove() - TODO
 * @tree: Target VM interval tree.
 * @node: Node to be removed.
 */
static __always_inline void
pvr_vm_interval_tree_remove(struct pvr_vm_interval_tree *tree,
			    struct pvr_vm_interval_tree_node *node)
{
	WARN(!pvr_vm_interval_tree_node_is_inserted(node),
	     "%s(%p,%p) called on a node which is not in a tree", __func__,
	     tree, node);

	/* This function is generated by INTERVAL_TREE_DEFINE(). */
	__pvr_vm_interval_tree_remove(node, &tree->root);

	pvr_vm_interval_tree_node_mark_removed(node);
}

/**
 * pvr_vm_interval_tree_subtree_search() - TODO
 * @node: Node to search from.
 * @start: Start value of the iterable range.
 * @size: Size of the iterable range.
 */
static __always_inline struct pvr_vm_interval_tree_node *
pvr_vm_interval_tree_subtree_search(struct pvr_vm_interval_tree_node *node,
				    u64 start, u64 size)
{
	/* This function is generated by INTERVAL_TREE_DEFINE(). */
	return __pvr_vm_interval_tree_subtree_search(
		node, start, pvr_vm_interval_tree_compute_last(start, size));
}

/**
 * pvr_vm_interval_tree_iter_first() - TODO
 * @tree: Target VM interval tree.
 * @start: Start value of the iterable range.
 * @size: Size of the iterable range.
 *
 * *This function is generated by INTERVAL_TREE_DEFINE().*
 */
static __always_inline struct pvr_vm_interval_tree_node *
pvr_vm_interval_tree_iter_first(struct pvr_vm_interval_tree *tree, u64 start,
				u64 size)
{
	u64 last = pvr_vm_interval_tree_compute_last(start, size);

	/* This function is generated by INTERVAL_TREE_DEFINE(). */
	return __pvr_vm_interval_tree_iter_first(&tree->root, start, last);
}

/**
 * pvr_vm_interval_tree_iter_next() - TODO
 * @node: Node to iterate from.
 * @start: Start value of the iterable range.
 * @size: Size of the iterable range.
 */
static __always_inline struct pvr_vm_interval_tree_node *
pvr_vm_interval_tree_iter_next(struct pvr_vm_interval_tree_node *node,
			       u64 start, u64 size)
{
	u64 last = pvr_vm_interval_tree_compute_last(start, size);

	/* This function is generated by INTERVAL_TREE_DEFINE(). */
	return __pvr_vm_interval_tree_iter_next(node, start, last);
}

/**
 * pvr_vm_interval_tree_init() - Initialize an interval tree.
 * @tree: Target VM interval tree.
 */
static __always_inline void
pvr_vm_interval_tree_init(struct pvr_vm_interval_tree *tree)
{
	tree->root = RB_ROOT_CACHED;
}

/**
 * pvr_vm_interval_tree_fini() - Teardown an interval tree.
 * @tree: Target VM interval tree.
 *
 * It is an error to call this function on a non-empty interval tree. Doing
 * so is very likely to cause a memory leak. For this reason,
 * pvr_vm_interval_tree_fini() will emit a kernel warning for each entry found
 * in the target tree before returning.
 */
static void
pvr_vm_interval_tree_fini(struct pvr_vm_interval_tree *tree)
{
	struct pvr_vm_interval_tree_node *node;

	/* clang-format off */
	for_each_pvr_vm_interval_tree_node(tree, node, 0, U64_MAX) {
		WARN(true, "%s(%p) found [%llx,%llx]@%p", __func__, tree,
		     pvr_vm_interval_tree_node_start(node),
		     pvr_vm_interval_tree_node_last(node), node);
	}
	/* clang-format on */
}

/**
 * pvr_vm_interval_tree_contains() - Check if any node in an interval tree
 *                                   overlaps with a specified range.
 * @tree: Target VM interval tree.
 * @start: Start value of the search range.
 * @size: Size of the search range.
 *
 * This function is just a call to pvr_vm_interval_tree_iter_first() with the
 * returned pointer coerced into a ``bool`` for convenience. It should always
 * be inlined.
 *
 * Return:
 *  * %true if any node in the target interval tree overlaps with the range
 *    specified by @start and @size, or
 *  * %false otherwise.
 */
static __always_inline bool
pvr_vm_interval_tree_contains(struct pvr_vm_interval_tree *tree, u64 start,
			      u64 size)
{
	return pvr_vm_interval_tree_iter_first(tree, start, size);
}

/**
 * pvr_vm_interval_tree_find() - Find a node which completely contains a range.
 * @tree: Target VM interval tree.
 * @start: Start value of the required range.
 * @size: Size of the required range.
 *
 * Return:
 *  * A node containing the entire range specified by @start and @size, or
 *  * %NULL if such a node does not currently exist in @tree.
 */
static struct pvr_vm_interval_tree_node *
pvr_vm_interval_tree_find(struct pvr_vm_interval_tree *tree, u64 start,
			  u64 size)
{
	/*
	 * We only search for the start of the range here, because the search
	 * implementation for interval_tree only looks for intersections. After
	 * a node is found that contains the start value, we can check that it
	 * also contains the last value.
	 */
	struct pvr_vm_interval_tree_node *node =
		pvr_vm_interval_tree_iter_first(tree, start, 0);

	/*
	 * Check that a node exists around the requested range and that it
	 * encompasses the entire range.
	 */
	if (!node || pvr_vm_interval_tree_compute_last(start, size) >
			     pvr_vm_interval_tree_node_last(node))
		return NULL;

	return node;
}

/**
 * pvr_vm_interval_tree_get() - Fetch a node by its exact start value.
 * @tree: Target VM interval tree.
 * @start: Start value of the required node.
 *
 * Return:
 *  * A node whose start address is exactly @start, or
 *  * %NULL if such a node does not currently exist in @tree.
 */
static struct pvr_vm_interval_tree_node *
pvr_vm_interval_tree_get(struct pvr_vm_interval_tree *tree, u64 start)
{
	struct pvr_vm_interval_tree_node *node =
		pvr_vm_interval_tree_iter_first(tree, start, 0);

	if (!node || pvr_vm_interval_tree_node_start(node) != start)
		return NULL;

	return node;
}

/**
 * DOC: Type-save VM interval tree wrappers
 *
 * TODO
 */

/**
 * PVR_GEN_INTERVAL_TREE_WRAPPER_TYPE_AND_DECLS() - TODO
 * @node_wrapper_: Type name of the structure to use as a node.
 *
 * TODO
 *
 * This is the first half of a two-part implementation. The second half can be
 * found in PVR_GEN_INTERVAL_TREE_WRAPPER_DEFS().
 */
#define PVR_GEN_INTERVAL_TREE_WRAPPER_TYPE_AND_DECLS(node_wrapper_)             \
	struct node_wrapper_##_tree {                                           \
		struct pvr_vm_interval_tree tree;                               \
	} __packed;                                                             \
                                                                                \
	struct node_wrapper_;                                                   \
                                                                                \
	static __always_inline void node_wrapper_##_tree_init(                  \
		struct node_wrapper_##_tree *tree);                             \
                                                                                \
	static __always_inline void node_wrapper_##_tree_fini(                  \
		struct node_wrapper_##_tree *tree);                             \
                                                                                \
	static __always_inline u64 node_wrapper_##_start(                       \
		struct node_wrapper_ *node);                                    \
                                                                                \
	static __always_inline u64 node_wrapper_##_last(                        \
		struct node_wrapper_ *node);                                    \
                                                                                \
	static __always_inline void node_wrapper_##_tree_insert(                \
		struct node_wrapper_##_tree *tree,                              \
		struct node_wrapper_ *node);                                    \
                                                                                \
	static __always_inline void node_wrapper_##_tree_remove(                \
		struct node_wrapper_##_tree *tree,                              \
		struct node_wrapper_ *node);                                    \
                                                                                \
	static __always_inline struct node_wrapper_                             \
		*node_wrapper_##_tree_iter_first(                               \
			struct node_wrapper_##_tree *tree, u64 start,           \
			u64 size);                                              \
                                                                                \
	static __always_inline struct node_wrapper_                             \
		*node_wrapper_##_tree_iter_next(struct node_wrapper_ *node,     \
						u64 start, u64 size);           \
                                                                                \
	static __always_inline bool node_wrapper_##_tree_contains(              \
		struct node_wrapper_##_tree *tree, u64 start, u64 size);        \
                                                                                \
	static __always_inline struct node_wrapper_ *node_wrapper_##_tree_find( \
		struct node_wrapper_##_tree *tree, u64 start, u64 size);        \
                                                                                \
	static __always_inline struct node_wrapper_ *node_wrapper_##_tree_get(  \
		struct node_wrapper_##_tree *tree, u64 start);

/**
 * PVR_GEN_INTERVAL_TREE_WRAPPER_DEFS() - TODO
 * @node_wrapper_: Type name of the structure to use as a node.
 * @node_wrapper_member_: Member name of the base
 * &struct pvr_vm_interval_tree_node in the structure specified by
 * @node_wrapper_.
 *
 * TODO
 *
 * This is the second half of a two-part implementation. The first half can be
 * found in PVR_GEN_INTERVAL_TREE_WRAPPER_TYPE_AND_DECLS().
 */
#define PVR_GEN_INTERVAL_TREE_WRAPPER_DEFS(node_wrapper_,                       \
					   node_wrapper_member_)                \
	static __always_inline void node_wrapper_##_tree_init(                  \
		struct node_wrapper_##_tree *tree)                              \
	{                                                                       \
		pvr_vm_interval_tree_init(&tree->tree);                         \
	}                                                                       \
                                                                                \
	static __always_inline void node_wrapper_##_tree_fini(                  \
		struct node_wrapper_##_tree *tree)                              \
	{                                                                       \
		pvr_vm_interval_tree_fini(&tree->tree);                         \
	}                                                                       \
                                                                                \
	static __always_inline u64 node_wrapper_##_start(                       \
		struct node_wrapper_ *node)                                     \
	{                                                                       \
		return pvr_vm_interval_tree_node_start(                         \
			&node->node_wrapper_member_);                           \
	}                                                                       \
                                                                                \
	static __always_inline u64 node_wrapper_##_size(                        \
		struct node_wrapper_ *node)                                     \
	{                                                                       \
		return pvr_vm_interval_tree_node_size(                          \
			&node->node_wrapper_member_);                           \
	}                                                                       \
                                                                                \
	static __always_inline u64 node_wrapper_##_last(                        \
		struct node_wrapper_ *node)                                     \
	{                                                                       \
		return pvr_vm_interval_tree_node_last(                          \
			&node->node_wrapper_member_);                           \
	}                                                                       \
                                                                                \
	static __always_inline struct node_wrapper_ *node_wrapper_##_from_node( \
		struct pvr_vm_interval_tree_node *node)                         \
	{                                                                       \
		return container_of(node, struct node_wrapper_,                 \
				    node_wrapper_member_);                      \
	}                                                                       \
                                                                                \
	static __always_inline void node_wrapper_##_tree_insert(                \
		struct node_wrapper_##_tree *tree, struct node_wrapper_ *node)  \
	{                                                                       \
		pvr_vm_interval_tree_insert(&tree->tree,                        \
					    &node->node_wrapper_member_);       \
	}                                                                       \
                                                                                \
	static __always_inline void node_wrapper_##_tree_remove(                \
		struct node_wrapper_##_tree *tree, struct node_wrapper_ *node)  \
	{                                                                       \
		pvr_vm_interval_tree_remove(&tree->tree,                        \
					    &node->node_wrapper_member_);       \
	}                                                                       \
                                                                                \
	static __always_inline struct node_wrapper_                             \
		*node_wrapper_##_tree_iter_first(                               \
			struct node_wrapper_##_tree *tree, u64 start,           \
			u64 size)                                               \
	{                                                                       \
		return node_wrapper_##_from_node(                               \
			pvr_vm_interval_tree_iter_first(&tree->tree, start,     \
							size));                 \
	}                                                                       \
                                                                                \
	static __always_inline struct node_wrapper_                             \
		*node_wrapper_##_tree_iter_next(struct node_wrapper_ *node,     \
						u64 start, u64 size)            \
	{                                                                       \
		return node_wrapper_##_from_node(                               \
			pvr_vm_interval_tree_iter_next(                         \
				&node->node_wrapper_member_, start, size));     \
	}                                                                       \
                                                                                \
	static __always_inline bool node_wrapper_##_tree_contains(              \
		struct node_wrapper_##_tree *tree, u64 start, u64 size)         \
	{                                                                       \
		return pvr_vm_interval_tree_contains(&tree->tree, start,        \
						     size);                     \
	}                                                                       \
                                                                                \
	static __always_inline struct node_wrapper_ *node_wrapper_##_tree_find( \
		struct node_wrapper_##_tree *tree, u64 start, u64 size)         \
	{                                                                       \
		return node_wrapper_##_from_node(                               \
			pvr_vm_interval_tree_find(&tree->tree, start, size));   \
	}                                                                       \
                                                                                \
	static __always_inline struct node_wrapper_ *node_wrapper_##_tree_get(  \
		struct node_wrapper_##_tree *tree, u64 start)                   \
	{                                                                       \
		return node_wrapper_##_from_node(                               \
			pvr_vm_interval_tree_get(&tree->tree, start));          \
	}

PVR_GEN_INTERVAL_TREE_WRAPPER_TYPE_AND_DECLS(pvr_vm_mapping);

/**
 * DOC: Memory context
 *
 * TODO
 */

/**
 * struct pvr_vm_context - TODO
 */
struct pvr_vm_context {
	/**
	 * @pvr_dev: TODO
	 */
	struct pvr_device *pvr_dev;

	/** @root_table: The root of the page table tree structure.*/
	struct pvr_page_table_l2 root_table;

	/**
	 * @mappings: TODO
	 */
	struct pvr_vm_mapping_tree mappings;

	/**
	 * @lock: Global lock on this entire structure of page tables.
	 *
	 * .. admonition:: Future work
	 *
	 *    This "global" lock is potentially not the most efficient method
	 *    of protecting the entire page table tree structure. Investigation
	 *    into more granular methods would be a good idea.
	 */
	struct mutex lock;
};

dma_addr_t pvr_vm_get_page_table_root_addr(struct pvr_vm_context *vm_ctx)
{
	return vm_ctx->root_table.backing_page.dma_addr;
}

/**
 * pvr_vm_context_init() - TODO
 * @vm_ctx: Target VM context.
 * @pvr_dev: Target PowerVR device.
 */
static int
pvr_vm_context_init(struct pvr_vm_context *vm_ctx, struct pvr_device *pvr_dev)
{
	int err;

	err = pvr_page_table_l2_init(&vm_ctx->root_table, pvr_dev);
	if (err)
		goto err_out;

	pvr_vm_mapping_tree_init(&vm_ctx->mappings);

	mutex_init(&vm_ctx->lock);

	vm_ctx->pvr_dev = pvr_dev;

	return 0;

err_out:
	return err;
}

/**
 * pvr_vm_context_fini() - TODO
 * @vm_ctx: Target VM context.
 * @enable_warnings: Whether to print warnings for remaining mappings.
 */
static void
pvr_vm_context_fini(struct pvr_vm_context *vm_ctx, bool enable_warnings)
{
	struct pvr_vm_mapping *mapping;

	/* Destroy any remaining mappings. */
	mutex_lock(&vm_ctx->lock);

	while ((mapping = pvr_vm_mapping_tree_iter_first(&vm_ctx->mappings, 0, U64_MAX)) != NULL) {
		WARN(enable_warnings, "%s(%p) found [%llx,%llx]@%p", __func__, &vm_ctx->mappings,
		     pvr_vm_mapping_start(mapping), pvr_vm_mapping_last(mapping), mapping);
		pvr_vm_mapping_unmap(vm_ctx, mapping);
		pvr_vm_mapping_fini(mapping);
		kfree(mapping);
	}

	mutex_unlock(&vm_ctx->lock);

	mutex_destroy(&vm_ctx->lock);
	pvr_vm_mapping_tree_fini(&vm_ctx->mappings);
	pvr_page_table_l2_fini(&vm_ctx->root_table);
}

static int
pvr_vm_context_unmap_from_ptr(struct pvr_page_table_ptr *ptr, u64 nr_pages)
{
	u64 page;
	int err;

	if (nr_pages == 0)
		return 0;

	/*
	 * Destroy first page outside loop, as it doesn't require a pointer
	 * increment beforehand. If the L0 page table reference in @ptr is
	 * %NULL, there cannot be a mapped page at @ptr (so skip ahead).
	 */
	if (ptr->l0_table)
		pvr_page_destroy(ptr);

	for (page = 1; page < nr_pages; ++page) {
		err = pvr_page_table_ptr_next_page(ptr, false);
		/*
		 * If the page table tree structure at @ptr is incomplete,
		 * skip ahead. We don't care about unmapping pages that
		 * cannot exist.
		 *
		 * FIXME: This could be made more efficient by jumping ahead
		 * using pvr_page_table_ptr_set().
		 */
		if (err == -ENXIO)
			continue;

		if (err)
			goto err_out;

		pvr_page_destroy(ptr);
	}

	return 0;

err_out:
	return err;
}

/**
 * pvr_vm_context_unmap() - TODO
 * @vm_ctx: Target memory context.
 * @device_addr: First virtual device address to unmap.
 * @nr_pages: Number of pages to unmap.
 *
 * The total amount of virtual memory unmapped by pvr_vm_context_unmap()
 * is @nr_pages * 4KiB.
 */
static int
pvr_vm_context_unmap(struct pvr_vm_context *vm_ctx, u64 device_addr,
		     u64 nr_pages)
{
	struct pvr_page_table_ptr ptr;
	int err;

	err = pvr_page_table_ptr_init(&ptr, vm_ctx->pvr_dev,
				      &vm_ctx->root_table, device_addr, false);
	if (err && err != -ENXIO)
		goto err_out;

	err = pvr_vm_context_unmap_from_ptr(&ptr, nr_pages);
	if (err)
		goto err_ptr_fini;

	err = 0;
	goto err_ptr_fini;

err_ptr_fini:
	pvr_page_table_ptr_fini(&ptr);

err_out:
	return err;
}

static int
pvr_vm_context_map_direct(struct pvr_vm_context *vm_ctx, dma_addr_t dma_addr,
			  u64 size, struct pvr_page_table_ptr *ptr,
			  struct pvr_page_flags_raw flags)
{
	unsigned int pages = size >> PVR_DEVICE_PAGE_SHIFT;
	unsigned int page;

	struct pvr_page_table_ptr ptr_copy;

	int err;

	/*
	 * Before progressing, save a copy of the start pointer so we can use
	 * it again if we enter an error state and have to destroy pages.
	 */
	pvr_page_table_ptr_copy(&ptr_copy, ptr);

	/*
	 * Create first page outside loop, as it doesn't require a pointer
	 * increment beforehand.
	 */
	err = pvr_page_create(ptr, dma_addr, flags);
	if (err)
		goto err_fini_ptr_copy;

	for (page = 1; page < pages; ++page) {
		err = pvr_page_table_ptr_next_page(ptr, true);
		if (err)
			goto err_destroy_pages;

		dma_addr += PVR_DEVICE_PAGE_SIZE;

		err = pvr_page_create(ptr, dma_addr, flags);
		if (err)
			goto err_destroy_pages;
	}

	err = 0;
	goto err_fini_ptr_copy;

err_destroy_pages:
	err = pvr_vm_context_unmap_from_ptr(&ptr_copy, page);

err_fini_ptr_copy:
	pvr_page_table_ptr_fini(&ptr_copy);

	return err;
}

/**
 * pvr_vm_context_map_partial_sgl() - Map part of a scatter-gather table entry
 *                                    to virtual device memory.
 * @vm_ctx: Target VM context.
 * @sgl: Target scatter-gather table entry.
 * @offset: Offset into @sgl to map from. Must result in a starting address
 * from @sgl which is CPU page-aligned.
 * @size: Size of the memory to be mapped in bytes. Must be a non-zero multiple
 * of the device page size.
 * @ptr: Page table pointer which points to the first page that should be
 * mapped to. This will point to the last page mapped to on return.
 * @page_flags: Page options to be applied to every virtual device memory page
 * in the created mapping.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EINVAL if the range specified by @offset and @size is not completely
 *    within @sgl.
 */
static int
pvr_vm_context_map_partial_sgl(struct pvr_vm_context *vm_ctx,
			       struct scatterlist *sgl, u64 offset, u64 size,
			       struct pvr_page_table_ptr *ptr,
			       struct pvr_page_flags_raw page_flags)
{
	dma_addr_t dma_addr = sg_dma_address(sgl);
	unsigned int dma_len = sg_dma_len(sgl);

	if (offset + size > dma_len || offset > dma_len)
		return -EINVAL;

	return pvr_vm_context_map_direct(vm_ctx, dma_addr + offset, size, ptr,
					 page_flags);
}

static int
pvr_vm_context_map_sgl(struct pvr_vm_context *vm_ctx, struct scatterlist *sgl,
		       struct pvr_page_table_ptr *ptr,
		       struct pvr_page_flags_raw page_flags)
{
	dma_addr_t dma_addr = sg_dma_address(sgl);
	unsigned int dma_len = sg_dma_len(sgl);

	return pvr_vm_context_map_direct(vm_ctx, dma_addr, dma_len, ptr,
					 page_flags);
}

/**
 * pvr_vm_context_map_sgt() - Map an entire scatter-gather table into virtual
 *                            device memory.
 * @vm_ctx: Target VM context.
 * @sgt: Target scatter-gather table.
 * @device_addr: Virtual device address to map to. Must be device page-aligned.
 * @page_flags: Page options to be applied to every virtual device memory page
 * in the created mapping.
 *
 * Return:
 *  * 0 on success, or
 *  * ...
 */
static int
pvr_vm_context_map_sgt(struct pvr_vm_context *vm_ctx, struct sg_table *sgt,
		       u64 device_addr, struct pvr_page_flags_raw page_flags)
{
	struct pvr_page_table_ptr ptr;
	struct pvr_page_table_ptr ptr_copy;

	struct scatterlist *sgl;
	unsigned int sgt_idx;

	u64 accumulated_size = 0;
	u64 created_size;

	int err;

	/*
	 * Ensure that every sg table entry has a DMA address and length that
	 * is a multiple of the device page size.
	 */
	/* clang-format off */
	for_each_sgtable_dma_sg(sgt, sgl, sgt_idx) {
		accumulated_size += sg_dma_len(sgl);

		if (sg_dma_address(sgl) & ~PVR_DEVICE_PAGE_MASK ||
		    sg_dma_len(sgl) & ~PVR_DEVICE_PAGE_MASK) {
			err = -EINVAL;
			goto err_out;
		}
	}
	/* clang-format on */

	err = pvr_page_table_ptr_init(&ptr, vm_ctx->pvr_dev,
				      &vm_ctx->root_table, device_addr, true);
	if (err) {
		err = -EINVAL;
		goto err_out;
	}

	/*
	 * Before progressing, save a copy of the start pointer so we can use
	 * it again if we enter an error state and have to destroy pages.
	 */
	pvr_page_table_ptr_copy(&ptr_copy, &ptr);

	/*
	 * Map first sg table entry outside loop, as it doesn't require a
	 * pointer increment beforehand. We know &sgl is valid here because an
	 * sg table must contain at least one entry.
	 */
	sgl = sgt->sgl;
	err = pvr_vm_context_map_sgl(vm_ctx, sgl, &ptr, page_flags);
	if (err)
		goto err_fini_ptr;

	created_size = sg_dma_len(sgl);

	while ((sgl = sg_next(sgl))) {
		err = pvr_page_table_ptr_next_page(&ptr, true);
		if (err) {
			err = -EINVAL;
			goto err_unmap;
		}

		err = pvr_vm_context_map_sgl(vm_ctx, sgl, &ptr, page_flags);
		if (err)
			goto err_unmap;

		created_size += sg_dma_len(sgl);
	}

	err = 0;
	goto err_fini_ptr_and_ptr_copy;

err_unmap:
	pvr_vm_context_unmap_from_ptr(&ptr_copy,
				      created_size >> PVR_DEVICE_PAGE_SHIFT);

err_fini_ptr_and_ptr_copy:
	pvr_page_table_ptr_fini(&ptr_copy);

err_fini_ptr:
	pvr_page_table_ptr_fini(&ptr);

err_out:
	return err;
}

/**
 * pvr_vm_context_map_partial_sgt() - Map part of a scatter-gather table into
 *                                    virtual device memory.
 * @vm_ctx: Target VM context.
 * @sgt: Target scatter-gather table.
 * @sgt_offset: Offset into @sgt to map from. Must result in a starting
 * address from @sgt which is CPU page-aligned.
 * @device_addr: Virtual device address to map to. Must be device page-aligned.
 * @size: Size of memory to be mapped in bytes. Must be a non-zero multiple
 * of the device page size.
 * @page_flags: Page options to be applied to every virtual device memory page
 * in the created mapping.
 *
 * Return:
 *  * 0 on success, or
 *  * ...
 */
static int
pvr_vm_context_map_partial_sgt(struct pvr_vm_context *vm_ctx,
			       struct sg_table *sgt, u64 sgt_offset,
			       u64 device_addr, u64 size,
			       struct pvr_page_flags_raw page_flags)
{
	struct pvr_page_table_ptr ptr;
	struct pvr_page_table_ptr ptr_copy;

	struct scatterlist *sgl;

	struct scatterlist *first_sgl;
	struct scatterlist *last_sgl;

	/*
	 * For these three (four) values:
	 *  * "offset" refers to the position in the given sgl to start mapping
	 *    from, and
	 *  * "size" refers to the amount of that sgl to map.
	 *
	 * For &first_sgl, "size" is the distance between "offset" and the
	 * total size of the sgl.
	 *
	 * For &last_sgl, "offset" is always zero because it is contiguous with
	 * the previous sgl. The only case it would be non-zero is when the
	 * first and last sgls are the same, but this case is handled
	 * specially.
	 */
	u64 first_sgl_offset;
	/* There is no last_sgl_offset (see above). */
	u64 first_sgl_size;
	u64 last_sgl_size;

	u64 accumulated_size = 0;
	u64 created_size;

	int err;

	/* @sgt must contain at least one entry. */
	if (!sgt->sgl) {
		err = -EINVAL;
		goto err_out;
	}

	/*
	 * First, skip through the sg table until we hit an entry which
	 * contains sgt_offset.
	 */
	sgl = sgt->sgl;
	do {
		accumulated_size += sg_dma_len(sgl);

		if (accumulated_size > sgt_offset)
			goto found_first_sgl;
	} while ((sgl = sg_next(sgl)) != NULL);

	/*
	 * If we fall out of the loop above, we've reached the end of @sgt
	 * without finding the start of the requested range.
	 */
	err = -EINVAL;
	goto err_out;

found_first_sgl:
	/* Record the entry discovered in the loop above. */
	first_sgl = sgl;
	first_sgl_size = accumulated_size - sgt_offset;
	first_sgl_offset = sg_dma_len(first_sgl) - first_sgl_size;

	/*
	 * Ensure that sgt_offset is within the bounds of the sg table; that
	 * the DMA address given by the offset into the first sg table entry
	 * is aligned to the device page size, and that the part of the first
	 * sg table entry past the offset is a multiple of the device page
	 * size.
	 */
	if (accumulated_size < sgt_offset ||
	    (sg_dma_address(sgl) + first_sgl_offset) & ~PVR_DEVICE_PAGE_MASK ||
	    first_sgl_size & ~PVR_DEVICE_PAGE_MASK) {
		err = -EINVAL;
		goto err_out;
	}

	/*
	 * Resume iterating through the sg table until we hit an entry which
	 * contains (sgt_offset + size). Use do-while here because the first
	 * and last entries could be the same.
	 */
	do {
		if (accumulated_size >= sgt_offset + size)
			goto found_last_sgl;

		accumulated_size += sg_dma_len(sgl);

		/*
		 * This check should technically be at the top of this loop.
		 * However, we've already performed it above for the first
		 * iteration, so we move it to the bottom to prevent evaluating
		 * it again. It will still be performed before every break
		 * conditional.
		 */
		if (sg_dma_address(sgl) & ~PVR_DEVICE_PAGE_MASK ||
		    sg_dma_len(sgl) & ~PVR_DEVICE_PAGE_MASK) {
			err = -EINVAL;
			goto err_out;
		}
	} while ((sgl = sg_next(sgl)) != NULL);

	/*
	 * If we fall out of the loop above, we've reached the end of @sgt
	 * without finding the end of the requested range.
	 */
	err = -EINVAL;
	goto err_out;

found_last_sgl:
	/* Record the entry discovered in the loop above. */
	last_sgl = sgl;
	last_sgl_size = accumulated_size - (sgt_offset + size);

	/*
	 * Ensure (sgt_offset + size) is within the bounds of the sg table and
	 * that the part of the last sg table entry up to size is a
	 * multiple of the page size.
	 */
	if (accumulated_size < sgt_offset + size ||
	    last_sgl_size & ~PVR_DEVICE_PAGE_MASK ||
	    sg_dma_address(last_sgl) & ~PVR_DEVICE_PAGE_MASK) {
		err = -EINVAL;
		goto err_out;
	}

	err = pvr_page_table_ptr_init(&ptr, vm_ctx->pvr_dev,
				      &vm_ctx->root_table, device_addr, true);
	if (err) {
		err = -EINVAL;
		goto err_out;
	}

	/*
	 * If we only need to look at a single sg table entry, do that now so
	 * we can apply both first_sgt_offset and last_sgt_size to it.
	 */
	if (first_sgl == last_sgl) {
		err = pvr_vm_context_map_partial_sgl(vm_ctx, first_sgl,
						     first_sgl_offset,
						     last_sgl_size, &ptr,
						     page_flags);
		if (err)
			goto err_fini_ptr;

		/*
		 * Flag the L0 page table as requiring a flush when the page
		 * table pointer is destroyed.
		 */
		pvr_page_table_ptr_require_sync(&ptr, 0);

		goto out;
	}

	/*
	 * Before progressing, save a copy of the start pointer so we can use
	 * it again if we enter an error state and have to destroy pages.
	 * This is not needed for the case covered above since there is no
	 * route to err_unmap from there.
	 */
	pvr_page_table_ptr_copy(&ptr_copy, &ptr);

	/*
	 * When multiple sgls are mapped, we do so in three stages. Stages one
	 * and three take care of the first and last sgls respectively. Each of
	 * these use the associated values "offset" and "size" described above.
	 *
	 * The remaining "middle" sgls are mapped in their entirety by stage
	 * two which does not need to care about those values.
	 *
	 * If the first and last sgls are adjacent (i.e. there are exactly two
	 * sgls to map), stage two is skipped.
	 */

	/* [1/3] Map first page. */
	err = pvr_vm_context_map_partial_sgl(vm_ctx, first_sgl,
					     first_sgl_offset, first_sgl_size,
					     &ptr, page_flags);
	if (err)
		goto err_fini_ptr_and_ptr_copy;

	created_size = first_sgl_size;

	/* [2/3] Map middle pages (if any). */
	for (sgl = sg_next(first_sgl); sgl != last_sgl; sgl = sg_next(sgl)) {
		err = pvr_page_table_ptr_next_page(&ptr, true);
		if (err) {
			err = -EINVAL;
			goto err_unmap;
		}

		err = pvr_vm_context_map_sgl(vm_ctx, sgl, &ptr, page_flags);
		if (err)
			goto err_unmap;

		created_size += sg_dma_len(sgl);
	}

	/* [3/3] Map last page. */
	err = pvr_page_table_ptr_next_page(&ptr, true);
	if (err) {
		err = -EINVAL;
		goto err_unmap;
	}

	err = pvr_vm_context_map_partial_sgl(vm_ctx, last_sgl, 0, last_sgl_size,
					     &ptr, page_flags);
	if (err)
		goto err_unmap;

	/*
	 * No need to update &created_size here as there are no more routes
	 * to err_unmap past this point.
	 */

out:
	err = 0;
	goto err_fini_ptr_and_ptr_copy;

err_unmap:
	pvr_vm_context_unmap_from_ptr(&ptr_copy,
				      created_size >> PVR_DEVICE_PAGE_SHIFT);

err_fini_ptr_and_ptr_copy:
	pvr_page_table_ptr_fini(&ptr_copy);

err_fini_ptr:
	pvr_page_table_ptr_fini(&ptr);

err_out:
	return err;
}

/**
 * DOC: Memory mappings
 *
 * TODO
 */
/**
 * DOC: Memory mappings (constants)
 *
 * .. c:macro:: PVR_VM_MAPPING_COMPLETE
 *
 *    This is a "magic" value which, when assigned to the
 *    &pvr_vm_mapping->pvr_obj_offset member of a &struct pvr_vm_mapping,
 *    indicates that it maps the entire associated &struct pvr_gem_object.
 */

#define PVR_VM_MAPPING_COMPLETE ((unsigned int)(UINT_MAX))

/**
 * struct pvr_vm_mapping - TODO
 *
 * This structure implicitly contains the device address and size of the
 * mapping through @node members &pvr_vm_interval_tree_node.start and
 * &pvr_vm_interval_tree_node.size respectively.
 */
struct pvr_vm_mapping {
	struct pvr_vm_interval_tree_node node;
	struct pvr_gem_object *pvr_obj;
	unsigned int pvr_obj_offset;

	bool slc_bypass;
	bool pm_fw_protect;
};

/**
 * DOC: Memory mappings (implementation)
 */

PVR_GEN_INTERVAL_TREE_WRAPPER_DEFS(pvr_vm_mapping, node)

static struct pvr_page_flags_raw
pvr_vm_mapping_page_flags_raw(struct pvr_vm_mapping *mapping)
{
	/*
	 * FIXME: There is currently no way to mark a mapping as read-only or
	 * cache-coherent. Should there be?
	 */
	return pvr_page_flags_raw_create(false, false, mapping->slc_bypass,
					 mapping->pm_fw_protect);
}

/**
 * pvr_vm_mapping_init_partial() - TODO
 * @mapping: Target memory mapping.
 * @device_addr: TODO
 * @size: TODO
 * @pvr_obj: TODO
 * @pvr_obj_offset: TODO
 *
 * The memory behind @mapping should be zeroed before calling this function.
 */
static void
pvr_vm_mapping_init_partial(struct pvr_vm_mapping *mapping, u64 device_addr,
			    u64 size, struct pvr_gem_object *pvr_obj,
			    u64 pvr_obj_offset)
{
	u64 flags = pvr_obj->flags;

	/*
	 * Increment the refcount on the underlying physical memory resource
	 * to prevent de-allocation while the mapping exists.
	 */
	pvr_gem_object_get(pvr_obj);

	mapping->pvr_obj = pvr_obj;
	mapping->pvr_obj_offset = pvr_obj_offset;

	mapping->slc_bypass = flags & DRM_PVR_BO_DEVICE_BYPASS_CACHE;
	mapping->pm_fw_protect = flags & DRM_PVR_BO_DEVICE_PM_FW_PROTECT;

	pvr_vm_interval_tree_node_init(&mapping->node, device_addr, size);
}

/**
 * pvr_vm_mapping_init() - TODO
 * @mapping: Target memory mapping.
 * @device_addr: TODO
 * @pvr_obj: TODO
 *
 * Internally, this function just calls pvr_vm_mapping_init_partial() with the
 * extra arguments &size and &pvr_obj_offset populated with the size of
 * @pvr_obj and the "magic" constant %PVR_VM_MAPPING_COMPLETE respectively.
 */
static __always_inline void
pvr_vm_mapping_init(struct pvr_vm_mapping *mapping, u64 device_addr,
		    struct pvr_gem_object *pvr_obj)
{
	pvr_vm_mapping_init_partial(mapping, device_addr,
				    pvr_gem_object_size(pvr_obj), pvr_obj,
				    PVR_VM_MAPPING_COMPLETE);
}

static void
pvr_vm_mapping_fini(struct pvr_vm_mapping *mapping)
{
	pvr_vm_interval_tree_node_fini(&mapping->node);

	pvr_gem_object_put(mapping->pvr_obj);
}

static int
pvr_vm_mapping_map(struct pvr_vm_context *vm_ctx,
		   struct pvr_vm_mapping *mapping)
{
	int err;

	if (!pvr_gem_object_is_imported(mapping->pvr_obj)) {
		err = pvr_gem_object_get_pages(mapping->pvr_obj);
		if (err)
			goto err_out;
	}

	if (mapping->pvr_obj_offset == PVR_VM_MAPPING_COMPLETE) {
		err = pvr_vm_context_map_sgt(
			vm_ctx, mapping->pvr_obj->sgt,
			pvr_vm_mapping_start(mapping),
			pvr_vm_mapping_page_flags_raw(mapping));
	} else {
		err = pvr_vm_context_map_partial_sgt(
			vm_ctx, mapping->pvr_obj->sgt, mapping->pvr_obj_offset,
			pvr_vm_mapping_start(mapping),
			pvr_vm_mapping_size(mapping),
			pvr_vm_mapping_page_flags_raw(mapping));
	}
	WARN_ON(pvr_vm_mmu_flush(vm_ctx->pvr_dev));
	if (err)
		goto err_put_pages;

	pvr_vm_mapping_tree_insert(&vm_ctx->mappings, mapping);

	return 0;

err_put_pages:
	if (!pvr_gem_object_is_imported(mapping->pvr_obj))
		pvr_gem_object_put_pages(mapping->pvr_obj);

err_out:
	return err;
}

static int
pvr_vm_mapping_unmap(struct pvr_vm_context *vm_ctx,
		     struct pvr_vm_mapping *mapping)
{
	int err;

	pvr_vm_mapping_tree_remove(&vm_ctx->mappings, mapping);

	err = pvr_vm_context_unmap(vm_ctx, pvr_vm_mapping_start(mapping),
				   pvr_vm_mapping_size(mapping) >> PVR_DEVICE_PAGE_SHIFT);
	WARN_ON(pvr_vm_mmu_flush(vm_ctx->pvr_dev));
	if (err)
		goto err_out;

	if (!pvr_gem_object_is_imported(mapping->pvr_obj))
		pvr_gem_object_put_pages(mapping->pvr_obj);

	return 0;

err_out:
	return err;
}

/*
 * Public API
 *
 * For an overview of these functions, see *DOC: Public API* in "pvr_vm.h".
 */

/**
 * pvr_device_addr_is_valid() - Tests whether a virtual device address
 *                              is valid.
 * @device_addr: Virtual device address to test.
 *
 * Return:
 *  * %true if @device_addr is within the valid range for a device page
 *    table and is aligned to the device page size, or
 *  * %false otherwise.
 */
bool
pvr_device_addr_is_valid(u64 device_addr)
{
	return (device_addr & ~PVR_PAGE_TABLE_ADDR_MASK) == 0 &&
	       (device_addr & ~PVR_DEVICE_PAGE_MASK) == 0;
}

/**
 * pvr_device_addr_and_size_are_valid() - Tests whether a device-virtual
 * address and associated size are both valid.
 *
 * Calling pvr_device_addr_is_valid() twice (once on @size, and again on
 * @device_addr + @size) to verify a device-virtual address range initially
 * seems intuitive, but it produces a false-negative when the address range
 * is right at the end of device-virtual address space.
 *
 * This function catches that corner case, as well as checking that
 * @size is non-zero.
 *
 * Return:
 *  * %true if @device_addr is device page aligned; @size is device page
 *    aligned; the range specified by @device_addr and @size is within the
 *    bounds of the device-virtual address space, and @size is non-zero, or
 *  * %false otherwise.
 */
bool
pvr_device_addr_and_size_are_valid(u64 device_addr, u64 size)
{
	return pvr_device_addr_is_valid(device_addr) &&
	       size != 0 && (size & ~PVR_DEVICE_PAGE_MASK) == 0 &&
	       (device_addr + size <= PVR_PAGE_TABLE_ADDR_SPACE_SIZE);
}

/**
 * pvr_vm_create_context() - Create a new VM context.
 * @pvr_dev: Target PowerVR device.
 *
 * Return:
 *  * A handle to the newly-minted VM context on success,
 *  * -%EINVAL if the feature "virtual address space bits" on @pvr_dev is
 *    missing or has an unsupported value,
 *  * -%ENOMEM if allocation of the structure behind the opaque handle fails,
 *    or
 *  * Any error encountered while setting up internal structures.
 */
struct pvr_vm_context *
pvr_vm_create_context(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);

	struct pvr_vm_context *vm_ctx;
	u16 device_addr_bits;

	int err;

	err = PVR_FEATURE_VALUE(pvr_dev, virtual_address_space_bits,
				&device_addr_bits);
	if (err) {
		drm_err(drm_dev,
			"Failed to get device virtual address space bits\n");
		goto err_out;
	}

	if (device_addr_bits != PVR_PAGE_TABLE_ADDR_BITS) {
		drm_err(drm_dev,
			"Device has unsupported virtual address space size\n");
		err = -EINVAL;
		goto err_out;
	}

	vm_ctx = kzalloc(sizeof(*vm_ctx), GFP_KERNEL);
	if (!vm_ctx) {
		err = -ENOMEM;
		goto err_out;
	}

	err = pvr_vm_context_init(vm_ctx, pvr_dev);
	if (err)
		goto err_free_vm_ctx;

	return vm_ctx;

err_free_vm_ctx:
	kfree(vm_ctx);

err_out:
	return ERR_PTR(err);
}

/**
 * pvr_vm_destroy_context() - Destroy an existing VM context.
 * @vm_ctx: Target VM context.
 * @enable_warnings: Whether to print warnings for remaining mappings.
 *
 * It is an error to call pvr_vm_destroy_context() on a VM context that has
 * already been destroyed.
 */
void
pvr_vm_destroy_context(struct pvr_vm_context *vm_ctx, bool enable_warnings)
{
	pvr_vm_context_fini(vm_ctx, enable_warnings);
	kfree(vm_ctx);
}

static int
pvr_vm_map_mapping_locked(struct pvr_vm_context *vm_ctx,
			  struct pvr_vm_mapping *mapping)
{
	u64 device_addr = pvr_vm_mapping_start(mapping);
	u64 size = pvr_vm_mapping_size(mapping);

	int err;

	lockdep_assert_held(&vm_ctx->lock);

	/*
	 * Check that the requested mapping range does not overlap with an
	 * existing mapping.
	 */
	if (pvr_vm_mapping_tree_contains(&vm_ctx->mappings, device_addr,
					 size)) {
		err = -EEXIST;
		goto err_out;
	}

	err = pvr_vm_mapping_map(vm_ctx, mapping);
	if (err)
		goto err_out;

	return 0;

err_out:
	return err;
}

/**
 * pvr_vm_map() - Map a section of physical memory into a section of virtual
 *                device memory.
 * @vm_ctx: Target VM context.
 * @pvr_obj: Target PowerVR memory object.
 * @device_addr: Virtual device address at the start of the requested mapping.
 *
 * If you only need to map part of @pvr_obj, use pvr_vm_map_partial() instead.
 *
 * No handle is returned to represent the mapping. Instead, callers should
 * remember @device_addr and use that as a handle.
 *
 * Return:
 *  * 0 on success,
 *  * -%EINVAL if @device_addr is not a valid page-aligned virtual device
 *    address or any part of @pvr_obj is not device virtual page-aligned,
 *  * -%EEXIST if the requested mapping overlaps with an existing mapping,
 *  * -%ENOMEM if allocation of internally required CPU memory fails, or
 *  * Any error encountered while performing internal operations required to
 *    create the mapping.
 */
int
pvr_vm_map(struct pvr_vm_context *vm_ctx, struct pvr_gem_object *pvr_obj,
	   u64 device_addr)
{
	size_t size = pvr_gem_object_size(pvr_obj);

	struct pvr_vm_mapping *mapping;
	int err;

	if (!pvr_device_addr_and_size_are_valid(device_addr, size) ||
	    size & ~PAGE_MASK) {
		err = -EINVAL;
		goto err_out;
	}

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping) {
		err = -ENOMEM;
		goto err_out;
	}

	mutex_lock(&vm_ctx->lock);

	pvr_vm_mapping_init(mapping, device_addr, pvr_obj);

	err = pvr_vm_map_mapping_locked(vm_ctx, mapping);
	if (err)
		goto err_fini_mapping;

	err = 0;
	goto err_unlock;

err_fini_mapping:
	pvr_vm_mapping_fini(mapping);
	kfree(mapping);

err_unlock:
	mutex_unlock(&vm_ctx->lock);

err_out:
	return err;
}

/**
 * pvr_vm_map_partial() - Map a section of physical memory into a section of
 *                        virtual device memory.
 * @vm_ctx: Target VM context.
 * @pvr_obj: Target PowerVR memory object.
 * @pvr_obj_offset: Offset into @pvr_obj to map from.
 * @device_addr: Virtual device address at the start of the requested mapping.
 * @size: Size of the requested mapping.
 *
 * If you need to map an entire @pvr_obj, use pvr_vm_map() instead.
 *
 * No handle is returned to represent the mapping. Instead, callers should
 * remember @device_addr and use that as a handle.
 *
 * Return:
 *  * 0 on success,
 *  * -%EINVAL if @device_addr is not a valid page-aligned virtual device
 *    address; the region specified by @pvr_obj_offset and @size does not fall
 *    entirely within @pvr_obj, or any part of the specified region of @pvr_obj
 *    is not device virtual page-aligned,
 *  * -%EEXIST if the requested mapping overlaps with an existing mapping,
 *  * -%ENOMEM if allocation of internally required CPU memory fails, or
 *  * Any error encountered while performing internal operations required to
 *    create the mapping.
 */
int
pvr_vm_map_partial(struct pvr_vm_context *vm_ctx,
		   struct pvr_gem_object *pvr_obj, u64 pvr_obj_offset,
		   u64 device_addr, u64 size)
{
	size_t pvr_obj_size = pvr_gem_object_size(pvr_obj);

	struct pvr_vm_mapping *mapping;
	int err;

	if (!pvr_device_addr_and_size_are_valid(device_addr, size) ||
	    pvr_obj_offset & ~PAGE_MASK || size & ~PAGE_MASK ||
	    pvr_obj_offset + size > pvr_obj_size ||
	    pvr_obj_offset > pvr_obj_size) {
		err = -EINVAL;
		goto err_out;
	}

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping) {
		err = -ENOMEM;
		goto err_out;
	}

	mutex_lock(&vm_ctx->lock);

	pvr_vm_mapping_init_partial(mapping, device_addr, size, pvr_obj,
				    pvr_obj_offset);

	err = pvr_vm_map_mapping_locked(vm_ctx, mapping);
	if (err)
		goto err_fini_mapping;

	err = 0;
	goto err_unlock;

err_fini_mapping:
	pvr_vm_mapping_fini(mapping);
	kfree(mapping);

err_unlock:
	mutex_unlock(&vm_ctx->lock);

err_out:
	return err;
}

/**
 * pvr_vm_unmap() - Unmap an already mapped section of virtual device memory.
 * @vm_ctx: Target VM context.
 * @device_addr: Virtual device address at the start of the target mapping.
 *
 * Return:
 *  * 0 on success,
 *  * -%EINVAL if @device_addr is not a valid page-aligned virtual device
 *    address,
 *  * -%ENOENT if @device_addr is not a handle to an existing mapping, or
 *  * Any error encountered while performing internal operations required to
 *    destroy the mapping.
 */
int
pvr_vm_unmap(struct pvr_vm_context *vm_ctx, u64 device_addr)
{
	struct pvr_vm_mapping *mapping;
	int err;

	if (!pvr_device_addr_is_valid(device_addr)) {
		err = -EINVAL;
		goto err_out;
	}

	mutex_lock(&vm_ctx->lock);

	mapping = pvr_vm_mapping_tree_iter_first(&vm_ctx->mappings,
						 device_addr, 0);
	if (!mapping || pvr_vm_mapping_start(mapping) != device_addr) {
		err = -ENOENT;
		goto err_unlock;
	}

	err = pvr_vm_mapping_unmap(vm_ctx, mapping);
	if (err)
		goto err_unlock;

	pvr_vm_mapping_fini(mapping);
	kfree(mapping);

	err = 0;
	goto err_unlock;

err_unlock:
	mutex_unlock(&vm_ctx->lock);

err_out:
	return err;
}

/*
 * Static data areas are determined by firmware. Each array must be kept
 * in order of increasing offset, as this is used to determine the size of the
 * reserved area for each heap.
 */
static const struct drm_pvr_static_data_area general_static_data_areas[] = {
	{
		.id = DRM_PVR_STATIC_DATA_AREA_FENCE,
		.offset = 0,
		.size = 128,
	},
	{
		.id = DRM_PVR_STATIC_DATA_AREA_YUV_CSC,
		.offset = 128,
		.size = 1024,
	},
};

static const struct drm_pvr_static_data_area pds_static_data_areas[] = {
	{
		.id = DRM_PVR_STATIC_DATA_AREA_VDM_SYNC,
		.offset = 0,
		.size = 128,
	},
	{
		.id = DRM_PVR_STATIC_DATA_AREA_EOT,
		.offset = 128,
		.size = 128,
	},
};

static const struct drm_pvr_static_data_area usc_static_data_areas[] = {
	{
		.id = DRM_PVR_STATIC_DATA_AREA_VDM_SYNC,
		.offset = 0,
		.size = 128,
	},
};

#define LAST_AREA(areas) (areas[ARRAY_SIZE(areas) - 1])

#define GET_RESERVED_SIZE(areas) round_up((LAST_AREA(areas).offset + LAST_AREA(areas).size), \
					      PAGE_SIZE)

static const struct pvr_heap pvr_heaps[] = {
	{
		.id = DRM_PVR_HEAP_GENERAL,
		.flags = 0,
		.base = ROGUE_GENERAL_HEAP_BASE,
		.size = ROGUE_GENERAL_HEAP_SIZE,
		.reserved_base = ROGUE_GENERAL_HEAP_BASE,
		.reserved_size = GET_RESERVED_SIZE(general_static_data_areas),
		.page_size_log2 = 12,
		.static_data_areas = general_static_data_areas,
		.nr_static_data_areas = ARRAY_SIZE(general_static_data_areas),
	},
	{
		.id = DRM_PVR_HEAP_PDS_CODE_DATA,
		.flags = 0,
		.base = ROGUE_PDSCODEDATA_HEAP_BASE,
		.size = ROGUE_PDSCODEDATA_HEAP_SIZE,
		.reserved_base = ROGUE_PDSCODEDATA_HEAP_BASE,
		.reserved_size = GET_RESERVED_SIZE(pds_static_data_areas),
		.page_size_log2 = 12,
		.static_data_areas = pds_static_data_areas,
		.nr_static_data_areas = ARRAY_SIZE(pds_static_data_areas),
	},
	{
		.id = DRM_PVR_HEAP_USC_CODE,
		.flags = 0,
		.base = ROGUE_USCCODE_HEAP_BASE,
		.size = ROGUE_USCCODE_HEAP_SIZE,
		.reserved_base = ROGUE_USCCODE_HEAP_BASE,
		.reserved_size = GET_RESERVED_SIZE(usc_static_data_areas),
		.page_size_log2 = 12,
		.static_data_areas = usc_static_data_areas,
		.nr_static_data_areas = ARRAY_SIZE(usc_static_data_areas),
	},
	{
		.id = DRM_PVR_HEAP_VIS_TEST,
		.flags = 0,
		.base = ROGUE_VISTEST_HEAP_BASE,
		.size = ROGUE_VISTEST_HEAP_SIZE,
		.reserved_base = 0,
		.reserved_size = 0,
		.page_size_log2 = 12,
		.static_data_areas = NULL,
		.nr_static_data_areas = 0,
	},
};

static const struct pvr_heap rgnhdr_heap = {
	.id = DRM_PVR_HEAP_RGNHDR,
	.flags = 0,
	.base = ROGUE_RGNHDR_HEAP_BASE,
	.size = ROGUE_RGNHDR_HEAP_SIZE,
	.reserved_base = 0,
	.reserved_size = 0,
	.page_size_log2 = 12,
};

static __always_inline u32
pvr_get_nr_heaps(struct pvr_device *pvr_dev)
{
	u32 heaps = ARRAY_SIZE(pvr_heaps);

	/* Region header heap is only present if BRN63142 is present. */
	if (PVR_HAS_QUIRK(pvr_dev, 63142))
		heaps++;

	return heaps;
}

int
pvr_get_heap_info(struct pvr_device *pvr_dev, struct drm_pvr_ioctl_get_heap_info_args *args)
{
	int err;

	if (args->data) {
		const struct pvr_heap *pvr_heap;

		if (args->heap_nr < ARRAY_SIZE(pvr_heaps)) {
			pvr_heap = &pvr_heaps[args->heap_nr];
		} else if (args->heap_nr == ARRAY_SIZE(pvr_heaps) &&
			PVR_HAS_QUIRK(pvr_dev, 63142)) {
			/* Region header heap is only present if BRN63142 is present. */
			pvr_heap = &rgnhdr_heap;
		} else  {
			err = -EINVAL;
			goto err_out;
		}

		switch (args->op) {
		case DRM_PVR_HEAP_OP_GET_HEAP_INFO: {
			struct drm_pvr_heap heap_out;

			heap_out.id = pvr_heap->id;
			heap_out.flags = pvr_heap->flags;
			heap_out.base = pvr_heap->base;
			heap_out.size = pvr_heap->size;
			heap_out.reserved_base = pvr_heap->reserved_base;
			heap_out.reserved_size = pvr_heap->reserved_size;
			heap_out.page_size_log2 = pvr_heap->page_size_log2;
			heap_out.nr_static_data_areas = pvr_heap->nr_static_data_areas;

			if (copy_to_user(u64_to_user_ptr(args->data), &heap_out,
					 sizeof(heap_out))) {
				err = -EFAULT;
				goto err_out;
			}
			break;
		}

		case DRM_PVR_HEAP_OP_GET_STATIC_DATA_AREAS: {
			if (!pvr_heap->nr_static_data_areas) {
				err = -EINVAL;
				goto err_out;
			}

			if (copy_to_user(u64_to_user_ptr(args->data),
					 pvr_heap->static_data_areas,
					 pvr_heap->nr_static_data_areas *
					 sizeof(struct drm_pvr_static_data_area))) {
				err = -EFAULT;
				goto err_out;
			}
			break;
		}

		default:
			err = -EINVAL;
			goto err_out;
		}
	}

	/* Always write the number of heaps, regardless of whether a data pointer was provided. */
	args->nr_heaps = pvr_get_nr_heaps(pvr_dev);

	return 0;

err_out:
	return err;
}

/**
 * pvr_heap_contains_range() - Determine if a given heap contains the specified
 * device-virtual address range.
 * @heap: Target heap.
 * @start: Inclusive start of the target range.
 * @end: Inclusive end of the target range.
 *
 * It is an error to call this function with values of @start and @end that do
 * not satisfy the condition @start <= @end.
 *
 * Return:
 *  * %true iff @heap completely contains the range specified by @start and
 *    @end, or
 *  * %false otherwise.
 */
static __always_inline bool
pvr_heap_contains_range(const struct pvr_heap *pvr_heap, u64 start, u64 end)
{
	return pvr_heap->base <= start && end < pvr_heap->base + pvr_heap->size;
}

/**
 * pvr_find_heap_containing() - Find a heap which contains the specified
 * device-virtual address range.
 * @pvr_dev: Target PowerVR device.
 * @start: Start of the target range.
 * @size: Size of the target range.
 *
 * Return:
 *  * A pointer to a constant instance of struct drm_pvr_heap representing the
 *    heap containing the entire range specified by @start and @size on
 *    success, or
 *  * %NULL if no such heap exists.
 */
const struct pvr_heap *
pvr_find_heap_containing(struct pvr_device *pvr_dev, u64 start, u64 size)
{
	u32 heap_id;
	u64 end;

	if (check_add_overflow(start, size - 1, &end))
		return NULL;

	/*
	 * There are no optimizations to be made here, since there are no
	 * guarantees about the order of &pvr_heaps. Iterate over the entire
	 * array.
	 */
	for (heap_id = 0; heap_id < ARRAY_SIZE(pvr_heaps); ++heap_id) {
		/*
		 * If the target range is completely within this heap, break
		 * out and return the heap.
		 */
		if (pvr_heap_contains_range(&pvr_heaps[heap_id], start, end))
			return &pvr_heaps[heap_id];
	}

	/* Search quirky heaps only if the associated quirk is present. */
	if (PVR_HAS_QUIRK(pvr_dev, 63142) &&
	    pvr_heap_contains_range(&rgnhdr_heap, start, end)) {
		return &rgnhdr_heap;
	}

	/* If we make it this far, we've exhausted all possible heaps. */
	return NULL;
}

/**
 * pvr_vm_find_gem_object() - Look up a buffer object from a given virtual
 * device address.
 * @vm_ctx: [IN] Target VM context.
 * @device_addr: [IN] Virtual device address at the start of the required
 * object.
 * @mapped_offset_out: [OUT] Pointer to location to write offset of the start
 * of the mapped region within the buffer object. May be %NULL if this
 * information is not required.
 * @mapped_size_out: [OUT] Pointer to location to write size of the mapped
 * region. May be %NULL if this information is not required.
 *
 * If successful, a reference will be taken on the buffer object. The caller
 * must drop the reference with pvr_gem_object_put().
 *
 * Return:
 *  * The PowerVR buffer object mapped at @device_addr if one exists, or
 *  * -%EINVAL otherwise.
 */
struct pvr_gem_object *
pvr_vm_find_gem_object(struct pvr_vm_context *vm_ctx, u64 device_addr,
		       u64 *mapped_offset_out, u64 *mapped_size_out)
{
	struct pvr_vm_mapping *mapping;
	struct pvr_gem_object *pvr_obj;
	int err;

	mutex_lock(&vm_ctx->lock);

	mapping = pvr_vm_mapping_tree_iter_first(&vm_ctx->mappings,
						 device_addr, 0);
	if (!mapping || pvr_vm_mapping_start(mapping) != device_addr) {
		err = -EINVAL;
		goto err_unlock;
	}

	pvr_obj = mapping->pvr_obj;
	if (WARN_ON(!pvr_obj)) {
		err = -EINVAL;
		goto err_unlock;
	}

	pvr_gem_object_get(pvr_obj);

	if (mapped_offset_out)
		*mapped_offset_out = mapping->pvr_obj_offset;
	if (mapped_size_out)
		*mapped_size_out = pvr_vm_mapping_size(mapping);

	mutex_unlock(&vm_ctx->lock);

	return pvr_obj;

err_unlock:
	mutex_unlock(&vm_ctx->lock);

	return ERR_PTR(err);
}
