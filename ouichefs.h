/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */
#ifndef _OUICHEFS_H
#define _OUICHEFS_H

#include <linux/build_bug.h>
#include <linux/fs.h>

// TYPE DEFINITIONS: Makes it easier to update code if we want to adjust the size of some fields
#define ouichefs_snap_id_t uint32_t   /* Unique ID of a snapshot */
#define ouichefs_snap_index_t uint8_t /* Data type large enough to index OUICHEFS_MAX_SNAPSHOTS */
// All out internal block addresses are uint32_t

// MAGIC values: Change at will - but be careful
#define OUICHEFS_MAGIC 0x48434957
#define OUICHEFS_SB_BLOCK_NR 0
#define OUICHEFS_BLOCK_SIZE (1 << 12) /* 4 KiB */

// DERIVATIVE values; They are computed from/constraint by the layout and magic values
/* num. of data blocks a single index block can reference */
#define OUICHEFS_INDEX_BLOCK_LEN (OUICHEFS_BLOCK_SIZE / sizeof(uint32_t))
/* num. of blocks a single meta block stores data for */
#define OUICHEFS_META_BLOCK_LEN \
	(OUICHEFS_BLOCK_SIZE / sizeof(ouichefs_snap_index_t))
#define OUICHEFS_MAX_FILESIZE (OUICHEFS_INDEX_BLOCK_LEN * OUICHEFS_BLOCK_SIZE)
#define OUICHEFS_FILENAME_LEN 28 /* max. character length of a filename */
#define OUICHEFS_MAX_SUBFILES 128 /* How many files a directory can hold */
/* Maximal number of CONCURRENTLY existing snapshots */
#define OUICHEFS_MAX_SNAPSHOTS 128

/*
 * ouiche_fs partition layout
 *
 * +---------------+
 * |  superblock   |  1 block
 * +---------------+
 * |  inode store  |  sb->nr_istore_blocks blocks
 * +---------------+
 * | ifree bitmap  |  sb->nr_ifree_blocks blocks
 * +---------------+
 * | bfree bitmap  |  sb->nr_bfree_blocks blocks
 * +---------------+
 * |  meta blocks  |  sb->nr_meta_blocks blocks
 * +---------------+
 * |    data       |
 * |      blocks   |  rest of the blocks
 * +---------------+
 *
 */

struct ouichefs_inode {
	uint32_t i_mode; /* File mode */
	uint32_t i_uid; /* Owner id */
	uint32_t i_gid; /* Group id */
	uint32_t i_size; /* Size in bytes */
	uint32_t i_ctime; /* Inode change time (sec)*/
	uint64_t i_nctime; /* Inode change time (nsec) */
	uint32_t i_atime; /* Access time (sec) */
	uint64_t i_natime; /* Access time (nsec) */
	uint32_t i_mtime; /* Modification time (sec) */
	uint64_t i_nmtime; /* Modification time (nsec) */
	uint32_t i_blocks; /* Block count */
	uint32_t i_nlink; /* Hard links count */
	uint32_t index_block; /* Block with list of blocks for this file */
};

struct ouichefs_inode_info {
	uint32_t index_block;
	struct inode vfs_inode;
};

#define OUICHEFS_INODES_PER_BLOCK \
	(OUICHEFS_BLOCK_SIZE / sizeof(struct ouichefs_inode))

struct ouichefs_snapshot_info {
	uint64_t created; /* Creation time (sec) */
	uint32_t root_inode; /* Address of this snapshots root inode */
	ouichefs_snap_id_t id; /* Unique identifier of this snapshot */
};

struct ouichefs_sb_info {
	uint32_t magic; /* Magic number */

	uint32_t nr_blocks; /* Total number of blocks (incl sb & inodes) */
	uint32_t nr_inodes; /* Total number of inodes */

	uint32_t nr_istore_blocks; /* Number of inode store blocks */
	uint32_t nr_ifree_blocks; /* Number of inode free bitmap blocks */
	uint32_t nr_bfree_blocks; /* Number of block free bitmap blocks */

	uint32_t nr_free_inodes; /* Number of free inodes */
	uint32_t nr_free_blocks; /* Number of free blocks */

	uint32_t nr_meta_blocks; /* Number of metadata blocks */

	/* Next available ID for snapshots */
	ouichefs_snap_id_t next_snapshot_id;
	/* List of all snapshots. TODO: Ordered by id maybe? */
	struct ouichefs_snapshot_info snapshots[OUICHEFS_MAX_SNAPSHOTS];
	/* Index in snapshots array of currently used snapshot */
	ouichefs_snap_index_t current_snapshot_index;

