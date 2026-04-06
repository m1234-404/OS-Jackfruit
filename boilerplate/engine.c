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

// Define this BEFORE run_supervisor to fix the warning
void register_with_kernel(int fd, pid_t pid, control_request_t *req) {
    struct monitor_request m_req;
    memset(&m_req, 0, sizeof(m_req));
    
    m_req.pid = pid;
    m_req.soft_limit_bytes = req->soft_limit_bytes;
    m_req.hard_limit_bytes = req->hard_limit_bytes;
    strncpy(m_req.container_id, req->container_id, 31);

    if (ioctl(fd, MONITOR_REGISTER, &m_req) < 0) {
        perror("IOCTL Register Failed");
    }
}

int container_main(void *arg) {
    child_config_t *config = (child_config_t *)arg;

    sethostname(config->id, strlen(config->id));
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        perror("Chroot failed");
        return 1;
    }

    umount2("/proc", MNT_DETACH); 
    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    char *const child_args[] = {"/bin/sh", "-c", config->command, NULL};
    execvp(child_args[0], child_args);
    return 0;
}

int run_supervisor(char *rootfs_path) {
    int mon_fd = open("/dev/container_monitor", O_RDWR);
    if (mon_fd < 0) perror("Could not open monitor device");

    mkfifo(FIFO_PATH, 0666);
    int fifo_read = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    int dummy_fd = open(FIFO_PATH, O_WRONLY); 
    (void)dummy_fd; // Fix unused variable warning

    printf("Supervisor running. Waiting for commands...\n");

    while (1) {
        control_request_t req;
        if (read(fifo_read, &req, sizeof(req)) == sizeof(req)) {
            if (req.kind == CMD_RUN) {
                char *stack = malloc(STACK_SIZE);
                child_config_t *config = malloc(sizeof(child_config_t));
                
                strncpy(config->id, req.container_id, 31);
                strncpy(config->rootfs, rootfs_path, 255);
                strncpy(config->command, req.command, 255);

                pid_t pid = clone(container_main, stack + STACK_SIZE, 
                                  CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, config);
                
                if (pid > 0) {
                    printf("Container %s started (PID: %d)\n", req.container_id, pid);
                    register_with_kernel(mon_fd, pid, &req);
                }
            }
        }
        while (waitpid(-1, NULL, WNOHANG) > 0);
        usleep(100000);
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <supervisor|run> [args]\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        return run_supervisor(argv[2]);
    } 
    
    if (strcmp(argv[1], "run") == 0) {
        // This part sends the command to the supervisor
        control_request_t req;
        memset(&req, 0, sizeof(req));
        req.kind = CMD_RUN;
        strncpy(req.container_id, argv[2], 31);
        strncpy(req.rootfs, argv[3], 255);
        strncpy(req.command, argv[4], 255);
        
        // Memory limits (defaulting to 50MB for test)
        req.soft_limit_bytes = 50 * 1024 * 1024;
        req.hard_limit_bytes = 80 * 1024 * 1024;

        int fd = open(FIFO_PATH, O_WRONLY);
        if (fd < 0) {
            perror("Is the supervisor running?");
            return 1;
        }
        write(fd, &req, sizeof(req));
        close(fd);
        printf("Sent run request for %s to supervisor\n", argv[2]);
        return 0;
    }

    return 0;
}

