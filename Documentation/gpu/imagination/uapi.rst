====
UAPI
====
The sources associated with this section can be found in ``pvr_drm.h``.

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: PowerVR UAPI


IOCTLS
======
.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: IOCTLS

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: PVR_IOCTL

CREATE_BO
---------
.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: IOCTL CREATE_BO

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ioctl_create_bo_args

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: Flags for CREATE_BO

GET_BO_MMAP_OFFSET
------------------
.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: IOCTL GET_BO_MMAP_OFFSET

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ioctl_get_bo_mmap_offset_args

GET_PARAM
---------
.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: IOCTL GET_PARAM

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ioctl_get_param_args

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_param

CREATE_CONTEXT
--------------
.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: IOCTL CREATE_CONTEXT

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ioctl_create_context_args

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ctx_priority
                 drm_pvr_ctx_type
                 drm_pvr_static_render_context_state
                 drm_pvr_static_render_context_state_format
                 drm_pvr_reset_framework
                 drm_pvr_reset_framework_format

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: Flags for CREATE_CONTEXT

DESTROY_CONTEXT
---------------
.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: IOCTL DESTROY_CONTEXT

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ioctl_destroy_context_args

CREATE_OBJECT
-------------
.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: IOCTL CREATE_OBJECT

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ioctl_create_object_args

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_object_type
                 drm_pvr_ioctl_create_free_list_args
                 create_hwrt_geom_data_args
                 create_hwrt_rt_data_args
                 create_hwrt_free_list_args
                 drm_pvr_ioctl_create_hwrt_dataset_args

DESTROY_OBJECT
--------------
.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: IOCTL DESTROY_OBJECT

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ioctl_destroy_object_args

GET_HEAP_INFO
-------------
.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: IOCTL GET_HEAP_INFO

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ioctl_get_heap_info_args

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_heap_id

VM_OP
-----
.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: IOCTL VM_OP

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ioctl_vm_op_args

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :identifiers: drm_pvr_ioctl_vm_op_map_args drm_pvr_ioctl_vm_op_unmap_args

.. kernel-doc:: include/uapi/drm/pvr_drm.h
   :doc: Flags for VM_OP

Internal notes
==============
.. kernel-doc:: include/gpu/drm/imagination/pvr_device.h
   :doc: IOCTL validation helpers

.. kernel-doc:: drivers/gpu/drm/imagination/pvr_device.h
   :identifiers: PVR_STATIC_ASSERT_64BIT_ALIGNED PVR_IOCTL_UNION_PADDING_CHECK
                 pvr_ioctl_union_padding_check