	/* THESE MUST ALWAYS BE LAST */
	unsigned long *ifree_bitmap; /* In-memory free inodes bitmap */
	unsigned long *bfree_bitmap; /* In-memory free blocks bitmap */
};

struct ouichefs_metadata_block {
	/* One reference counter for each block */
	ouichefs_snap_index_t refcount[OUICHEFS_META_BLOCK_LEN];
};

struct ouichefs_file_index_block {
	uint32_t blocks[OUICHEFS_INDEX_BLOCK_LEN];
};

struct ouichefs_dir_block {
	struct ouichefs_file {
		uint32_t inode;
		char filename[OUICHEFS_FILENAME_LEN];
	} files[OUICHEFS_MAX_SUBFILES];
};

/* superblock functions */
int ouichefs_fill_super(struct super_block *sb, void *data, int silent);

/* inode functions */
int ouichefs_init_inode_cache(void);
void ouichefs_destroy_inode_cache(void);
struct inode *ouichefs_iget(struct super_block *sb, unsigned long ino);

/* data block functions */
int ouichefs_alloc_block(struct super_block *sb, uint32_t *bno);
int ouichefs_get_block(struct super_block *sb, uint32_t bno);
void ouichefs_put_block(struct super_block *sb, uint32_t bno,
	bool is_index_block);

/* snapshot functions */
int create_ouichefs_partition_entry(const char *dev_name);
void remove_ouichefs_partition_entry(const char *dev_name);
int init_sysfs_interface(void);
void cleanup_sysfs_interface(void);

/* file functions */
extern const struct file_operations ouichefs_file_ops;
extern const struct file_operations ouichefs_dir_ops;
extern const struct address_space_operations ouichefs_aops;

/* Getters for superblock and inode */
#define OUICHEFS_SB(sb) (sb->s_fs_info)
#define OUICHEFS_INODE(inode) \
	(container_of(inode, struct ouichefs_inode_info, vfs_inode))

// Do some compile-time sanity checks
static_assert(sizeof(struct ouichefs_metadata_block) <= OUICHEFS_BLOCK_SIZE,
			"ouichefs_metadata_block is bigger than a block!");
static_assert(sizeof(struct ouichefs_sb_info) <= OUICHEFS_BLOCK_SIZE,
			"ouichefs_sb_info is bigger than a block!");
static_assert(sizeof(struct ouichefs_file_index_block) <= OUICHEFS_BLOCK_SIZE,
			"ouichefs_file_index_block is bigger than a block!");
static_assert(sizeof(struct ouichefs_dir_block) <= OUICHEFS_BLOCK_SIZE,
			"ouichefs_dir_block is bigger than a block!");
static_assert(sizeof(struct ouichefs_inode) <= OUICHEFS_BLOCK_SIZE,
			"ouichefs_inode is bigger than a block!");
static_assert(OUICHEFS_MAX_SNAPSHOTS <= (1l << 8 * sizeof(ouichefs_snap_index_t)),
			"type ouichefs_snap_index_t cannot fit OUICHEFS_MAX_SNAPSHOTS!");
static_assert(OUICHEFS_MAX_FILESIZE >= (1l << 22),
			"OUICHEFS_MAX_FILESIZE is smaller than 4MB!");

// LAYOUT HELPERS: defines some "index <> block" helpers that depend on FS layout
/* Get inode block for inode */
#define OUICHEFS_GET_INODE_BLOCK(ino) \
	(1 + (ino / ((uint32_t) OUICHEFS_INODES_PER_BLOCK)))
/* Offset inside the inode block */
#define OUICHEFS_GET_INODE_SHIFT(ino) \
	(ino % OUICHEFS_INODES_PER_BLOCK)
#define OUICHEFS_GET_IFREE_START(sbi) \
	(1 + sbi->nr_istore_blocks)
#define OUICHEFS_GET_BFREE_START(sbi) \
	(1 + sbi->nr_istore_blocks + sbi->nr_ifree_blocks)
#define OUICHEFS_GET_DATA_START(sbi) \
	(OUICHEFS_GET_BFREE_START(sbi) + sbi->nr_bfree_blocks + sbi->nr_meta_blocks)
/* Get metadata block for data block */
#define OUICHEFS_GET_META_BLOCK(bno, sbi) \
	(OUICHEFS_GET_BFREE_START(sbi) + sbi->nr_bfree_blocks + \
	((bno - OUICHEFS_GET_DATA_START(sbi)) / ((uint32_t) OUICHEFS_META_BLOCK_LEN)))
/* Offset inside the metadata block */
#define OUICHEFS_GET_META_SHIFT(bno) \
	((bno - OUICHEFS_GET_DATA_START(sbi)) % OUICHEFS_META_BLOCK_LEN)

#endif /* _OUICHEFS_H */
