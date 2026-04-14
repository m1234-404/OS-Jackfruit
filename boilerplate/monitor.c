#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/pid.h>
#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

struct monitored_entry {
    pid_t pid;
    uint64_t soft_limit_bytes;
    uint64_t hard_limit_bytes;
    char container_id[MONITOR_NAME_LEN];
    int soft_warned;
    struct list_head list;
};

static LIST_HEAD(monitored_list);
static DEFINE_SPINLOCK(monitored_lock);
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

static long get_rss_bytes(pid_t pid) {
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = -1;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        get_task_struct(task);
        rcu_read_unlock();
        mm = get_task_mm(task);
        if (mm) {
            rss_pages = get_mm_rss(mm);
            mmput(mm);
        }
        put_task_struct(task);
    } else {
        rcu_read_unlock();
    }
    return (rss_pages >= 0) ? rss_pages * PAGE_SIZE : -1;
}

static void timer_callback(struct timer_list *t) {
    struct monitored_entry *entry, *tmp;
    unsigned long flags;

    spin_lock_irqsave(&monitored_lock, flags);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        long rss = get_rss_bytes(entry->pid);
        if (rss < 0) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if ((uint64_t)rss > entry->soft_limit_bytes && !entry->soft_warned) {
            pr_warn("[Monitor] Container %s (PID %d) exceeded soft limit\n", entry->container_id, entry->pid);
            entry->soft_warned = 1;
        }

        if ((uint64_t)rss > entry->hard_limit_bytes) {
            struct task_struct *task;
            pr_emerg("[Monitor] HARD LIMIT KILL: %s (PID %d)\n", entry->container_id, entry->pid);
            rcu_read_lock();
            task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
            if (task) send_sig(SIGKILL, task, 1);
            rcu_read_unlock();
            list_del(&entry->list);
            kfree(entry);
        }
    }
    spin_unlock_irqrestore(&monitored_lock, flags);
    mod_timer(&monitor_timer, jiffies + HZ);
}

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    struct monitor_request req;
    struct monitored_entry *entry;
    unsigned long flags;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) return -ENOMEM;
        entry->pid = req.pid;
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_warned = 0;
        strncpy(entry->container_id, req.container_id, MONITOR_NAME_LEN);

        spin_lock_irqsave(&monitored_lock, flags);
        list_add(&entry->list, &monitored_list);
        spin_unlock_irqrestore(&monitored_lock, flags);
        
        pr_info("[Monitor] Registered container %s (PID %d)\n", req.container_id, req.pid);
        return 0;
    }
    return -EINVAL;
}

static struct file_operations fops = { .unlocked_ioctl = monitor_ioctl, .owner = THIS_MODULE };

static int __init monitor_init(void) {
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0) return -1;
    cl = class_create(DEVICE_NAME);
    device_create(cl, NULL, dev_num, NULL, DEVICE_NAME);
    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, dev_num, 1);
    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + HZ);
    return 0;
}

static void __exit monitor_exit(void) {
    struct monitored_entry *entry, *tmp;
    timer_delete_sync(&monitor_timer);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Container Memory Monitor");
