// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/time.h>
#include <linux/list.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/mount.h>
#include <linux/kernel.h>

#include "ouichefs.h"

#define OUICHEFS_DEVICE_NAME_LENGTH 16

struct ouichefs_partition {
	char name[OUICHEFS_DEVICE_NAME_LENGTH]; // device name, e.g. "sda"
	struct kobject kobj;
	struct list_head partition_list;
	struct super_block *sb;
};

LIST_HEAD(ouichefs_partitions);

#define to_ouichefs_partition(x) container_of(x, struct ouichefs_partition, kobj)

struct partition_attribute {
	struct attribute attr;
	ssize_t (*show)(struct ouichefs_partition *partition,
		struct partition_attribute *attr, char *buf);
	ssize_t (*store)(struct ouichefs_partition *partition,
		struct partition_attribute *attr, const char *buf, size_t count);
};

#define to_partition_attribute(x) container_of(x, struct partition_attribute, attr)

static ssize_t partition_attr_show(struct kobject *kobj,
			     struct attribute *attr,
			     char *buf)
{
	struct partition_attribute *attribute;
	struct ouichefs_partition *partition;

	attribute = to_partition_attribute(attr);
	partition = to_ouichefs_partition(kobj);

	if (!attribute->show)
		return -EIO;

	return attribute->show(partition, attribute, buf);
}

static ssize_t partition_attr_store(struct kobject *kobj,
			      struct attribute *attr,
			      const char *buf, size_t len)
{
	struct partition_attribute *attribute;
	struct ouichefs_partition *partition;

	attribute = to_partition_attribute(attr);
	partition = to_ouichefs_partition(kobj);

	if (!attribute->store)
		return -EIO;

	return attribute->store(partition, attribute, buf, len);
}

static const struct sysfs_ops partition_sysfs_ops = {
	.show = partition_attr_show,
	.store = partition_attr_store,
};

struct super_block *get_superblock_from_filepath(const char *filepath)
{
	struct path path;
	struct dentry *dentry;
	struct inode *inode;
	struct super_block *sb;

    // Get a 'path' structure
	if (kern_path(filepath, LOOKUP_FOLLOW, &path) != 0) {
		pr_err("Failed to resolve path: %s\n", filepath);
		return NULL;
	}

	// Get dentry and inode
	dentry = path.dentry;
	inode = dentry->d_inode;

	if (!inode) {
		pr_err("Failed to get inode for path: %s\n", filepath);
		return NULL;
	}

    // Get superblock
	sb = inode->i_sb;

	path_put(&path);

	return sb;
}

static int add_snapshot(struct ouichefs_partition *part)
{
	int ret = ouichefs_snapshot_create(part->sb);
	if (!ret)
		pr_info("ouichefs: Created snapshot in partition %s\n", part->name);
	return ret;
}

static int remove_snapshot(struct ouichefs_partition *part, unsigned int id)
{
	int ret = ouichefs_snapshot_delete(part->sb, id);
	if (!ret)
		pr_info("ouichefs: Destroyed snapshot %u in partition %s\n", id, part->name);

	return ret;
}

static int restore_snapshot(struct ouichefs_partition *part, unsigned int id)
{
	int ret = ouichefs_snapshot_restore(part->sb, id);
	if (!ret)
		pr_info("ouichefs: Restored snapshot %u in partition %s\n", id, part->name);

	return ret;
}

static ssize_t create_store(struct ouichefs_partition *part, struct partition_attribute *attr,
			      const char *buf, size_t count)
{
	int ret = add_snapshot(part);

	if (ret)
		return ret;
	return count;
}

static ssize_t destroy_store(struct ouichefs_partition *part, struct partition_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned int id;
	int ret;

	if (kstrtouint(buf, 0, &id))
		return -EINVAL;

	ret = remove_snapshot(part, id);
	if (ret)
		return ret;
	return count;
}

