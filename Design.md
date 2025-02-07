# Design

This document outlines our design decisions for the snapshotting system.

## Done
- **Snapshots are immutable**: Once a snapshot has been created, its contents cannot be changed anymore.
This is to prevent complications regarding changes to intermediate snapshots that would need to propagate downwards (e.g. 5 snapshots done after each other, 2nd is restored and edited;
This change must not propagate to the 3 snapshots after it)
- **Changes are stored in a "live snapshot"**, there is only one at a time (containing all changes since the last snapshot was done).
If a snapshot is restored, the old "live" snapshot is marked "normal" and a new "live" copy of the restored snapshot is created (and henceforth used for changes).
- **Metadata is copied for now** and data is Copy-on-Write, since this is easier to implement. We want to have Copy-on-Write metadata later.

## TBD
- How do we clean up data when we delete a snapshot? To check if data is no longer referenced, we need to check if any "child" inodes exist. If they do we cannot delete it.
- How do we snapshot the actual blocks?
- Deleting intermediate snapshots can become messy on CoW Metadata, since we have to update lots of references between inodes
- How do we lock the file system while we perform snapshot operations?
