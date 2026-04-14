#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/resource.h> // Added for setpriority (Task 5)
#include <getopt.h>       // Added for flag parsing (Task 2)
#include <errno.h>
#include <signal.h>
#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define FIFO_PATH "/tmp/container_engine_fifo"
#define LOG_BUFFER_SIZE 1024
#define MAX_CONTAINERS 10

typedef struct {
    char data[256];
    char container_id[MONITOR_NAME_LEN];
} log_msg_t;

typedef struct {
    log_msg_t buffer[LOG_BUFFER_SIZE];
    int head, tail, count;
    int shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} bounded_buffer_t;

typedef enum { STARTING, RUNNING, STOPPED, KILLED } c_state;
typedef struct {
    char id[MONITOR_NAME_LEN];
    pid_t pid;
    c_state state;
    int stop_requested;
    int is_blocking; // Added to track 'run' vs 'start'
} container_info_t;

container_info_t registry[MAX_CONTAINERS];
int container_count = 0;
pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
bounded_buffer_t logger_buffer;
int supervisor_running = 1;

typedef struct {
    int cmd_type; // 1: START/RUN, 2: PS, 3: STOP
    int is_blocking;
    char container_id[MONITOR_NAME_LEN];
    char rootfs[256];
    char command[256];
    uint64_t soft_limit;
    uint64_t hard_limit;
    int nice_value; // Added for Task 5
    int pipe_fd; 
} control_request_t;

// [Task 3: Logging logic remains unchanged as per instructions]
void log_push(const char *id, const char *msg) {
    pthread_mutex_lock(&logger_buffer.lock);
    while (logger_buffer.count == LOG_BUFFER_SIZE && !logger_buffer.shutdown)
        pthread_cond_wait(&logger_buffer.not_full, &logger_buffer.lock);
    if (logger_buffer.shutdown) { pthread_mutex_unlock(&logger_buffer.lock); return; }
    log_msg_t *m = &logger_buffer.buffer[logger_buffer.head];
    strncpy(m->container_id, id, MONITOR_NAME_LEN);
    strncpy(m->data, msg, 255);
    logger_buffer.head = (logger_buffer.head + 1) % LOG_BUFFER_SIZE;
    logger_buffer.count++;
    pthread_cond_signal(&logger_buffer.not_empty);
    pthread_mutex_unlock(&logger_buffer.lock);
}

void* consumer_thread(void* arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&logger_buffer.lock);
        while (logger_buffer.count == 0 && !logger_buffer.shutdown)
            pthread_cond_wait(&logger_buffer.not_empty, &logger_buffer.lock);
        if (logger_buffer.shutdown && logger_buffer.count == 0) { pthread_mutex_unlock(&logger_buffer.lock); break; }
        log_msg_t *m = &logger_buffer.buffer[logger_buffer.tail];
        char path[512];
        snprintf(path, sizeof(path), "logs/%s.log", m->container_id);
        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0666);
        if (fd >= 0) { if (write(fd, m->data, strlen(m->data)) < 0) perror("log write"); close(fd); }
        logger_buffer.tail = (logger_buffer.tail + 1) % LOG_BUFFER_SIZE;
        logger_buffer.count--;
        pthread_cond_signal(&logger_buffer.not_full);
        pthread_mutex_unlock(&logger_buffer.lock);
    }
    return NULL;
}

typedef struct { int read_fd; char id[MONITOR_NAME_LEN]; } relay_args_t;
void* log_relay(void* arg) {
    relay_args_t *ra = (relay_args_t*)arg;
    char buf[512]; ssize_t n;
    while ((n = read(ra->read_fd, buf, sizeof(buf) - 1)) > 0) { buf[n] = '\0'; log_push(ra->id, buf); }
    close(ra->read_fd); free(ra); return NULL;
}

void handle_sigint(int sig) { (void)sig; supervisor_running = 0; }

