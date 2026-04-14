#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MONITOR_NAME_LEN 32

struct monitor_request {
    int32_t pid;
    uint32_t padding; 
    uint64_t soft_limit_bytes;
    uint64_t hard_limit_bytes;
    char container_id[MONITOR_NAME_LEN];
} __attribute__((packed, aligned(8)));

#define MONITOR_MAGIC 'M'
#define MONITOR_REGISTER _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)

#endif
