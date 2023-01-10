// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_free_list.h"
#include "pvr_gem.h"
#include "pvr_object.h"
#include "pvr_rogue_fwif.h"
#include "pvr_vm.h"

#include <drm/drm_gem.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <uapi/drm/pvr_drm.h>

#define FREE_LIST_ENTRY_SIZE sizeof(u32)

#define FREE_LIST_ALIGNMENT \
	((ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE / FREE_LIST_ENTRY_SIZE) - 1)

#define FREE_LIST_MIN_PAGES 25
#define FREE_LIST_MIN_PAGES_BRN66011 40
#define FREE_LIST_MIN_PAGES_ROGUEXE 25

/**
 * pvr_get_free_list_min_pages() - Get minimum free list size for this device
 * @pvr_dev: Device pointer.
 *
 * Returns:
 *  * Minimum free list size, in PM physical pages.
 */
u32
pvr_get_free_list_min_pages(struct pvr_device *pvr_dev)
{
	u32 value;

	if (PVR_HAS_FEATURE(pvr_dev, roguexe)) {
		if (PVR_HAS_QUIRK(pvr_dev, 66011))
			value = FREE_LIST_MIN_PAGES_BRN66011;
		else
			value = FREE_LIST_MIN_PAGES_ROGUEXE;
	} else {
		value = FREE_LIST_MIN_PAGES;
	}

	return value;
}

static int
free_list_create_kernel_structure(struct pvr_file *pvr_file,
				  struct drm_pvr_ioctl_create_free_list_args *args,
				  struct pvr_free_list *free_list)
{
	struct pvr_gem_object *free_list_obj;
	u64 free_list_size;
	int err;

	if (args->grow_threshold < 0 || args->grow_threshold > 100 ||
	    args->initial_num_pages > args->max_num_pages ||
	    args->grow_num_pages > args->max_num_pages ||
	    args->max_num_pages == 0 ||
	    (args->initial_num_pages < args->max_num_pages && !args->grow_num_pages) ||
	    (args->initial_num_pages == args->max_num_pages && args->grow_num_pages)) {
		err = -EINVAL;
		goto err_out;
	}
	if ((args->initial_num_pages & FREE_LIST_ALIGNMENT) ||
	    (args->max_num_pages & FREE_LIST_ALIGNMENT) ||
	    (args->grow_num_pages & FREE_LIST_ALIGNMENT)) {
		err = -EINVAL;
		goto err_out;
	}

	free_list_obj = pvr_vm_find_gem_object(pvr_file->user_vm_ctx, args->free_list_gpu_addr,
					       NULL, &free_list_size);
	if (!free_list_obj) {
		err = -EINVAL;
		goto err_out;
	}

	if ((free_list_obj->flags & DRM_PVR_BO_CPU_ALLOW_USERSPACE_ACCESS) ||
	    !(free_list_obj->flags & DRM_PVR_BO_DEVICE_PM_FW_PROTECT) ||
	    free_list_size < (args->max_num_pages * FREE_LIST_ENTRY_SIZE)) {
		err = -EINVAL;
		goto err_put_free_list_obj;
	}

	free_list->base.type = DRM_PVR_OBJECT_TYPE_FREE_LIST;
	free_list->pvr_dev = pvr_file->pvr_dev;
	free_list->current_pages = 0;
	free_list->max_pages = args->max_num_pages;
	free_list->grow_pages = args->grow_num_pages;
	free_list->grow_threshold = args->grow_threshold;
	free_list->id = atomic_inc_return(&pvr_file->free_list_id);
	INIT_LIST_HEAD(&free_list->mem_block_list);
	free_list->obj = free_list_obj;

	err = pvr_gem_object_get_pages(free_list->obj);
	if (err < 0)
		goto err_put_free_list_obj;

	return 0;

err_put_free_list_obj:
	pvr_gem_object_put(free_list_obj);

err_out:
	return err;
}

static void
free_list_destroy_kernel_structure(struct pvr_free_list *free_list)
{
	pvr_gem_object_put_pages(free_list->obj);
	pvr_gem_object_put(free_list->obj);
}

/**
 * calculate_free_list_ready_pages() - Function to work out the number of free
 *                                     list pages to reserve for growing within
 *                                     the FW without having to wait for the
 *                                     host to progress a grow request
 * @free_list: Pointer to free list.
 * @pages: Total pages currently in free list.
 *
 * If the threshold or grow size means less than the alignment size (4 pages on
 * Rogue), then the feature is not used.
 *
 * Return: number of pages to reserve.
 */
