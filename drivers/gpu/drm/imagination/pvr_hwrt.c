// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_free_list.h"
#include "pvr_hwrt.h"
#include "pvr_gem.h"
#include "pvr_object.h"
#include "pvr_rogue_fwif.h"

#include <drm/drm_gem.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <uapi/drm/pvr_drm.h>

/* Size of Shadow Render Target Cache entry */
#define SRTC_ENTRY_SIZE sizeof(u32)
/* Size of Renders Accumulation Array entry */
#define RAA_ENTRY_SIZE sizeof(u32)

static int
hwrt_init_kernel_structure(struct pvr_file *pvr_file,
			   struct drm_pvr_ioctl_create_hwrt_dataset_args *args,
			   struct create_hwrt_free_list_args *free_list_args,
			   struct pvr_hwrt_dataset *hwrt)
{
	int err;
	int i;

	hwrt->base.type = DRM_PVR_OBJECT_TYPE_HWRT_DATASET;
	hwrt->pvr_dev = pvr_file->pvr_dev;
	hwrt->max_rts = args->max_rts;
	hwrt->num_free_lists = args->num_free_lists;
	hwrt->num_rt_datas = args->num_rt_datas;

	/* Get pointers to the free lists */
	for (i = 0; i < hwrt->num_free_lists; i++) {
		hwrt->free_lists[i] =
			pvr_free_list_get(pvr_file,
					  free_list_args[i].free_list_handle);
		if (!hwrt->free_lists[i]) {
			err = -EINVAL;
			goto err_put_free_lists;
		}
	}

	return 0;

err_put_free_lists:
	for (i = 0; i < hwrt->num_free_lists; i++) {
		if (hwrt->free_lists[i]) {
			pvr_free_list_put(hwrt->free_lists[i]);
			hwrt->free_lists[i] = NULL;
		}
	}

	return err;
}

static void
hwrt_fini_kernel_structure(struct pvr_hwrt_dataset *hwrt)
{
	int i;

	for (i = 0; i < hwrt->num_free_lists; i++) {
		if (hwrt->free_lists[i]) {
			pvr_free_list_put(hwrt->free_lists[i]);
			hwrt->free_lists[i] = NULL;
		}
	}
}

static int
hwrt_init_common_fw_structure(struct pvr_file *pvr_file,
			      struct drm_pvr_ioctl_create_hwrt_dataset_args *args,
			      struct pvr_hwrt_dataset *hwrt)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct rogue_fwif_hwrtdata_common *hwrt_data_common_fw;
	int err;

	/*
	 * Create and map the FW structure so we can initialise it. This is not
	 * accessed on the CPU side post-initialisation so the mapping lifetime
	 * is only for this function.
	 */
	hwrt_data_common_fw =
		pvr_gem_create_and_map_fw_object(pvr_dev,
						 sizeof(*hwrt_data_common_fw),
						 PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
						 DRM_PVR_BO_CREATE_ZEROED, &hwrt->common_fw_obj);
	if (IS_ERR(hwrt_data_common_fw)) {
		err = PTR_ERR(hwrt_data_common_fw);
		goto err_out;
	}

	hwrt_data_common_fw->geom_caches_need_zeroing = false;
	hwrt_data_common_fw->screen_pixel_max = args->screen_pixel_max;
	hwrt_data_common_fw->multi_sample_ctl = args->multi_sample_control;
	hwrt_data_common_fw->flipped_multi_sample_ctl =
		args->flipped_multi_sample_control;
	hwrt_data_common_fw->tpc_stride = args->tpc_stride;
	hwrt_data_common_fw->tpc_size = args->tpc_stride;
	hwrt_data_common_fw->te_screen = args->te_screen_size;
	hwrt_data_common_fw->mtile_stride = args->mtile_stride;
	hwrt_data_common_fw->teaa = args->te_aa;
	hwrt_data_common_fw->te_mtile1 = args->te_mtile[0];
	hwrt_data_common_fw->te_mtile2 = args->te_mtile[1];
	hwrt_data_common_fw->isp_merge_lower_x = args->isp_merge_lower_x;
	hwrt_data_common_fw->isp_merge_lower_y = args->isp_merge_lower_y;
	hwrt_data_common_fw->isp_merge_upper_x = args->isp_merge_upper_x;
	hwrt_data_common_fw->isp_mergy_upper_y = args->isp_merge_upper_y;
	hwrt_data_common_fw->isp_merge_scale_x = args->isp_merge_scale_x;
	hwrt_data_common_fw->isp_merge_scale_y = args->isp_merge_scale_y;
	hwrt_data_common_fw->rgn_header_size = args->region_header_size;
	hwrt_data_common_fw->isp_mtile_size = args->isp_mtile_size;

	pvr_fw_object_vunmap(hwrt->common_fw_obj, hwrt_data_common_fw, false);

	return 0;

err_out:
	return err;
}

