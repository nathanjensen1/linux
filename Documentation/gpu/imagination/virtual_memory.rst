===========================
GPU Virtual Memory Handling
===========================
The sources associated with this section can be found in ``pvr_vm.c`` and
``pvr_vm.h``.

.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: PowerVR Virtual Memory Handling


Public API
==========
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.h
   :doc: Public API

Types
-----
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.h
   :identifiers: pvr_vm_page_options

Functions
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_vm_create_context pvr_vm_destroy_context
                 pvr_vm_map pvr_vm_map_partial pvr_vm_unmap

Helper functions
----------------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_device_addr_is_valid

Constants
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.h
   :doc: Public API (constants)


VM backing pages
================
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: VM backing pages

Types
-----
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_vm_backing_page

Functions
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_vm_backing_page_init pvr_vm_backing_page_fini
                 pvr_vm_backing_page_sync

Constants
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: VM backing pages (constants)


Raw page tables
===============
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: Raw page tables

Types
-----
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_page_table_l2_entry_raw pvr_page_table_l1_entry_raw
                 pvr_page_table_l0_entry_raw
                 pvr_page_table_l2_raw pvr_page_table_l1_raw
                 pvr_page_table_l0_raw

Functions
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_page_table_l2_entry_raw_is_valid
                 pvr_page_table_l2_entry_raw_set
                 pvr_page_table_l2_entry_raw_clear
                 pvr_page_table_l1_entry_raw_is_valid
                 pvr_page_table_l1_entry_raw_set
                 pvr_page_table_l1_entry_raw_clear
                 pvr_page_table_l0_entry_raw_is_valid
                 pvr_page_table_l0_entry_raw_set
                 pvr_page_table_l0_entry_raw_clear


Mirror page tables
==================
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: Mirror page tables

Types
-----
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_page_table_l2 pvr_page_table_l1 pvr_page_table_l0

Functions
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_page_table_l2_init pvr_page_table_l2_fini
                 pvr_page_table_l2_sync pvr_page_table_l2_get_raw
                 pvr_page_table_l2_get_entry_raw
                 pvr_page_table_l2_insert pvr_page_table_l2_remove
                 pvr_page_table_l1_init pvr_page_table_l1_fini
                 pvr_page_table_l1_sync pvr_page_table_l1_get_raw
                 pvr_page_table_l1_get_entry_raw
                 pvr_page_table_l1_insert pvr_page_table_l1_remove
                 pvr_page_table_l0_init pvr_page_table_l0_fini
                 pvr_page_table_l0_sync pvr_page_table_l0_get_raw
                 pvr_page_table_l0_get_entry_raw
                 pvr_page_table_l0_insert pvr_page_table_l0_remove


Page table index utilities
==========================
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: Page table index utilities

Functions
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_page_table_l2_idx pvr_page_table_l1_idx
                 pvr_page_table_l0_idx

Constants
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: Page table index utilities (constants)


High-level page table operations
================================
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: High-level page table operations

Functions
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_page_table_l1_create pvr_page_table_l1_get_or_create
                 pvr_page_table_l0_create pvr_page_table_l0_get_or_create
                 pvr_page_create pvr_page_destroy

Internal functions
------------------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_page_table_l1_create_unchecked __pvr_page_table_l1_destroy
                 pvr_page_table_l0_create_unchecked __pvr_page_table_l0_destroy


Page table pointer
==================
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: Page table pointer

Types
-----
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_page_table_ptr

Functions
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_page_table_ptr_init pvr_page_table_ptr_fini
                 pvr_page_table_ptr_next_page pvr_page_table_ptr_set
                 pvr_page_table_ptr_require_sync pvr_page_table_ptr_copy
                 pvr_page_table_ptr_sync pvr_page_table_ptr_sync_partial

Internal functions
------------------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_page_table_ptr_sync_manual pvr_page_table_ptr_load_tables

Constants
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: Page table pointer (constants)


Interval tree base implementation
=================================
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :doc: Interval tree base implementation

Types
-----
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_vm_interval_tree_node

Functions
---------
.. kernel-doc:: drivers/gpu/drm/imagination/pvr_vm.c
   :identifiers: pvr_vm_interval_tree_compute_last
                 pvr_vm_interval_tree_node_start pvr_vm_interval_tree_node_size
                 pvr_vm_interval_tree_node_last
                 pvr_vm_interval_tree_insert
