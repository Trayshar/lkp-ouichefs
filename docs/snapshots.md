# Design

This document outlines our design decisions for the snapshotting system.

## High-Level Concepts

- **Snapshots are immutable**: Once a snapshot has been created, its contents cannot be changed anymore.
*This is to prevent complications regarding changes to intermediate snapshots that would need to propagate downwards (e.g. 5 snapshots done after each other, 2nd is restored and edited;
This change must not propagate to the 3 snapshots after it)*
- **Changes are stored in a "live snapshot"**, there is only one at a time (containing all changes since the last snapshot was done).
If a snapshot is restored, the old "live" snapshot is marked "normal" and a new "live" copy of the restored snapshot is created (and henceforth used for changes).
*This is our own liberty, and I think it makes sense.*
- **Metadata is copied for now** and data is Copy-on-Write, since this is easier to implement. All we need to store is the new root inode, an identifier and a modification time. Additionally, we tag each inode with the snapshot it belongs to to make debugging easier. To do a snapshot, copy all inodes and overwrite the id. We allow way more IDs than concurrent snapshots to enable ascending use of the IDs - makes it easier to present in the sysfs interface.
**We want to have Copy-on-Write metadata later on**.

## Low-Level Concepts

- **Copy-on-Write Data** is implemented by counting how many other blocks (`ouichefs_file_index_block`, `ouichefs_dir_block`, or an inode) reference it.
To ensure snapshot immutability, if we want to change data inside a block that is referenced more than once, we have to copy it first (and change the reference to point to the new block and decrease the old one's counter). After that we can change the new block.
Cleaning up data (after snapshot deletion) becomes easy: decrease the counter, if it's 0, actually delete the data, else do nothing.
Snapshotting itself just increases the counter.
- **Where to store metadata of a data block**: Storing it inside the data block (e.g. at the beginning) seems like quite the hassle, because now `block size > data in each data block` which we have would need to adjust in all allocation code.
Thus we use a "Metadata Block" that stores this count for some blocks. We take the size of one counter (lets say 1 byte), divide the block size by it (4KB, means ~4000 blocks per metablock) and that's our count. Metadata blocks are stored after the bitmap, because that is closest to the actual data. I considered having them between the data blocks (every ~4000th block would be reserved for metadata), but this is still more tedious to implement and doesn't provide any benefit (as far as I'm aware)
*Pro: easy to implement, data is still stored consecutively*
*Con: we have to load an additional metadata block each time we want to snapshot/delete the block*
- **Where to store snapshot metadata**: Inside the superblock, for now. This works for small number of snapshots (and small snapshot metadata sizes). Eventually, we want a separate block area for snapshots (to allow snapshot descriptions and possibly more)

## TBD

- CoW metadata: How do we clean up data when we delete a snapshot? To check if data is no longer referenced, we need to check if any "child" inodes exist. If they do we cannot delete it.
- CoW metadata: Deleting intermediate snapshots can become messy, since we have to update lots of references between inodes
- How do we lock the file system while we perform snapshot operations?
