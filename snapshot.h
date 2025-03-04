/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/fs.h>

// TYPE DEFINITIONS: Makes it easier to update code if we want to adjust the size of some fields
#define ouichefs_snap_id_t uint32_t   /* Unique ID of a snapshot */
#define ouichefs_snap_index_t uint8_t /* Data type large enough to index OUICHEFS_MAX_SNAPSHOTS */
// All out internal block addresses are uint32_t

struct ouichefs_snapshot_info {
	time64_t created; /* Creation time (sec) */
	uint32_t root_inode; /* Address of this snapshots root inode */
	ouichefs_snap_id_t id; /* Unique identifier of this snapshot */
};

int create(struct super_block *sb, struct ouichefs_snapshot_info *sn);

void delete(ouichefs_snap_id_t s_id);

void list(void);

void restore(ouichefs_snap_id_t s_id);