// Updated for Task 5: Scheduler Experiments
int child_fn(void *arg) {
    control_request_t *req = (control_request_t *)arg;
    dup2(req->pipe_fd, STDOUT_FILENO);
    dup2(req->pipe_fd, STDERR_FILENO);
    close(req->pipe_fd);

    // Apply scheduling priority (Task 5)
    if (setpriority(PRIO_PROCESS, 0, req->nice_value) < 0) perror("setpriority");

    if (sethostname(req->container_id, strlen(req->container_id)) < 0) perror("sethostname");
    if (chroot(req->rootfs) != 0 || chdir("/") != 0) exit(1);
    mount("proc", "/proc", "proc", 0, NULL);
    
    execlp("/bin/sh", "sh", "-c", req->command, NULL);
    return 0;
}

int run_supervisor() {
    signal(SIGINT, handle_sigint);
    int m_fd = open("/dev/container_monitor", O_RDWR);
    mkdir("logs", 0777);
    
    pthread_mutex_init(&logger_buffer.lock, NULL);
    pthread_cond_init(&logger_buffer.not_full, NULL);
    pthread_cond_init(&logger_buffer.not_empty, NULL);
    logger_buffer.shutdown = 0;

    pthread_t cons_tid;
    pthread_create(&cons_tid, NULL, consumer_thread, NULL);

    unlink(FIFO_PATH);
    mkfifo(FIFO_PATH, 0666);
    int f_fd = open(FIFO_PATH, O_RDWR | O_NONBLOCK);
    
    printf("Supervisor Ready (PID %d).\n", getpid());

    while (supervisor_running) {
        control_request_t req;
        if (read(f_fd, &req, sizeof(req)) > 0) {
            if (req.cmd_type == 1) { // START or RUN
                int pipefds[2];
                if (pipe(pipefds) < 0) perror("pipe");
                req.pipe_fd = pipefds[1];

                void *stack = malloc(STACK_SIZE);
                pid_t pid = clone(child_fn, (char*)stack + STACK_SIZE, 
                                 CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, &req);
                
                close(pipefds[1]); 
                
                char init_msg[128];
                snprintf(init_msg, sizeof(init_msg), "--- Container %s Started (PID %d) ---\n", req.container_id, pid);
                log_push(req.container_id, init_msg);

                relay_args_t *ra = malloc(sizeof(relay_args_t));
                ra->read_fd = pipefds[0];
                strncpy(ra->id, req.container_id, MONITOR_NAME_LEN);
                pthread_t relay_tid;
                pthread_create(&relay_tid, NULL, log_relay, ra);
                pthread_detach(relay_tid);

                pthread_mutex_lock(&registry_lock);
                registry[container_count++] = (container_info_t){
                    .pid = pid, .state = RUNNING, .stop_requested = 0, .is_blocking = req.is_blocking
                };
                strncpy(registry[container_count-1].id, req.container_id, MONITOR_NAME_LEN);
                pthread_mutex_unlock(&registry_lock);

                struct monitor_request m_req = { .pid = (int32_t)pid, .soft_limit_bytes = req.soft_limit, .hard_limit_bytes = req.hard_limit };
                strncpy(m_req.container_id, req.container_id, MONITOR_NAME_LEN);
                if (m_fd >= 0) ioctl(m_fd, MONITOR_REGISTER, &m_req);
            } else if (req.cmd_type == 2) { // PS
                printf("\nID\tPID\tSTATE\tNICE\n");
                pthread_mutex_lock(&registry_lock);
                for(int i=0; i<container_count; i++) 
                    printf("%s\t%d\t%d\t%d\n", registry[i].id, registry[i].pid, (int)registry[i].state, getpriority(PRIO_PROCESS, registry[i].pid));
                pthread_mutex_unlock(&registry_lock);
            } else if (req.cmd_type == 3) { // STOP
                pthread_mutex_lock(&registry_lock);
                for(int i=0; i<container_count; i++) {
                    if (strcmp(registry[i].id, req.container_id) == 0 && registry[i].state == RUNNING) {
                        registry[i].stop_requested = 1;
                        kill(registry[i].pid, SIGTERM);
                    }
                }
                pthread_mutex_unlock(&registry_lock);
            }
            fflush(stdout);
        }
        
        int status; pid_t dead_pid;
        while ((dead_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            pthread_mutex_lock(&registry_lock);
            for(int i=0; i<container_count; i++) {
                if(registry[i].pid == dead_pid) {
                    if (registry[i].stop_requested) registry[i].state = STOPPED;
                    else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) registry[i].state = KILLED;
                    else registry[i].state = STOPPED;
                    
                    // If 'run' was used, notify the client via a response FIFO
                    if (registry[i].is_blocking) {
                        char res_path[128];
                        snprintf(res_path, sizeof(res_path), "/tmp/engine_res_%s", registry[i].id);
                        int res_fd = open(res_path, O_WRONLY);
                        if (res_fd >= 0) { write(res_fd, &status, sizeof(status)); close(res_fd); }
                    }
                    printf("Container %s exited.\n", registry[i].id);
                }
            }
            pthread_mutex_unlock(&registry_lock);
            fflush(stdout);
        }
        usleep(50000);
    }
    // [Cleanup logic remains unchanged]
    return 0;
}

