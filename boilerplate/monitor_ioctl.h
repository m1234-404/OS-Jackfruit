#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>
#include <sys/types.h>

// 1. Container Configuration for clone()
typedef struct {
    char id[32];
    char rootfs[256];
    char command[256];
} child_config_t;

// 2. FIFO Control Commands
typedef enum { CMD_RUN = 2, CMD_PS = 3, CMD_STOP = 5 } command_kind_t;

typedef struct {
    command_kind_t kind;
    char container_id[32];
    char rootfs[256];
    char command[256];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
} control_request_t;

// 3. Kernel IOCTL communication
struct monitor_request {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[32];
};

#define MONITOR_MAGIC 'm'
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)

#endif
