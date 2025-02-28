// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/fs.h>
#include "ouichefs.h"


int ouichefs_snapshot_create(struct super_block *sb)
{
	int ret = 0;

	//allocate new snapshot
	struct ouichefs_snapshot_info *sn = kmalloc(sizeof(struct ouichefs_snapshot_info), GFP_KERNEL);

	if (!sn)
		return -ENOMEM;

    //freeze fs to lock it
	ret = freeze_super(sb);

	if (ret) {
		pr_err("file system freeze failed\n");
		goto cleanup;
	}


    //copy inodes

    //change root inode of fs to be root inode of new snapshot

    //unfreeze fs to unlock it

	ret = thaw_super(sb);

	if (ret) {
		pr_err("File system unfreeze failed\n");
		goto cleanup;
	}

	return 0;

cleanup:
	kfree(sn);
	return ret;
}

int ouichefs_snapshot_delete(struct super_block *sb, ouichefs_snap_id_t s_id)
{
	return -ENOTSUPP;
}

int ouichefs_snapshot_list(struct super_block *sb, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot_info s_info;
	struct tm *tm;
	ssize_t pos = 0;

	tm = kmalloc(sizeof(struct tm), GFP_KERNEL);

	for (int i = 0; i < OUICHEFS_MAX_SNAPSHOTS; i++) {
		s_info = sbi->snapshots[i];
		if (s_info.id == 0 || i == sbi->current_snapshot_index) {
			continue;
		} else {
			time64_to_tm(s_info.created, 0, tm);
			pos += scnprintf(buf + pos, PAGE_SIZE - pos,
				"%u: %02d.%02d.%02ld %02d:%02d:%02d\n",
				s_info.id,
				tm->tm_mday, tm->tm_mon + 1, (tm->tm_year + 1900) % 100,
				tm->tm_hour, tm->tm_min, tm->tm_sec);
		}
	}

	kfree(tm);

	return pos;
}

int ouichefs_snapshot_restore(struct super_block *sb, ouichefs_snap_id_t s_id)
{
	return -ENOTSUPP;
}