static void
hwrt_fini_common_fw_structure(struct pvr_hwrt_dataset *hwrt)
{
	pvr_fw_object_release(hwrt->common_fw_obj);
}

static int
hwrt_data_init_fw_structure(struct pvr_file *pvr_file,
			    struct pvr_hwrt_dataset *hwrt,
			    struct drm_pvr_ioctl_create_hwrt_dataset_args *args,
			    struct create_hwrt_geom_data_args *geom_data_args,
			    struct create_hwrt_rt_data_args *rt_data_args,
			    struct pvr_hwrt_data *hwrt_data)
{
	struct pvr_device *pvr_dev = pvr_file->pvr_dev;
	struct rogue_fwif_hwrtdata *hwrt_data_fw;
	struct rogue_fwif_rta_ctl *rta_ctl;
	int free_list_i;
	int err;

	/*
	 * Create and map the FW structure so we can initialise it. This is not
	 * accessed on the CPU side post-initialisation so the mapping lifetime
	 * is only for this function.
	 */
	hwrt_data_fw = pvr_gem_create_and_map_fw_object(pvr_dev, sizeof(*hwrt_data_fw),
							PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
							DRM_PVR_BO_CREATE_ZEROED,
							&hwrt_data->fw_obj);
	if (IS_ERR(hwrt_data_fw)) {
		err = PTR_ERR(hwrt_data_fw);
		goto err_out;
	}

	pvr_gem_get_fw_addr(hwrt->common_fw_obj, &hwrt_data_fw->hwrt_data_common_fw_addr);

	/* MList Data Store */
	hwrt_data_fw->pm_mlist_dev_addr = rt_data_args->pm_mlist_dev_addr;

	for (free_list_i = 0; free_list_i < hwrt->num_free_lists;
	     free_list_i++) {
		pvr_gem_get_fw_addr(hwrt->free_lists[free_list_i]->fw_obj,
				    &hwrt_data_fw->freelists_fw_addr[free_list_i]);
	}

	hwrt_data_fw->vheap_table_dev_addr = geom_data_args->vheap_table_dev_addr;
	hwrt_data_fw->macrotile_array_dev_addr =
		rt_data_args->macrotile_array_dev_addr;
	hwrt_data_fw->rgn_header_dev_addr = rt_data_args->region_header_dev_addr;
	hwrt_data_fw->rtc_dev_addr = geom_data_args->rtc_dev_addr;
	hwrt_data_fw->tail_ptrs_dev_addr = geom_data_args->tail_ptrs_dev_addr;

	rta_ctl = &hwrt_data_fw->rta_ctl;

	rta_ctl->render_target_index = 0;
	rta_ctl->active_render_targets = 0;
	rta_ctl->valid_render_targets_fw_addr = 0;
	rta_ctl->rta_num_partial_renders_fw_addr = 0;
	rta_ctl->max_rts = args->max_rts;

	if (args->max_rts > 1) {
		err = pvr_gem_create_fw_object(pvr_dev, args->max_rts * SRTC_ENTRY_SIZE,
					       PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
					       DRM_PVR_BO_CREATE_ZEROED,
					       &hwrt_data->srtc_obj);
		if (err)
			goto err_put_fw_obj;
		pvr_gem_get_fw_addr(hwrt_data->srtc_obj, &rta_ctl->valid_render_targets_fw_addr);

		err = pvr_gem_create_fw_object(pvr_dev, args->max_rts * RAA_ENTRY_SIZE,
					       PVR_BO_FW_FLAGS_DEVICE_UNCACHED |
					       DRM_PVR_BO_CREATE_ZEROED,
					       &hwrt_data->raa_obj);
		if (err)
			goto err_put_shadow_rt_cache;
		pvr_gem_get_fw_addr(hwrt_data->raa_obj, &rta_ctl->rta_num_partial_renders_fw_addr);
	}

	pvr_fw_object_vunmap(hwrt_data->fw_obj, hwrt_data_fw, false);

	return 0;

err_put_shadow_rt_cache:
	pvr_fw_object_release(hwrt_data->srtc_obj);

err_put_fw_obj:
	pvr_fw_object_vunmap(hwrt_data->fw_obj, hwrt_data_fw, false);
	pvr_fw_object_release(hwrt_data->fw_obj);

err_out:
	return err;
}

static void
hwrt_data_fini_fw_structure(struct pvr_hwrt_dataset *hwrt, int hwrt_nr)
{
	struct pvr_hwrt_data *hwrt_data = &hwrt->data[hwrt_nr];

	if (hwrt->max_rts > 1) {
		pvr_fw_object_release(hwrt_data->raa_obj);
		pvr_fw_object_release(hwrt_data->srtc_obj);
	}

	pvr_fw_object_release(hwrt_data->fw_obj);
}

/**
 * pvr_hwrt_dataset_create() - Create a new HWRT dataset
 * @pvr_file: Pointer to pvr_file structure.
 * @args: Creation arguments from userspace.
 *
 * Return:
 *  * HWRT pointer on success, or
 *  * -%ENOMEM on out of memory.
 */