// Updated for Task 2: Mandatory CLI grammar and flag parsing
int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "supervisor") == 0) return run_supervisor();

    int fd = open(FIFO_PATH, O_WRONLY);
    if (fd < 0) { perror("Supervisor not running"); return 1; }

    control_request_t req = {0};
    req.nice_value = 0;
    req.soft_limit = 40ULL << 20; // Default 40 MiB
    req.hard_limit = 64ULL << 20; // Default 64 MiB

    if (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "run") == 0) {
        if (argc < 5) { printf("Usage: engine %s <id> <rootfs> <cmd> [flags]\n", argv[1]); return 1; }
        req.cmd_type = 1;
        req.is_blocking = (strcmp(argv[1], "run") == 0);
        strncpy(req.container_id, argv[2], 31);
        strncpy(req.rootfs, argv[3], 255);
        strncpy(req.command, argv[4], 255);

        struct option long_options[] = {
            {"soft-mib", required_argument, 0, 's'},
            {"hard-mib", required_argument, 0, 'h'},
            {"nice",     required_argument, 0, 'n'},
            {0, 0, 0, 0}
        };

        int opt;
        while ((opt = getopt_long(argc, argv, "s:h:n:", long_options, NULL)) != -1) {
            switch (opt) {
                case 's': req.soft_limit = (uint64_t)atoll(optarg) << 20; break;
                case 'h': req.hard_limit = (uint64_t)atoll(optarg) << 20; break;
                case 'n': req.nice_value = atoi(optarg); break;
            }
        }

        if (req.is_blocking) {
            char res_path[128];
            snprintf(res_path, sizeof(res_path), "/tmp/engine_res_%s", req.container_id);
            mkfifo(res_path, 0666);
            if (write(fd, &req, sizeof(req)) < 0) perror("write");
            
            int res_fd = open(res_path, O_RDONLY);
            int exit_status;
            read(res_fd, &exit_status, sizeof(exit_status));
            printf("Container %s exited with status %d\n", req.container_id, WEXITSTATUS(exit_status));
            close(res_fd);
            unlink(res_path);
            return 0;
        }
    } else if (strcmp(argv[1], "ps") == 0) {
        req.cmd_type = 2;
    } else if (strcmp(argv[1], "stop") == 0 && argc >= 3) {
        req.cmd_type = 3;
        strncpy(req.container_id, argv[2], 31);
    } else if (strcmp(argv[1], "logs") == 0 && argc >= 3) {
        char path[512];
        snprintf(path, sizeof(path), "logs/%s.log", argv[2]);
        execlp("cat", "cat", path, NULL);
        return 0;
    }

    if (write(fd, &req, sizeof(req)) < 0) perror("write to fifo");
    close(fd);
    return 0;
}