static ssize_t restore_store(struct ouichefs_partition *part, struct partition_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned int id;
	int ret;

	if (kstrtouint(buf, 0, &id))
		return -EINVAL;

	ret = restore_snapshot(part, id);
	if (ret)
		return ret;
	return count;
}

static ssize_t list_show(struct ouichefs_partition *part, struct partition_attribute *attr,
			   char *buf)
{
	return ouichefs_snapshot_list(part->sb, buf);
}

static struct partition_attribute create_attr = __ATTR(create, 0220, NULL, create_store);
static struct partition_attribute destroy_attr = __ATTR(destroy, 0220, NULL, destroy_store);
static struct partition_attribute restore_attr = __ATTR(restore, 0220, NULL, restore_store);
static struct partition_attribute list_attr = __ATTR(list, 0444, list_show, NULL);

static struct attribute *partition_attrs[] = {
	&create_attr.attr,
	&destroy_attr.attr,
	&restore_attr.attr,
	&list_attr.attr,
	NULL,
};

ATTRIBUTE_GROUPS(partition);

static void partition_release(struct kobject *kobj)
{
	struct ouichefs_partition *part;

	part = to_ouichefs_partition(kobj);
	list_del(&part->partition_list);
	kfree(part);
}

static const struct kobj_type ktype_default = {
	.sysfs_ops = &partition_sysfs_ops,
	.release = partition_release,
	.default_groups = partition_groups,
};

static struct kset *ouichefs_kset;

static const char *find_last_part_of_path(const char *path)
{
	int n = strlen(path);
	const char *suffix = path + n;

	while (path[--n] != '/' && 0 < n)
		;

	if (path[n] == '/')
		suffix = path + n + 1;

	return suffix;
}

static int create_partition_sysfs_entry(struct ouichefs_partition *part)
{
	int ret;

	part->kobj.kset = ouichefs_kset;

	ret = kobject_init_and_add(&part->kobj, &ktype_default,
			       NULL, "%s", part->name);
	if (ret) {
		kobject_put(&part->kobj);
		return ret;
	}

	kobject_uevent(&part->kobj, KOBJ_ADD);

	return 0;
}

int create_ouichefs_partition_entry(const char *dev_name, struct super_block *sb)
{
	int ret;
	struct ouichefs_partition *part;
	const char *partition_name;

	part = kzalloc(sizeof(*part), GFP_KERNEL);
	if (!part) {
		ret = -ENOMEM;
		goto error_alloc;
	}

	partition_name = find_last_part_of_path(dev_name);
	strscpy(part->name, partition_name, OUICHEFS_DEVICE_NAME_LENGTH);
	part->sb = sb;
	ret = create_partition_sysfs_entry(part);
	if (ret) {
		kfree(part);
		goto error_alloc;
	}

	list_add(&part->partition_list, &ouichefs_partitions);

	pr_info("ouichefs: Partition %s registered\n", part->name);

	return 0;

error_alloc:
	kset_unregister(ouichefs_kset);
	return ret;
}

void remove_ouichefs_partition_entry(const char *dev_name)
{
	struct ouichefs_partition *part, *tmp;

	list_for_each_entry_safe(part, tmp, &ouichefs_partitions, partition_list) {
		if (!strcmp(part->name, dev_name)) {
			kobject_put(&part->kobj);
			pr_info("ouichefs: sysfs entry removed for partition '%s'\n", dev_name);
			break;
		}
	}
}

int init_sysfs_interface(void)
{
	pr_info("ouichefs: Initializing sysfs interface\n");

	/* Create base sysfs directory: /sys/fs/ouichefs */
	ouichefs_kset = kset_create_and_add("ouichefs", NULL, fs_kobj);

	if (!ouichefs_kset)
		return -ENOMEM;

	return 0;
}

void cleanup_sysfs_interface(void)
{
	kset_unregister(ouichefs_kset);
}