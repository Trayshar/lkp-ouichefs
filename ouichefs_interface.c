#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/time.h>
#include <linux/list.h>
#include <linux/sysfs.h>

#include "ouichefs.h"

LIST_HEAD(ouichefs_partitions);

#define to_ouichefs_partition(x) container_of(x, struct ouichefs_partition, kobj)

struct partition_attribute {
	struct attribute attr;
	ssize_t (*show)(struct ouichefs_partition *partition, struct partition_attribute *attr, char *buf);
	ssize_t (*store)(struct ouichefs_partition *partition, struct partition_attribute *attr, const char *buf, size_t count);
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

static int add_snapshot(struct ouichefs_partition *part)
{
    struct ouichefs_snapshot *snap;
    struct timespec64 now;

    snap = kmalloc(sizeof(*snap), GFP_KERNEL);
    if (!snap)
        return -ENOMEM;

    snap->id = part->next_id++;
    ktime_get_real_ts64(&now);
    snap->created = now;
    INIT_LIST_HEAD(&snap->list);

    mutex_lock(&part->snap_lock);
    list_add_tail(&snap->list, &part->snapshot_list);
    mutex_unlock(&part->snap_lock);

    /* TODO: add implementation here*/
    pr_info("ouichefs: Created snapshot %u in partition %s\n", snap->id, part->name);
    return 0;
}

static int remove_snapshot(struct ouichefs_partition *part, unsigned int id)
{
    struct ouichefs_snapshot *snap, *tmp;
    bool found = false;

    mutex_lock(&part->snap_lock);
    list_for_each_entry_safe(snap, tmp, &part->snapshot_list, list) {
        if (snap->id == id) {
            list_del(&snap->list);
            kfree(snap);
            found = true;
            break;
        }
    }
    mutex_unlock(&part->snap_lock);

    /* TODO: add implementation here*/
    if (found) {
        pr_info("ouichefs: Destroyed snapshot %u in partition %s\n", id, part->name);
        return 0;
    }
    return -ENOENT;
}

static int restore_snapshot(struct ouichefs_partition *part, unsigned int id)
{
    struct ouichefs_snapshot *snap;
    bool found = false;

    mutex_lock(&part->snap_lock);
    list_for_each_entry(snap, &part->snapshot_list, list) {
        if (snap->id == id) {
            found = true;
            break;
        }
    }
    mutex_unlock(&part->snap_lock);

    if (!found)
        return -ENOENT;

    /* TODO: add implementation here*/
    pr_info("ouichefs: Restored snapshot %u in partition %s\n", id, part->name);
    return 0;
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
    struct ouichefs_snapshot *snap;
    ssize_t pos = 0;
    struct tm tm;

    mutex_lock(&part->snap_lock);
    list_for_each_entry(snap, &part->snapshot_list, list) {
        time64_to_tm(snap->created.tv_sec, 0, &tm);
        /* prints snapshots in format ID: dd.mm.yy HH:MM:SS */
        pos += scnprintf(buf + pos, PAGE_SIZE - pos,
                         "%u: %02d.%02d.%02ld %02d:%02d:%02d\n",
                         snap->id,
                         tm.tm_mday, tm.tm_mon + 1, (tm.tm_year + 1900) % 100,
                         tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    mutex_unlock(&part->snap_lock);
    return pos;
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

static void free_snapshots(struct ouichefs_partition *part)
{
    struct ouichefs_snapshot *snap, *tmp;

    mutex_lock(&part->snap_lock);
    list_for_each_entry_safe(snap, tmp, &part->snapshot_list, list) {
        list_del(&snap->list);
        kfree(snap);
    }
    mutex_unlock(&part->snap_lock);
}

static void partition_release(struct kobject *kobj)
{
    struct ouichefs_partition* part;

    part = to_ouichefs_partition(kobj);
    free_snapshots(part);
    list_del(&part->list);
    kfree(part);
}

static const struct kobj_type ktype_default = {
    .sysfs_ops = &partition_sysfs_ops,
    .release = partition_release,
    .default_groups = partition_groups,
};

static struct kset *ouichefs_kset;

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

int create_ouichefs_partition_entry(const char *dev_name)
{
    int ret;
    struct ouichefs_partition *part;

    part = kzalloc(sizeof(*part), GFP_KERNEL);
    if (!part) {
        ret = -ENOMEM;
        goto error_alloc;
    }

    strncpy(part->name, dev_name, OUICHEFS_DEVICE_NAME_LENGTH);
    mutex_init(&part->snap_lock);
    INIT_LIST_HEAD(&part->snapshot_list);
    part->next_id = 1;

    ret = create_partition_sysfs_entry(part);
    if (ret) {
        kfree(part);
        goto error_alloc;
    }

    list_add(&part->list, &ouichefs_partitions);

    pr_info("ouichefs: Partition %s registered\n", part->name);

    return 0;

error_alloc:
    kset_unregister(ouichefs_kset);
    return ret;
}

void remove_ouichefs_partition_entry(const char *dev_name)
{
    struct ouichefs_partition *part, *tmp;

    list_for_each_entry_safe(part, tmp, &ouichefs_partitions, list) {
        if (!strcmp(part->name, dev_name)) {
            free_snapshots(part);
            sysfs_remove_group(&part->kobj, &partition_group);
            kobject_put(&part->kobj);
            list_del(&part->list);
            kfree(part);

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