static u32
calculate_free_list_ready_pages(struct pvr_free_list *free_list, u32 pages)
{
	u32 ready_pages = ((pages * free_list->grow_threshold) / 100);

	/* The number of pages must be less than the grow size. */
	ready_pages = min(ready_pages, free_list->grow_pages);

	/*
	 * The number of pages must be a multiple of the free list align size.
	 */
	ready_pages &= ~FREE_LIST_ALIGNMENT;

	return ready_pages;
}

static int
free_list_create_fw_structure(struct pvr_file *pvr_file,
			      struct drm_pvr_ioctl_create_free_list_args *args,
			      struct pvr_free_list *free_list)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct rogue_fwif_freelist *free_list_fw;
	u32 ready_pages;
	int err;

	/*
	 * Create and map the FW structure so we can initialise it. This is not
	 * accessed on the CPU side post-initialisation so the mapping lifetime
	 * is only for this function.
	 */
	free_list_fw = pvr_gem_create_and_map_fw_object(pvr_dev, sizeof(*free_list_fw),
							PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
							DRM_PVR_BO_CREATE_ZEROED,
							&free_list->fw_obj);
	if (IS_ERR(free_list_fw)) {
		err = PTR_ERR(free_list_fw);
		goto err_out;
	}

	/* Fill out FW structure */
	ready_pages = calculate_free_list_ready_pages(free_list,
						      args->initial_num_pages);

	free_list_fw->max_pages = free_list->max_pages;
	free_list_fw->current_pages = args->initial_num_pages - ready_pages;
	free_list_fw->grow_pages = free_list->grow_pages;
	free_list_fw->ready_pages = ready_pages;
	free_list_fw->freelist_id = free_list->id;
	free_list_fw->grow_pending = false;
	free_list_fw->current_stack_top = free_list_fw->current_pages - 1;
	free_list_fw->freelist_dev_addr = args->free_list_gpu_addr;
	free_list_fw->current_dev_addr =
		(free_list_fw->freelist_dev_addr +
		 ((free_list_fw->max_pages - free_list_fw->current_pages) *
		  FREE_LIST_ENTRY_SIZE)) &
		~((u64)ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE - 1);

	pvr_fw_object_vunmap(free_list->fw_obj, free_list_fw, false);

	return 0;

err_out:
	return err;
}

static void
free_list_destroy_fw_structure(struct pvr_free_list *free_list)
{
	pvr_fw_object_release(free_list->fw_obj);
}

static int
pvr_free_list_insert_pages(struct pvr_free_list *free_list,
			   struct sg_table *sgt, u32 offset, u32 num_pages)
{
	struct sg_dma_page_iter dma_iter;
	u32 *page_list;
	int err;

	page_list = pvr_gem_object_vmap(free_list->obj, false);
	if (IS_ERR(page_list)) {
		err = PTR_ERR(page_list);
		goto err_out;
	}

	offset /= FREE_LIST_ENTRY_SIZE;
	/* clang-format off */
	for_each_sgtable_dma_page(sgt, &dma_iter, 0) {
		dma_addr_t dma_addr = sg_page_iter_dma_address(&dma_iter);
		u64 dma_pfn = dma_addr >>
			       ROGUE_BIF_PM_PHYSICAL_PAGE_ALIGNSHIFT;
		u32 dma_addr_offset;

		BUILD_BUG_ON(ROGUE_BIF_PM_PHYSICAL_PAGE_SIZE > PAGE_SIZE);

		for (dma_addr_offset = 0; dma_addr_offset < PAGE_SIZE;
		     dma_addr_offset += ROGUE_BIF_PM_PHYSICAL_PAGE_SIZE) {
			WARN_ON_ONCE(dma_pfn >> 32);

			page_list[offset++] = (u32)dma_pfn;
			dma_pfn++;

			num_pages--;
			if (!num_pages)
				break;
		}

		if (!num_pages)
			break;
	};
	/* clang-format on */

	pvr_gem_object_vunmap(free_list->obj, true);

	return 0;

err_out:
	return err;
}

