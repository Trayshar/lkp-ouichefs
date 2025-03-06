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
#include <linux/time64.h>

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
#define OUICHEFS_MAX_SNAPSHOTS 32

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
 * | idfree bitmap |  sb->nr_idfree_blocks blocks
 * +---------------+
 * | id_idx blocks |  sb->nr_ididx_blocks blocks
 * +---------------+
 * |  meta blocks  |  sb->nr_meta_blocks blocks
 * +---------------+
 * |    data       |
 * |      blocks   |  rest of the blocks
 * +---------------+
 *
 */


/* Actual inode data */
struct ouichefs_inode_data {
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
	uint32_t index_block; /* Index block / dir block of this inode */
	ouichefs_snap_index_t refcount; /* How many inodes link to this */
};

/* Stored in the id_idx region. Links inode data entry numbers to a block. */
struct ouichefs_inode_data_index_block {
	uint32_t blocks[OUICHEFS_INDEX_BLOCK_LEN];
};

/*
 * Inodes are saved in the 'inode store' region. They are just a mapping between
 * snapshots and actual inode data. This data lives in the data blocks and is
 * indexed by yet another number. "id_idx" blocks are used to map this number
 * to the actual data block holding the inode data.
 */
struct ouichefs_inode {
	uint32_t i_data[OUICHEFS_MAX_SNAPSHOTS];
};

/* In-memory layout of our inodes */
struct ouichefs_inode_info {
	uint32_t index_block;
	struct inode vfs_inode;
};

#define OUICHEFS_INODES_PER_BLOCK \
	(OUICHEFS_BLOCK_SIZE / sizeof(struct ouichefs_inode))
#define OUICHEFS_IDE_PER_DATA_BLOCK \
	(OUICHEFS_BLOCK_SIZE / sizeof(struct ouichefs_inode_data))
#define OUICHEFS_IDE_PER_INDEX_BLOCK \
	(OUICHEFS_IDE_PER_DATA_BLOCK * OUICHEFS_INDEX_BLOCK_LEN)

struct ouichefs_snapshot_info {
	time64_t created; /* Creation time (sec) */
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

	uint32_t nr_inode_data_entries; /* Maximal number of inode data entries */
	uint32_t nr_free_inode_data_entries; /* Number of free inode data entries */
	uint32_t nr_idfree_blocks; /* Number of inode data entry free bitmap blocks */
	uint32_t nr_ididx_blocks; /* Number of inode data index blocks */
	uint32_t nr_meta_blocks; /* Number of metadata blocks */

	/* List of all snapshots. */
	struct ouichefs_snapshot_info snapshots[OUICHEFS_MAX_SNAPSHOTS];