struct pvr_hwrt_dataset *
pvr_hwrt_dataset_create(struct pvr_file *pvr_file,
			struct drm_pvr_ioctl_create_hwrt_dataset_args *args)
{
	struct create_hwrt_geom_data_args *geom_data_args;
	struct create_hwrt_rt_data_args *rt_data_args;
	struct create_hwrt_free_list_args *free_list_args;
	struct pvr_hwrt_dataset *hwrt;
	int err;
	int i;

	if (args->num_geom_datas != ROGUE_FWIF_NUM_GEOMDATAS ||
	    args->num_rt_datas != ROGUE_FWIF_NUM_RTDATAS ||
	    args->num_free_lists != ROGUE_FWIF_NUM_RTDATA_FREELISTS) {
		err = -EINVAL;
		goto err_out;
	}

	if (args->_padding_7a || args->_padding_7c) {
		err = -EINVAL;
		goto err_out;
	}

	/* TODO: support multiple geom datas */
	BUILD_BUG_ON(ROGUE_FWIF_NUM_GEOMDATAS != 1);

	/* Copy geom / RT / free list structures from user space. */
	geom_data_args = kcalloc(args->num_geom_datas, sizeof(*geom_data_args),
				 GFP_KERNEL);
	rt_data_args =
		kcalloc(args->num_rt_datas, sizeof(*rt_data_args), GFP_KERNEL);
	free_list_args = kcalloc(args->num_free_lists, sizeof(*free_list_args),
				 GFP_KERNEL);
	if (!geom_data_args || !rt_data_args || !free_list_args) {
		err = -ENOMEM;
		goto err_free_args;
	}

	if (copy_from_user(geom_data_args,
			   u64_to_user_ptr(args->geom_data_args),
			   sizeof(*geom_data_args) * args->num_geom_datas)) {
		err = -EFAULT;
		goto err_free_args;
	}
	if (copy_from_user(rt_data_args, u64_to_user_ptr(args->rt_data_args),
			   sizeof(*rt_data_args) * args->num_rt_datas)) {
		err = -EFAULT;
		goto err_free_args;
	}
	if (copy_from_user(free_list_args,
			   u64_to_user_ptr(args->free_list_args),
			   sizeof(*free_list_args) * args->num_free_lists)) {
		err = -EFAULT;
		goto err_free_args;
	}

	/* Create and fill out the kernel structure */
	hwrt = kzalloc(sizeof(*hwrt), GFP_KERNEL);
	if (!hwrt) {
		err = -ENOMEM;
		goto err_free_args;
	}

	err = hwrt_init_kernel_structure(pvr_file, args, free_list_args, hwrt);
	if (err < 0)
		goto err_free_hwrt;

	err = hwrt_init_common_fw_structure(pvr_file, args, hwrt);
	if (err < 0)
		goto err_destroy_kernel_structure;

	for (i = 0; i < args->num_rt_datas; i++) {
		err = hwrt_data_init_fw_structure(pvr_file, hwrt, args,
						  geom_data_args,
						  &rt_data_args[i],
						  &hwrt->data[i]);
		if (err < 0) {
			i--;
			/* Destroy already created structures. */
			for (; i >= 0; i--)
				hwrt_data_fini_fw_structure(hwrt, i);
			goto err_destroy_common_fw_structure;
		}

		hwrt->data[i].hwrt_dataset = hwrt;
	}

	kfree(geom_data_args);
	kfree(rt_data_args);
	kfree(free_list_args);

	return hwrt;

err_destroy_common_fw_structure:
	hwrt_fini_common_fw_structure(hwrt);

err_destroy_kernel_structure:
	hwrt_fini_kernel_structure(hwrt);

err_free_hwrt:
	kfree(hwrt);

err_free_args:
	kfree(geom_data_args);
	kfree(rt_data_args);
	kfree(free_list_args);

err_out:
	return ERR_PTR(err);
}

/**
 * pvr_hwrt_dataset_destroy() - Destroy a HWRT data set
 * @hwrt: HWRT pointer
 *
 * This should not be called directly. HWRT references should be dropped via pvr_hwrt_dataset_put().
 */
void
pvr_hwrt_dataset_destroy(struct pvr_hwrt_dataset *hwrt)
{
	struct pvr_device *pvr_dev = hwrt->pvr_dev;
	int i;

	for (i = hwrt->num_rt_datas - 1; i >= 0; i--) {
		WARN_ON(pvr_object_cleanup(pvr_dev, ROGUE_FWIF_CLEANUP_HWRTDATA,
					   hwrt->data[i].fw_obj, 0));
		hwrt_data_fini_fw_structure(hwrt, i);
	}

	hwrt_fini_common_fw_structure(hwrt);
	hwrt_fini_kernel_structure(hwrt);

	kfree(hwrt);
}