static int
pvr_free_list_grow(struct pvr_free_list *free_list, u32 num_pages)
{
	struct pvr_device *pvr_dev = free_list->pvr_dev;
	struct pvr_free_list_node *free_list_node;
	u32 start_page;
	u32 offset;
	int err;

	if (num_pages & FREE_LIST_ALIGNMENT) {
		err = -EINVAL;
		goto err_out;
	}

	free_list_node = kzalloc(sizeof(*free_list_node), GFP_KERNEL);
	if (!free_list_node) {
		err = -ENOMEM;
		goto err_out;
	}

	free_list_node->num_pages = num_pages;
	free_list_node->free_list = free_list;

	free_list_node->mem_obj = pvr_gem_object_create(pvr_dev,
							num_pages <<
							ROGUE_BIF_PM_PHYSICAL_PAGE_ALIGNSHIFT,
							PVR_BO_FW_FLAGS_DEVICE_CACHED);
	if (IS_ERR(free_list_node->mem_obj)) {
		err = PTR_ERR(free_list_node->mem_obj);
		goto err_free;
	}

	err = pvr_gem_object_get_pages(free_list_node->mem_obj);
	if (err < 0)
		goto err_destroy_gem_object;

	start_page = free_list->max_pages - free_list->current_pages -
		     free_list_node->num_pages;
	offset = ((start_page * FREE_LIST_ENTRY_SIZE) &
		  ~((u64)ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE - 1));

	pvr_free_list_insert_pages(free_list, free_list_node->mem_obj->sgt,
				   offset, num_pages);

	list_add_tail(&free_list_node->node, &free_list->mem_block_list);

	free_list->current_pages += num_pages;

	/*
	 * Reserve a number ready pages to allow the FW to process OOM quickly
	 * and asynchronously request a grow.
	 */
	free_list->ready_pages =
		calculate_free_list_ready_pages(free_list,
						free_list->current_pages);
	free_list->current_pages -= free_list->ready_pages;

	return 0;

err_destroy_gem_object:
	pvr_gem_object_put(free_list_node->mem_obj);

err_free:
	kfree(free_list_node);

err_out:
	return err;
}

static void
pvr_free_list_free_node(struct pvr_free_list_node *free_list_node)
{
	pvr_gem_object_put_pages(free_list_node->mem_obj);
	pvr_gem_object_put(free_list_node->mem_obj);

	kfree(free_list_node);
}

/**
 * pvr_free_list_create() - Create a new free list and return an object pointer
 * @pvr_file: Pointer to pvr_file structure.
 * @args: Creation arguments from userspace.
 *
 * Return:
 *  * Free list pointer on success, or
 *  * -%ENOMEM on out of memory.
 */
struct pvr_free_list *
pvr_free_list_create(struct pvr_file *pvr_file,
		     struct drm_pvr_ioctl_create_free_list_args *args)
{
	struct pvr_free_list *free_list;
	int err;

	/* Create and fill out the kernel structure */
	free_list = kzalloc(sizeof(*free_list), GFP_KERNEL);
	if (!free_list) {
		err = -ENOMEM;
		goto err_out;
	}

	err = free_list_create_kernel_structure(pvr_file, args, free_list);
	if (err < 0)
		goto err_free;

	err = free_list_create_fw_structure(pvr_file, args, free_list);
	if (err < 0)
		goto err_destroy_kernel_structure;

	err = pvr_free_list_grow(free_list, args->initial_num_pages);
	if (err < 0)
		goto err_destroy_fw_structure;

	return free_list;

err_destroy_fw_structure:
	free_list_destroy_fw_structure(free_list);

err_destroy_kernel_structure:
	free_list_destroy_kernel_structure(free_list);

err_free:
	kfree(free_list);

err_out:
	return ERR_PTR(err);
}

/**
 * pvr_free_list_destroy() - Destroy a free list
 * @free_list: Free list to be destroyed.
 *
 * This should not be called directly. Free list references should be dropped via
 * pvr_free_list_put().
 */
void
pvr_free_list_destroy(struct pvr_free_list *free_list)
{
	struct list_head *pos, *n;

	WARN_ON(pvr_object_cleanup(free_list->pvr_dev, ROGUE_FWIF_CLEANUP_FREELIST,
				   free_list->fw_obj, 0));

	/* clang-format off */
	list_for_each_safe(pos, n, &free_list->mem_block_list) {
		struct pvr_free_list_node *free_list_node =
			container_of(pos, struct pvr_free_list_node, node);

		list_del(pos);
		pvr_free_list_free_node(free_list_node);
	}
	/* clang-format on */

	free_list_destroy_kernel_structure(free_list);
	free_list_destroy_fw_structure(free_list);
	kfree(free_list);
}
