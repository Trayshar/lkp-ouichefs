#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <endian.h>
#include <string.h>

#define OUICHEFS_MAGIC 0x48434957

#define OUICHEFS_SB_BLOCK_NR 0

#define OUICHEFS_BLOCK_SIZE (1 << 12) /* 4 KiB */
#define OUICHEFS_MAX_FILESIZE (1 << 22) /* 4 MiB */
#define OUICHEFS_FILENAME_LEN 28
#define OUICHEFS_MAX_SUBFILES 128
#define OUICHEFS_MAX_SNAPSHOTS 32
#define OUICHEFS_META_BLOCK_LEN (OUICHEFS_BLOCK_SIZE / sizeof(uint8_t))
#define OUICHEFS_INDEX_BLOCK_LEN (OUICHEFS_BLOCK_SIZE / sizeof(uint32_t))

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
	uint8_t refcount; /* How many inodes link to this */
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

#define OUICHEFS_INODES_PER_BLOCK \
	(OUICHEFS_BLOCK_SIZE / sizeof(struct ouichefs_inode))
#define OUICHEFS_IDE_PER_DATA_BLOCK \
	(OUICHEFS_BLOCK_SIZE / sizeof(struct ouichefs_inode_data))
#define OUICHEFS_IDE_PER_INDEX_BLOCK \
	(OUICHEFS_IDE_PER_DATA_BLOCK * OUICHEFS_INDEX_BLOCK_LEN)

struct ouichefs_snapshot_info {
	int64_t m_time; /* Modification time (sec) */
	uint8_t id; /* Unique identifier of this snapshot */
};

struct ouichefs_superblock {
	uint32_t magic; /* Magic number */

	uint32_t nr_blocks; /* Total number of blocks (incl sb & inodes) */
	uint32_t nr_inodes; /* Total number of inodes */

	uint32_t nr_istore_blocks; /* Number of inode store blocks */
	uint32_t nr_ifree_blocks; /* Number of free inodes bitmask blocks */
	uint32_t nr_bfree_blocks; /* Number of free blocks bitmask blocks */

	uint32_t nr_free_inodes; /* Number of free inodes */
	uint32_t nr_free_blocks; /* Number of free blocks */

	uint32_t nr_inode_data_entries; /* Maximal number of inode data entries */
	uint32_t nr_free_inode_data_entries; /* Number of free inode data entries */
	uint32_t nr_idfree_blocks; /* Number of inode data free bitmap blocks */
	uint32_t nr_ididx_blocks; /* Number of inode data index blocks */
	uint32_t nr_meta_blocks; /* Number of metadata blocks */

	/* List of all snapshots */
	struct ouichefs_snapshot_info snapshots[OUICHEFS_MAX_SNAPSHOTS];
} __attribute__((aligned(OUICHEFS_BLOCK_SIZE)));
_Static_assert(sizeof(struct ouichefs_superblock) == OUICHEFS_BLOCK_SIZE,
	       "Superblock size mismatch");

struct ouichefs_metadata_block {
	/* One reference counter for each block */
	uint8_t refcount[OUICHEFS_META_BLOCK_LEN];
};
_Static_assert(sizeof(struct ouichefs_metadata_block) == OUICHEFS_BLOCK_SIZE,
	       "Metadata block size mismatch");

struct ouichefs_file_index_block {
	uint32_t blocks[OUICHEFS_BLOCK_SIZE >> 2];
};
_Static_assert(sizeof(struct ouichefs_file_index_block) == OUICHEFS_BLOCK_SIZE,
	       "Index block size mismatch");

struct ouichefs_dir_block {
	struct ouichefs_file {
		uint32_t inode;
		char filename[OUICHEFS_FILENAME_LEN];
	} files[OUICHEFS_MAX_SUBFILES];
};
_Static_assert(sizeof(struct ouichefs_dir_block) == OUICHEFS_BLOCK_SIZE,
	       "Dir block size mismatch");

static inline void usage(char *appname)
{
	fprintf(stderr,
		"Usage:\n"
		"%s disk\n",
		appname);
}