	/* THESE MUST ALWAYS BE LAST */
	unsigned long *ifree_bitmap; /* In-memory free inodes bitmap */
	unsigned long *bfree_bitmap; /* In-memory free blocks bitmap */
	unsigned long *idfree_bitmap; /* In-memory free blocks bitmap */
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

enum ouichefs_datablock_type {
	OUICHEFS_DATA,        /* raw file data */
	OUICHEFS_INDEX,       /* struct ouichefs_file_index_block */
	OUICHEFS_DIR,         /* struct ouichefs_dir_block */
	OUICHEFS_INODE_DATA,  /* list of struct ouichefs_inode_data */
};

/* superblock functions */
int ouichefs_fill_super(struct super_block *sb, void *data, int silent);

/* inode functions */
int ouichefs_init_inode_cache(void);
void ouichefs_destroy_inode_cache(void);
struct inode *ouichefs_iget(struct super_block *sb, uint32_t ino, bool create);
int ouichefs_ifill(struct inode *inode, bool create);

/* inode data functions */
struct ouichefs_inode_data *ouichefs_get_inode_data(struct super_block *sb,
						    struct buffer_head **id_bh,
						    uint32_t ino, bool allocate,
						    bool cow);
int ouichefs_link_inode_data(struct super_block *sb, uint32_t ino,
			     struct ouichefs_inode *inode,
			     ouichefs_snap_index_t from,
			     ouichefs_snap_index_t to);
void ouichefs_put_inode_data(struct super_block *sb, uint32_t ino,
			     struct ouichefs_inode *inode,
			     ouichefs_snap_index_t snapshot);
/* data block functions */
int ouichefs_alloc_block(struct super_block *sb, uint32_t *bno);
int ouichefs_cow_block(struct super_block *sb, uint32_t *bno,
		       enum ouichefs_datablock_type b_type);
int ouichefs_get_block(struct super_block *sb, uint32_t bno);
void ouichefs_put_block(struct super_block *sb, uint32_t bno,
			enum ouichefs_datablock_type b_type);

/* snapshot functions */
int ouichefs_snapshot_create(struct super_block *sb, ouichefs_snap_id_t s_id);
int ouichefs_snapshot_delete(struct super_block *sb, ouichefs_snap_id_t s_id);
int ouichefs_snapshot_list(struct super_block *sb, char *buf);
int ouichefs_snapshot_restore(struct super_block *sb, ouichefs_snap_id_t s_id);

/* sysfs interface function */
int create_ouichefs_partition_entry(const char *dev_name, struct super_block *sb);
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
static_assert(sizeof(struct ouichefs_inode_data_index_block) <= OUICHEFS_BLOCK_SIZE,
			"ouichefs_inode_data_index_block is bigger than a block!");
static_assert(sizeof(struct ouichefs_dir_block) <= OUICHEFS_BLOCK_SIZE,
			"ouichefs_dir_block is bigger than a block!");
static_assert(sizeof(struct ouichefs_inode) <= OUICHEFS_BLOCK_SIZE,
			"ouichefs_inode is bigger than a block!");
static_assert(OUICHEFS_MAX_SNAPSHOTS <= (1l << 8 * sizeof(ouichefs_snap_index_t)),
			"type ouichefs_snap_index_t cannot fit OUICHEFS_MAX_SNAPSHOTS!");
static_assert(OUICHEFS_MAX_FILESIZE >= (1l << 22),
			"OUICHEFS_MAX_FILESIZE is smaller than 4MB!");

/*
 * File system layout helpers to ease accessing the various blocks and regions
 * of ouichefs. If you change the layout of the file system, update this.
 */

/*
 * Inodes are indexed linearly by their number (ino). Multiple inodes live in
 * the same physical block in the 'inode store' region.
 */
 #define OUICHEFS_GET_INODE_BLOCK(ino) \
	(1 + (ino / ((uint32_t) OUICHEFS_INODES_PER_BLOCK)))
#define OUICHEFS_GET_INODE_SHIFT(ino) \
	(ino % OUICHEFS_INODES_PER_BLOCK)

/*
 * Ouichefs uses various bitmaps to manage free indices and blocks.
 */
#define OUICHEFS_GET_IFREE_START(sbi) \
	(1 + sbi->nr_istore_blocks)
#define OUICHEFS_GET_BFREE_START(sbi) \
	(1 + sbi->nr_istore_blocks + sbi->nr_ifree_blocks)
#define OUICHEFS_GET_IDFREE_START(sbi) \
	(OUICHEFS_GET_BFREE_START(sbi) + sbi->nr_bfree_blocks)

/*
 * Similar to inodes, inode data is index by a simple number (idx).
 * Since inode data is discontinuously stored in the data region and each inode
 * data blocks holds multiple inode data entries, an additional index table
 * is needed to map each index number (idx) to some spot in a data block.
 * Similarly, each index block can hold multiple mappings.
 */
#define OUICHEFS_GET_IDIDX_BLOCK(sbi, idx) (\
	OUICHEFS_GET_IDFREE_START(sbi) + sbi->nr_idfree_blocks + \
	(idx / ((uint32_t) OUICHEFS_IDE_PER_INDEX_BLOCK)) \
)
#define OUICHEFS_GET_IDIDX_INDEX(sbi, idx) (\
	(idx % OUICHEFS_IDE_PER_INDEX_BLOCK) / \
	((uint32_t) OUICHEFS_IDE_PER_DATA_BLOCK) \
)
#define OUICHEFS_GET_IDIDX_SHIFT(sbi, idx) (\
	(idx % OUICHEFS_IDE_PER_INDEX_BLOCK) % OUICHEFS_IDE_PER_DATA_BLOCK \
)

/*
 * Data blocks hold data of many different formats. Each data block supports
 * Copy-on-Write, and hence each block has a reference counter associated with
 * it - this counter is stored in the metadata blocks.
 */
#define OUICHEFS_GET_DATA_START(sbi) \
	(OUICHEFS_GET_IDIDX_BLOCK(sbi, 0) + sbi->nr_ididx_blocks + sbi->nr_meta_blocks)
/* Get metadata block for data block */
#define OUICHEFS_GET_META_BLOCK(bno, sbi) \
	(OUICHEFS_GET_IDIDX_BLOCK(sbi, 0) + sbi->nr_ididx_blocks + \
	((bno - OUICHEFS_GET_DATA_START(sbi)) / ((uint32_t) OUICHEFS_META_BLOCK_LEN)))
/* Offset inside the metadata block */
#define OUICHEFS_GET_META_SHIFT(bno) \
	((bno - OUICHEFS_GET_DATA_START(sbi)) % OUICHEFS_META_BLOCK_LEN)

#endif /* _OUICHEFS_H */
