# ouiche_fs - a simple educational filesystem for Linux
The main objective of this project is to provide a simple Linux filesystem for students to build on.

## Summary
- [Usage](#Usage)
- [Design](#Design)
- [Roadmap](#Roadmap)

## Usage
### Building the kernel module
You can build the kernel module for your currently running kernel with `make`. If you wish to build the module against a different kernel, run `make KERNELDIR=<path>`. Insert the module with `insmod ouichefs.ko`.

This code was tested on a 6.5.7 kernel.

### Formatting a partition
First, build `mkfs.ouichefs` from the mkfs directory. Run `mkfs.ouichefs img` to format img as a ouiche_fs partition. For example, create a zeroed file of 50 MiB with `dd if=/dev/zero of=test.img bs=1M count=50` and run `mkfs.ouichefs test.img`. You can then mount this image on a system with the ouiche_fs kernel module installed.

## Design
This filesystem does not provide any fancy feature to ease understanding.

### Partition layout
    +------------+-------------+-------------------+-------------------+------------------------+--------------------------+----------------+-------------+
    | superblock | inode store | inode free bitmap | block free bitmap | inode data free bitmap | inode data index mapping | block metadata | data blocks |
    +------------+-------------+-------------------+-------------------+------------------------+--------------------------+----------------+-------------+
Each block is 4 KiB large.

### Superblock
The superblock is the first block of the partition (block 0). It contains the partition's metadata, such as the number of blocks, number of inodes, number of free inodes/blocks, ...

### Inode store
Contains all the inodes of the partition.
The maximum number of inodes is equal to the number of blocks of the partition.
In this implementation, an inode is just a list of inode data entries, one for each snapshot.

### Inode data entry
Each inode data entry contains 80 B of data: standard data such as file size and number of used blocks, as well as a ouiche_fs-specific field called `index_block`. This block contains:
  - for a directory: the list of files in this directory. A directory can contain at most 128 files, and filenames are limited to 28 characters to fit in a single block.
  
![directory block](docs/dir_block.png)
  - for a file: the list of blocks containing the actual data of this file. Since block IDs are stored as 32-bit values, at most 1024 links fit in a single block, limiting the size of a file to 4 MiB.

![file block](docs/file_block.png)

### Free bitmaps
These three bitmaps track if inodes/blocks/inode data entries are used or not.

### Inode data index mapping
This file system implements Copy-on-Write for both data and inodes.
The Inode CoW implementation uses 'inode data entries' to share data across snapshots.
These entries are stored inside data block which are allocated on demand.
As inode data entries are indexed continuously by a counter, but not stored consecutively in memory, the inode data index is used to map the index to the block that contains the actual data.

### Block Metadata
Metadata for all data blocks is stored here, e.g. a reference counter for each data block.
If some data is referenced multiple times, any changes are written to a copy of that block,
and only if nothing references the block is it actually freed.

### Data blocks
The remainder of the partition is used to store actual data on disk.

### Data structure relations in the Linux kernel
![Linux VFS](docs/vfs_struct_relations.png)

## Roadmap
### Current features
#### Snapshots using sysfs
- Creation and Restore don't consume extra space thanks to Copy-on-Write
- Delete
- List
#### Directories
- Creation and deletion
- List content
- Renaming

#### Regular files
- Creation and deletion
- Reading and writing (through the page cache)
- Renaming
- Copy-on-Write using Reflinking

### Future features
- Hard and symbolic link support