/* Returns ceil(a/b) */
static inline uint32_t idiv_ceil(uint32_t a, uint32_t b)
{
	uint32_t ret = a / b;
	if (a % b != 0)
		return ret + 1;
	return ret;
}

static struct ouichefs_superblock *write_superblock(int fd, struct stat *fstats)
{
	int ret;
	struct ouichefs_superblock *sb;
	uint32_t nr_inodes = 0, nr_blocks = 0, nr_inode_data_entries;
	uint32_t nr_ifree_blocks = 0, nr_bfree_blocks = 0, nr_idfree_blocks = 0;
	uint32_t nr_data_blocks = 0, nr_istore_blocks = 0, nr_ididx_blocks = 0;
	uint32_t nr_meta_blocks = 0, mod;

	sb = malloc(sizeof(struct ouichefs_superblock));
	if (!sb)
		return NULL;

	nr_blocks = fstats->st_size / OUICHEFS_BLOCK_SIZE;
	nr_inodes = nr_blocks;
	nr_inode_data_entries = nr_inodes * OUICHEFS_MAX_SNAPSHOTS;
	mod = nr_inodes % OUICHEFS_INODES_PER_BLOCK;
	if (mod != 0)
		nr_inodes += mod;
	nr_istore_blocks = idiv_ceil(nr_inodes, OUICHEFS_INODES_PER_BLOCK);
	nr_ifree_blocks = idiv_ceil(nr_inodes, OUICHEFS_BLOCK_SIZE * 8);
	nr_bfree_blocks = idiv_ceil(nr_blocks, OUICHEFS_BLOCK_SIZE * 8);
	nr_idfree_blocks = idiv_ceil(nr_inode_data_entries, OUICHEFS_BLOCK_SIZE * 8);
	nr_ididx_blocks = idiv_ceil(nr_inode_data_entries, OUICHEFS_IDE_PER_INDEX_BLOCK);

	nr_data_blocks = nr_blocks - 1 - nr_istore_blocks - nr_ifree_blocks -
			 nr_bfree_blocks - nr_idfree_blocks - nr_ididx_blocks;

	// Partition data blocks such that every data block has a metadata block
	// TODO: This leaves us with a bit more metadata blocks then we actually need
	nr_meta_blocks = idiv_ceil(nr_data_blocks, OUICHEFS_META_BLOCK_LEN + 1);
	nr_data_blocks -= nr_meta_blocks;

	memset(sb, 0, sizeof(struct ouichefs_superblock));
	sb->magic = htole32(OUICHEFS_MAGIC);
	sb->nr_blocks = htole32(nr_blocks);
	sb->nr_inodes = htole32(nr_inodes);
	sb->nr_inode_data_entries = htole32(nr_inode_data_entries);
	sb->nr_istore_blocks = htole32(nr_istore_blocks);
	sb->nr_ifree_blocks = htole32(nr_ifree_blocks);
	sb->nr_bfree_blocks = htole32(nr_bfree_blocks);
	sb->nr_idfree_blocks = htole32(nr_idfree_blocks);
	sb->nr_ididx_blocks = htole32(nr_ididx_blocks);
	sb->nr_meta_blocks = htole32(nr_meta_blocks);
	// The -1 are the root inode and the dir block it points to
	sb->nr_free_inodes = htole32(nr_inodes - 1);
	sb->nr_free_blocks = htole32(nr_data_blocks - 1);
	sb->nr_free_inode_data_entries = htole32(nr_inode_data_entries - 1);
	sb->snapshots[0].m_time = htole64(0);
	sb->snapshots[0].id = 0;

	ret = write(fd, sb, sizeof(struct ouichefs_superblock));
	if (ret != sizeof(struct ouichefs_superblock)) {
		free(sb);
		return NULL;
	}

