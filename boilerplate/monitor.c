#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "monitor_ioctl.h"

struct monitored_proc {
    pid_t pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
    char container_id[32];
    struct list_head list;
};

static LIST_HEAD(proc_list);
static DEFINE_MUTEX(list_lock);
static struct task_struct *monitor_thread;

static unsigned long get_process_rss(struct task_struct *task) {
    struct mm_struct *mm = get_task_mm(task);
    unsigned long rss = 0;
    if (mm) {
        rss = get_mm_rss(mm) << PAGE_SHIFT;
        mmput(mm);
    }
    return rss;
}

static int monitor_loop(void *data) {
    struct monitored_proc *curr, *tmp;
    while (!kthread_should_stop()) {
        mutex_lock(&list_lock);
        list_for_each_entry_safe(curr, tmp, &proc_list, list) {
            struct task_struct *task = pid_task(find_get_pid(curr->pid), PIDTYPE_PID);
            if (!task) {
                list_del(&curr->list);
                kfree(curr);
                continue;
            }
            unsigned long rss = get_process_rss(task);
            if (rss > curr->hard_limit) {
                send_sig(SIGKILL, task, 0);
            }
        }
        mutex_unlock(&list_lock);
        msleep(1000);
    }
    return 0;
}

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct monitor_request req;
    struct monitored_proc *new_node;
    if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;

    mutex_lock(&list_lock);
    if (cmd == MONITOR_REGISTER) {
        new_node = kmalloc(sizeof(*new_node), GFP_KERNEL);
        new_node->pid = req.pid;
        new_node->soft_limit = req.soft_limit_bytes;
        new_node->hard_limit = req.hard_limit_bytes;
        strncpy(new_node->container_id, req.container_id, 31);
        list_add(&new_node->list, &proc_list);
    }
    mutex_unlock(&list_lock);
    return 0;
}

static const struct file_operations fops = { .unlocked_ioctl = monitor_ioctl, .owner = THIS_MODULE };
static struct miscdevice mon_dev = { .minor = MISC_DYNAMIC_MINOR, .name = "container_monitor", .fops = &fops };

static int __init monitor_init(void) {
    misc_register(&mon_dev);
    monitor_thread = kthread_run(monitor_loop, NULL, "container_monitor_worker");
    return 0;
}

static void __exit monitor_exit(void) {
    if (monitor_thread) kthread_stop(monitor_thread);
    misc_deregister(&mon_dev);
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
