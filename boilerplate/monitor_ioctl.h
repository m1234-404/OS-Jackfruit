#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H
#include <linux/ioctl.h>
#include <sys/types.h>

typedef struct { char id[32], rootfs[256], command[256]; } child_config_t;
typedef enum { CMD_RUN = 2, CMD_PS = 3, CMD_STOP = 5 } command_kind_t;
typedef struct {
    command_kind_t kind;
    char container_id[32], rootfs[256], command[256];
    unsigned long soft_limit_bytes, hard_limit_bytes;
} control_request_t;
struct monitor_request {
    pid_t pid;
    unsigned long soft_limit_bytes, hard_limit_bytes;
    char container_id[32];
};
#define MONITOR_MAGIC 'm'
#define MONITOR_REGISTER _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#endif