	printf("Superblock: (%ld)\n"
	       "\tmagic=%#x\n"
	       "\tnr_blocks=%u\n"
	       "\tnr_inodes=%u (istore=%u blocks)\n"
	       "\tnr_inode_data_entries=%u (ididx=%u blocks)\n"
	       "\tnr_ifree_blocks=%u\n"
	       "\tnr_bfree_blocks=%u\n"
	       "\tnr_idfree_blocks=%u\n"
	       "\tnr_meta_blocks=%u\n"
	       "\tnr_free_inodes=%u\n"
	       "\tnr_free_blocks=%u\n"
	       "\tnr_free_inode_data_entries=%u\n",
	       sizeof(struct ouichefs_superblock), sb->magic, sb->nr_blocks,
	       sb->nr_inodes, sb->nr_istore_blocks,
	       sb->nr_inode_data_entries, sb->nr_ididx_blocks,
	       sb->nr_ifree_blocks, sb->nr_bfree_blocks, sb->nr_idfree_blocks,
	       sb->nr_meta_blocks, sb->nr_free_inodes, sb->nr_free_blocks,
	       sb->nr_free_inode_data_entries
	);

	return sb;
}

static int write_inode_store(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;
	uint32_t i;
	struct ouichefs_inode *inode;
	char *block;

	/* Allocate a zeroed block for inode store */
	block = malloc(OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;
	memset(block, 0, OUICHEFS_BLOCK_SIZE);

	/* Root inode (inode 1) points to first inode data entry (idx 1) */
	inode = (struct ouichefs_inode *)block + 1;
	inode->i_data[0] = 1;

	ret = write(fd, block, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	/* Reset inode store blocks to zero */
	memset(block, 0, OUICHEFS_BLOCK_SIZE);
	for (i = 1; i < sb->nr_istore_blocks; i++) {
		ret = write(fd, block, OUICHEFS_BLOCK_SIZE);
		if (ret != OUICHEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Inode store: wrote %d blocks (lseek %ld)\n"
	       "\tinode size = %ld B\n",
	       i, lseek(fd, 0, SEEK_CUR) / OUICHEFS_BLOCK_SIZE,
	       sizeof(struct ouichefs_inode));

end:
	free(block);
	return ret;
}

static int write_ifree_blocks(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;
	uint32_t i;
	char *block;
	uint64_t *ifree;

	block = malloc(OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;
	ifree = (uint64_t *)block;

	/* Set all bits to 1 */
	memset(ifree, 0xff, OUICHEFS_BLOCK_SIZE);

	/* First ifree block, containing first used inode */
	ifree[0] = htole64(0xfffffffffffffffc);
	ret = write(fd, ifree, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	/* All ifree blocks except the one containing 2 first inodes */
	ifree[0] = 0xffffffffffffffff;
	for (i = 1; i < le32toh(sb->nr_ifree_blocks); i++) {
		ret = write(fd, ifree, OUICHEFS_BLOCK_SIZE);
		if (ret != OUICHEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Ifree blocks: wrote %d blocks (lseek %ld)\n",
		i, lseek(fd, 0, SEEK_CUR) / OUICHEFS_BLOCK_SIZE);

end:
	free(block);

	return ret;
}

static int write_bfree_blocks(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;
	uint32_t i;
	char *block;
	uint64_t *bfree, mask, line;
	uint32_t nr_used = le32toh(sb->nr_istore_blocks) +
			   le32toh(sb->nr_ifree_blocks) +
			   le32toh(sb->nr_bfree_blocks) +
			   le32toh(sb->nr_idfree_blocks) +
			   le32toh(sb->nr_ididx_blocks) +
			   le32toh(sb->nr_meta_blocks) + 3;

	block = malloc(OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;
	bfree = (uint64_t *)block;

	/*
	 * First blocks (incl. sb + istore + ifree + bfree + meta + 1 used block)
	 * we suppose it won't go further than the first block
	 */
	memset(bfree, 0xff, OUICHEFS_BLOCK_SIZE);
	i = 0;
	while (nr_used) {
		line = 0xffffffffffffffff;
		for (mask = 0x1; mask != 0x0; mask <<= 1) {
			line &= ~mask;
			nr_used--;
			if (!nr_used)
				break;
		}
		bfree[i] = htole64(line);
		i++;
	}
	ret = write(fd, bfree, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	/* other blocks */
	memset(bfree, 0xff, OUICHEFS_BLOCK_SIZE);
	for (i = 1; i < le32toh(sb->nr_bfree_blocks); i++) {
		ret = write(fd, bfree, OUICHEFS_BLOCK_SIZE);
		if (ret != OUICHEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Bfree blocks: wrote %d blocks (lseek %ld)\n",
		i, lseek(fd, 0, SEEK_CUR) / OUICHEFS_BLOCK_SIZE);
end:
	free(block);

	return ret;
}

static int write_idfree_blocks(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;
	uint32_t i;
	char *block;
	uint64_t *idfree;

	block = malloc(OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;
	idfree = (uint64_t *)block;

	/* Set all bits to 1 */
	memset(idfree, 0xff, OUICHEFS_BLOCK_SIZE);

	/* First ifree block, containing first used inode */
	idfree[0] = htole64(0xfffffffffffffffc);
	ret = write(fd, idfree, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	/* All ifree blocks except the one containing 2 first inodes */
	idfree[0] = 0xffffffffffffffff;
	for (i = 1; i < le32toh(sb->nr_idfree_blocks); i++) {
		ret = write(fd, idfree, OUICHEFS_BLOCK_SIZE);
		if (ret != OUICHEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Idfree blocks: wrote %d blocks (lseek %ld)\n",
		i, lseek(fd, 0, SEEK_CUR) / OUICHEFS_BLOCK_SIZE);

end:
	free(block);

	return ret;
}


static int write_ididx_blocks(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0, i = 0;
	char *block;
	struct ouichefs_inode_data_index_block *ididx;
	uint32_t second_data_block = 2 + le32toh(sb->nr_istore_blocks) +
				     le32toh(sb->nr_bfree_blocks) +
				     le32toh(sb->nr_ifree_blocks) +
				     le32toh(sb->nr_idfree_blocks) +
				     le32toh(sb->nr_ididx_blocks) +
				     le32toh(sb->nr_meta_blocks);

	block = malloc(OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;
	memset(block, 0, OUICHEFS_BLOCK_SIZE);

	// First ididx block must link root inode (1) with idx 1,
	// which is in the 0th inode data block (at pos 1)
	ididx = (struct ouichefs_inode_data_index_block *) block;
	ididx->blocks[0] = second_data_block;
	ret = write(fd, block, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	// Write other blocks
	memset(block, 0, OUICHEFS_BLOCK_SIZE);
	for (i = 1; i < le32toh(sb->nr_ididx_blocks); i++) {
		ret = write(fd, block, OUICHEFS_BLOCK_SIZE);
		if (ret != OUICHEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Inode data index blocks: wrote %u blocks (lseek %ld)\n",
		i, lseek(fd, 0, SEEK_CUR) / OUICHEFS_BLOCK_SIZE);
end:
	free(block);

	return ret;
}

static int write_metadata_blocks(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0, i = 0;
	char *block;
	struct ouichefs_metadata_block *meta;

	block = malloc(OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;
	memset(block, 0, OUICHEFS_BLOCK_SIZE);

	// First metadata block must have the refcount counter set to 1
	// since the index block uses the first block as its dir block
	// The second block is used as it's inode_data
	meta = (struct ouichefs_metadata_block *) block;
	meta->refcount[0] = 1;
	meta->refcount[1] = 1;
	ret = write(fd, block, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}

	// Write other blocks
	memset(block, 0, OUICHEFS_BLOCK_SIZE);
	for (i = 1; i < le32toh(sb->nr_meta_blocks); i++) {
		ret = write(fd, block, OUICHEFS_BLOCK_SIZE);
		if (ret != OUICHEFS_BLOCK_SIZE) {
			ret = -1;
			goto end;
		}
	}
	ret = 0;

	printf("Metadata blocks: wrote %u blocks (lseek %ld)\n",
		i, lseek(fd, 0, SEEK_CUR) / OUICHEFS_BLOCK_SIZE);
end:
	free(block);

	return ret;
}

static int write_data_blocks(int fd, struct ouichefs_superblock *sb)
{
	int ret = 0;
	char *block;
	struct ouichefs_inode_data *idata;
	uint32_t first_data_block = 1 + le32toh(sb->nr_istore_blocks) +
					le32toh(sb->nr_bfree_blocks) +
					le32toh(sb->nr_ifree_blocks) +
					le32toh(sb->nr_idfree_blocks) +
					le32toh(sb->nr_ididx_blocks) +
					le32toh(sb->nr_meta_blocks);

	block = malloc(OUICHEFS_BLOCK_SIZE);
	if (!block)
		return -1;
	memset(block, 0, OUICHEFS_BLOCK_SIZE);

	// Write first data block; Its the dir_block for the root inode
	// and it is empty
	ret = write(fd, block, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}
	printf("Root index block: wrote 1 block\n");

	// Second data block is used as inode_data for root inode
	// Inside this block, the second slot (index 1) holds the actual data
	idata = (struct ouichefs_inode_data *) block;
	idata += 1;

	idata->i_mode =
		htole32(S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR |
			S_IWGRP | S_IXUSR | S_IXGRP | S_IXOTH);
	idata->i_uid = 0;
	idata->i_gid = 0;
	idata->i_size = htole32(OUICHEFS_BLOCK_SIZE);
	idata->i_ctime = idata->i_atime = idata->i_mtime = htole32(0);
	idata->i_nctime = idata->i_natime = idata->i_nmtime = htole64(0);
	idata->i_blocks = htole32(1);
	idata->i_nlink = htole32(2);
	idata->index_block = htole32(first_data_block);
	idata->refcount = 1;

	ret = write(fd, block, OUICHEFS_BLOCK_SIZE);
	if (ret != OUICHEFS_BLOCK_SIZE) {
		ret = -1;
		goto end;
	}
	printf("Inode data blocks: wrote 1 block (lseek %ld)\n",
		lseek(fd, 0, SEEK_CUR) / OUICHEFS_BLOCK_SIZE);
	ret = 0;
end:
	free(block);

	return ret;
}

int main(int argc, char **argv)
{
	int ret = EXIT_SUCCESS, fd;
	long int min_size;
	struct stat stat_buf;
	struct ouichefs_superblock *sb = NULL;

	if (argc != 2 || argv[1][0] == '-') {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	/* Open disk image */
	fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		perror("open():");
		return EXIT_FAILURE;
	}

	/* Get image size */
	ret = fstat(fd, &stat_buf);
	if (ret != 0) {
		perror("fstat():");
		ret = EXIT_FAILURE;
		goto fclose;
	}

	/* Check if image is large enough */
	min_size = 100 * OUICHEFS_BLOCK_SIZE;
	if (stat_buf.st_size < min_size) {
		fprintf(stderr,
			"File is not large enough (size=%ld, min size=%ld)\n",
			stat_buf.st_size, min_size);
		ret = EXIT_FAILURE;
		goto fclose;
	}

	/* Write superblock (block 0) */
	sb = write_superblock(fd, &stat_buf);
	if (!sb) {
		perror("write_superblock():");
		ret = EXIT_FAILURE;
		goto fclose;
	}

	/* Write inode store blocks (from block 1) */
	ret = write_inode_store(fd, sb);
	if (ret != 0) {
		perror("write_inode_store():");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write inode free bitmap blocks */
	ret = write_ifree_blocks(fd, sb);
	if (ret != 0) {
		perror("write_ifree_blocks()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write block free bitmap blocks */
	ret = write_bfree_blocks(fd, sb);
	if (ret != 0) {
		perror("write_bfree_blocks()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write inode data block free bitmap blocks */
	ret = write_idfree_blocks(fd, sb);
	if (ret != 0) {
		perror("write_idfree_blocks()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write inode index data blocks */
	ret = write_ididx_blocks(fd, sb);
	if (ret != 0) {
		perror("write_ididx_blocks()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write metadata blocks */
	ret = write_metadata_blocks(fd, sb);
	if (ret != 0) {
		perror("write_metadata_blocks()");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

	/* Write data blocks */
	ret = write_data_blocks(fd, sb);
	if (ret != 0) {
		perror("write_data_blocks():");
		ret = EXIT_FAILURE;
		goto free_sb;
	}

free_sb:
	free(sb);
fclose:
	close(fd);

	return ret;
}
