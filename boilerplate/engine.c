#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define FIFO_PATH "/tmp/container_engine_fifo"

// Helper to register with kernel
void register_with_kernel(int fd, pid_t pid, control_request_t *req) {
    struct monitor_request m_req = {
        .pid = pid,
        .soft_limit_bytes = req->soft_limit_bytes,
        .hard_limit_bytes = req->hard_limit_bytes
    };
    strncpy(m_req.container_id, req->container_id, 31);
    if (ioctl(fd, MONITOR_REGISTER, &m_req) < 0) perror("ioctl register");
}

int container_main(void *arg) {
    child_config_t *config = (child_config_t *)arg;

    // 1. Isolation
    sethostname(config->id, strlen(config->id));
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    // 2. Pivot Root / Chroot
    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        perror("chroot/chdir");
        return 1;
    }

    // 3. Mount Proc for local 'ps' visibility
    umount2("/proc", MNT_DETACH); 
    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    // 4. Execute
    char *const child_args[] = {"/bin/sh", "-c", config->command, NULL};
    execvp(child_args[0], child_args);
    return 0;
}

int run_supervisor() {
    int mon_fd = open("/dev/container_monitor", O_RDWR);
    mkfifo(FIFO_PATH, 0666);
    int fifo_read = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    int dummy_fd = open(FIFO_PATH, O_WRONLY); // Prevent EOF spin

    printf("Supervisor Active. Waiting for commands...\n");

    while (1) {
        control_request_t req;
        if (read(fifo_read, &req, sizeof(req)) == sizeof(req)) {
            if (req.kind == CMD_RUN) {
                char *stack = malloc(STACK_SIZE);
                child_config_t *config = malloc(sizeof(child_config_t));
                strcpy(config->id, req.container_id);
                strcpy(config->rootfs, req.rootfs);
                strcpy(config->command, req.command);

                pid_t pid = clone(container_main, stack + STACK_SIZE, 
                                  CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, config);
                
                if (pid > 0) {
                    printf("Container %s started (PID: %d)\n", req.container_id, pid);
                    register_with_kernel(mon_fd, pid, &req);
                }
            }
        }
        // Reap exited children
        while (waitpid(-1, NULL, WNOHANG) > 0);
        usleep(100000);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "supervisor") == 0) return run_supervisor();
    // (Add cmd_run / cmd_ps logic here to send data to FIFO)
    return 0;
}

