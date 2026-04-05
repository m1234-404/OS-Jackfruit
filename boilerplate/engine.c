/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);

    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

void *logging_thread(void *arg)
{
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
    log_item_t item;

    while (1) {
        if (bounded_buffer_pop(buffer, &item) != 0) {
            break;
        }

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "logs/%s.log", item.container_id);

        FILE *fp = fopen(path, "a");
        if (fp != NULL) {
            fwrite(item.data, 1, item.length, fp);
            fclose(fp);
        }
    }

    return NULL;
}

/* ====================== FIX: Proper log reader thread ====================== */
typedef struct {
    int fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} reader_args_t;

void *log_reader_fn(void *arg)
{
    reader_args_t *args = (reader_args_t *)arg;
    char buffer[LOG_CHUNK_SIZE];

    while (1) {
        ssize_t r = read(args->fd, buffer, sizeof(buffer));
        if (r <= 0)
            break;

        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, args->container_id, CONTAINER_ID_LEN);
        memcpy(item.data, buffer, r);
        item.length = r;

        bounded_buffer_push(args->buffer, &item);
    }

    close(args->fd);
    free(args);
    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    if (sethostname(config->id, strlen(config->id)) < 0) {
        perror("sethostname");
        return 1;
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount private");
        return 1;
    }

    if (chroot(config->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    mkdir("/proc", 0555);

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        return 1;
    }

    if (dup2(config->log_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }

    if (dup2(config->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }

    close(config->log_write_fd);

    execlp("/bin/sh", "sh", "-c", config->command, NULL);

    perror("exec");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static volatile sig_atomic_t stop = 0;

static void handle_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    ctx.containers = NULL;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* FIX: open monitor with O_RDWR */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor");
    }

    const char *fifo_path = "/tmp/container_engine_fifo";
    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo");
        goto cleanup;
    }

    ctx.server_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    if (ctx.server_fd < 0) {
        perror("open fifo");
        goto cleanup;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    pthread_t logger_thread;
    if (pthread_create(&logger_thread, NULL, logging_thread, &ctx.log_buffer) != 0) {
        perror("pthread_create");
        goto cleanup;
    }

    printf("Supervisor running with rootfs: %s\n", rootfs);

    while (!stop) {

        control_request_t req;
        ssize_t n = read(ctx.server_fd, &req, sizeof(req));

        if (n == sizeof(req)) {

            if (req.kind == CMD_RUN) {

                printf("Starting container: %s\n", req.container_id);

                container_record_t *rec = calloc(1, sizeof(*rec));
                if (!rec) continue;

                strncpy(rec->id, req.container_id, CONTAINER_ID_LEN);
                rec->state = CONTAINER_STARTING;
                rec->started_at = time(NULL);

                child_config_t *config = calloc(1, sizeof(*config));
                if (!config) {
                    free(rec);
                    continue;
                }

                strcpy(config->id, req.container_id);
                strcpy(config->rootfs, req.rootfs);
                strcpy(config->command, req.command);

                int pipefd[2];
                if (pipe(pipefd) < 0) {
                    perror("pipe");
                    free(config);
                    free(rec);
                    continue;
                }

                config->log_write_fd = pipefd[1];

                char *stack = malloc(STACK_SIZE);
                if (!stack) {
                    perror("malloc stack");
                    close(pipefd[0]);
                    close(pipefd[1]);
                    free(config);
                    free(rec);
                    continue;
                }

                pid_t pid = clone(child_fn,
                                  stack + STACK_SIZE,
                                  CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                                  config);

                if (pid < 0) {
                    perror("clone");
                    free(stack);
                    close(pipefd[0]);
                    close(pipefd[1]);
                    free(config);
                    free(rec);
                    continue;
                }

                close(pipefd[1]);

                int read_fd = pipefd[0];

                pthread_t reader_thread;
                reader_args_t *args = malloc(sizeof(*args));
                if (args) {
                    args->fd = read_fd;
                    args->buffer = &ctx.log_buffer;
                    strncpy(args->container_id, req.container_id, CONTAINER_ID_LEN);

                    pthread_create(&reader_thread, NULL, log_reader_fn, args);
                    pthread_detach(reader_thread);
                } else {
                    close(read_fd);
                }

                rec->host_pid = pid;
                rec->state = CONTAINER_RUNNING;

                /* FIX: Register container with kernel monitor */
                if (ctx.monitor_fd >= 0) {
                    if (register_with_monitor(ctx.monitor_fd,
                                              req.container_id,
                                              pid,
                                              req.soft_limit_bytes,
                                              req.hard_limit_bytes) < 0) {
                        perror("register_with_monitor");
                    }
                }

                pthread_mutex_lock(&ctx.metadata_lock);
                rec->next = ctx.containers;
                ctx.containers = rec;
                pthread_mutex_unlock(&ctx.metadata_lock);
            }

            else if (req.kind == CMD_PS) {

                printf("\nID\tPID\tSTATE\n");

                pthread_mutex_lock(&ctx.metadata_lock);

                container_record_t *curr = ctx.containers;
                while (curr) {
                    printf("%s\t%d\t%s\n",
                           curr->id,
                           curr->host_pid,
                           state_to_string(curr->state));
                    curr = curr->next;
                }

                pthread_mutex_unlock(&ctx.metadata_lock);
            }
        }

        int status;
        pid_t pid;

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {

            printf("Reaped child %d\n", pid);

            pthread_mutex_lock(&ctx.metadata_lock);

            container_record_t *curr = ctx.containers;
            while (curr) {
                if (curr->host_pid == pid) {
                    curr->state = CONTAINER_EXITED;

                    /* FIX: unregister from kernel monitor */
                    if (ctx.monitor_fd >= 0) {
                        unregister_from_monitor(ctx.monitor_fd, curr->id, curr->host_pid);
                    }

                    if (WIFEXITED(status))
                        curr->exit_code = WEXITSTATUS(status);

                    if (WIFSIGNALED(status))
                        curr->exit_signal = WTERMSIG(status);

                    break;
                }
                curr = curr->next;
            }

            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        usleep(100000);
    }

    printf("Supervisor shutting down...\n");

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(logger_thread, NULL);

cleanup:
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    return 0;
}

static int send_control_request(const control_request_t *req)
{
    const char *fifo_path = "/tmp/container_engine_fifo";

    int fd = open(fifo_path, O_WRONLY);
    if (fd < 0) {
        perror("open fifo");
        return 1;
    }

    ssize_t n = write(fd, req, sizeof(*req));
    if (n != sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    printf("Requesting container list...\n");
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    mkdir("logs", 0755);

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}

