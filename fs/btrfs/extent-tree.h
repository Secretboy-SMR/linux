/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_EXTENT_TREE_H
#define BTRFS_EXTENT_TREE_H

#include <linux/types.h>
#include "block-group.h"
#include "locking.h"

struct extent_buffer;
struct btrfs_free_cluster;
struct btrfs_fs_info;
struct btrfs_root;
struct btrfs_path;
struct btrfs_ref;
struct btrfs_disk_key;
struct btrfs_delayed_ref_head;
struct btrfs_delayed_ref_root;
struct btrfs_extent_inline_ref;

enum btrfs_extent_allocation_policy {
	BTRFS_EXTENT_ALLOC_CLUSTERED,
	BTRFS_EXTENT_ALLOC_ZONED,
};

struct find_free_extent_ctl {
	/* Basic allocation info */
	u64 ram_bytes;
	u64 num_bytes;
	u64 min_alloc_size;
	u64 empty_size;
	u64 flags;
	int delalloc;

	/* Where to start the search inside the bg */
	u64 search_start;

	/* For clustered allocation */
	u64 empty_cluster;
	struct btrfs_free_cluster *last_ptr;
	bool use_cluster;

	bool have_caching_bg;
	bool orig_have_caching_bg;

	/* Allocation is called for tree-log */
	bool for_treelog;

	/* Allocation is called for data relocation */
	bool for_data_reloc;

	/* RAID index, converted from flags */
	int index;

	/*
	 * Current loop number, check find_free_extent_update_loop() for details
	 */
	int loop;

	/*
	 * Set to true if we're retrying the allocation on this block group
	 * after waiting for caching progress, this is so that we retry only
	 * once before moving on to another block group.
	 */
	bool retry_uncached;

	/* If current block group is cached */
	int cached;

	/* Max contiguous hole found */
	u64 max_extent_size;

	/* Total free space from free space cache, not always contiguous */
	u64 total_free_space;

	/* Found result */
	u64 found_offset;

	/* Hint where to start looking for an empty space */
	u64 hint_byte;

	/* Allocation policy */
	enum btrfs_extent_allocation_policy policy;

	/* Whether or not the allocator is currently following a hint */
	bool hinted;

	/* Size class of block groups to prefer in early loops */
	enum btrfs_block_group_size_class size_class;
};

enum btrfs_inline_ref_type {
	BTRFS_REF_TYPE_INVALID,
	BTRFS_REF_TYPE_BLOCK,
	BTRFS_REF_TYPE_DATA,
	BTRFS_REF_TYPE_ANY,
};

int btrfs_get_extent_inline_ref_type(const struct extent_buffer *eb,
				     const struct btrfs_extent_inline_ref *iref,
				     enum btrfs_inline_ref_type is_data);
u64 hash_extent_data_ref(u64 root_objectid, u64 owner, u64 offset);

int btrfs_run_delayed_refs(struct btrfs_trans_handle *trans, u64 min_bytes);
u64 btrfs_cleanup_ref_head_accounting(struct btrfs_fs_info *fs_info,
				  struct btrfs_delayed_ref_root *delayed_refs,
				  struct btrfs_delayed_ref_head *head);
int btrfs_lookup_data_extent(struct btrfs_fs_info *fs_info, u64 start, u64 len);
int btrfs_lookup_extent_info(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info, u64 bytenr,
			     u64 offset, int metadata, u64 *refs, u64 *flags,
			     u64 *owner_root);
int btrfs_pin_extent(struct btrfs_trans_handle *trans, u64 bytenr, u64 num,
		     int reserved);
int btrfs_pin_extent_for_log_replay(struct btrfs_trans_handle *trans,
				    const struct extent_buffer *eb);
int btrfs_exclude_logged_extents(struct extent_buffer *eb);
int btrfs_cross_ref_exist(struct btrfs_inode *inode, u64 offset, u64 bytenr,
			  struct btrfs_path *path);
struct extent_buffer *btrfs_alloc_tree_block(struct btrfs_trans_handle *trans,
					     struct btrfs_root *root,
					     u64 parent, u64 root_objectid,
					     const struct btrfs_disk_key *key,
					     int level, u64 hint,
					     u64 empty_size,
					     u64 reloc_src_root,
					     enum btrfs_lock_nesting nest);
int btrfs_free_tree_block(struct btrfs_trans_handle *trans,
			  u64 root_id,
			  struct extent_buffer *buf,
			  u64 parent, int last_ref);
int btrfs_alloc_reserved_file_extent(struct btrfs_trans_handle *trans,
				     struct btrfs_root *root, u64 owner,
				     u64 offset, u64 ram_bytes,
				     struct btrfs_key *ins);
int btrfs_alloc_logged_file_extent(struct btrfs_trans_handle *trans,
				   u64 root_objectid, u64 owner, u64 offset,
				   struct btrfs_key *ins);
int btrfs_reserve_extent(struct btrfs_root *root, u64 ram_bytes, u64 num_bytes,
			 u64 min_alloc_size, u64 empty_size, u64 hint_byte,
			 struct btrfs_key *ins, int is_data, int delalloc);
int btrfs_inc_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct extent_buffer *buf, int full_backref);
int btrfs_dec_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct extent_buffer *buf, int full_backref);
int btrfs_set_disk_extent_flags(struct btrfs_trans_handle *trans,
				struct extent_buffer *eb, u64 flags);
int btrfs_free_extent(struct btrfs_trans_handle *trans, struct btrfs_ref *ref);

u64 btrfs_get_extent_owner_root(struct btrfs_fs_info *fs_info,
				struct extent_buffer *leaf, int slot);
int btrfs_free_reserved_extent(struct btrfs_fs_info *fs_info, u64 start, u64 len,
			       bool is_delalloc);
int btrfs_pin_reserved_extent(struct btrfs_trans_handle *trans,
			      const struct extent_buffer *eb);
int btrfs_finish_extent_commit(struct btrfs_trans_handle *trans);
int btrfs_inc_extent_ref(struct btrfs_trans_handle *trans, struct btrfs_ref *generic_ref);
int btrfs_drop_snapshot(struct btrfs_root *root, int update_ref,
				     int for_reloc);
int btrfs_drop_subtree(struct btrfs_trans_handle *trans,
			struct btrfs_root *root,
			struct extent_buffer *node,
			struct extent_buffer *parent);
void btrfs_error_unpin_extent_range(struct btrfs_fs_info *fs_info, u64 start, u64 end);
int btrfs_discard_extent(struct btrfs_fs_info *fs_info, u64 bytenr,
			 u64 num_bytes, u64 *actual_bytes);
int btrfs_trim_fs(struct btrfs_fs_info *fs_info, struct fstrim_range *range);

#endif
