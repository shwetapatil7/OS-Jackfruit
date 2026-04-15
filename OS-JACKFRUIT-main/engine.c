/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
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
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define DEVICE_NAME "container_monitor"
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
    int log_read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_ctx_t;

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
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
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
    char path[PATH_MAX];
    struct stat st = {0};

    if (stat(LOG_DIR, &st) == -1) {
        mkdir(LOG_DIR, 0700);
    }

    while (bounded_buffer_pop(buffer, &item) == 0) {
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t written = 0;
            while (written < item.length) {
                ssize_t res = write(fd, item.data + written, item.length - written);
                if (res <= 0) break;
                written += res;
            }
            close(fd);
        }
    }
    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    // Isolate mount namespace
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot failed");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir failed");
        return 1;
    }

    if (mount("proc", "/proc", "proc", MS_NOSUID|MS_NODEV|MS_NOEXEC, NULL) != 0) {
        perror("warning: mount /proc failed");
    }

    if (cfg->nice_value != 0) {
        if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) != 0) {
            perror("setpriority failed");
        }
    }

    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    char *argv_ch[64];
    int argc_ch = 0;
    char *token = strtok(cfg->command, " ");
    while (token != NULL && argc_ch < 63) {
        argv_ch[argc_ch++] = token;
        token = strtok(NULL, " ");
    }
    argv_ch[argc_ch] = NULL;

    execvp(argv_ch[0], argv_ch);
    perror("execvp");
    return 1;
}

int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid,
                          unsigned long soft_limit, unsigned long hard_limit)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit;
    req.hard_limit_bytes = hard_limit;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) return -1;
    return 0;
}

static void *producer_thread(void *arg)
{
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    log_item_t item;
    ssize_t n;
    
    strncpy(item.container_id, ctx->container_id, CONTAINER_ID_LEN);
    
    while ((n = read(ctx->log_read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = n;
        if (bounded_buffer_push(ctx->buffer, &item) < 0) {
            break;
        }
    }
    close(ctx->log_read_fd);
    free(ctx);
    return NULL;
}

static volatile sig_atomic_t g_stop = 0;

static void handler_sig(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop = 1;
    }
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int wstatus;
    pid_t pid;
    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *curr = ctx->containers;
        while (curr) {
            if (curr->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    curr->exit_code = WEXITSTATUS(wstatus);
                    curr->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(wstatus)) {
                    curr->exit_signal = WTERMSIG(wstatus);
                    if (curr->state != CONTAINER_STOPPED) {
                        curr->state = CONTAINER_KILLED;
                    }
                }
                break;
            }
            curr = curr->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    (void)rootfs; // Not strictly using the base rootfs path inside daemon except defensively

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { perror("bounded_buffer_init"); pthread_mutex_destroy(&ctx.metadata_lock); return 1; }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sig;
    sigaction(SIGCHLD, &sa, NULL); // Catch chld to interrupt accept loop
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind"); return 1;
    }
    listen(ctx.server_fd, 10);

    ctx.monitor_fd = open("/dev/" DEVICE_NAME, O_RDWR,0);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr, "Warning: Could not open /dev/%s\n", DEVICE_NAME);
    }

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx.log_buffer);
    printf("Supervisor daemon running.\n");

    while (!g_stop) {
        reap_children(&ctx);
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        control_request_t req;
        ssize_t n = recv(client_fd, &req, sizeof(req), 0);
        if (n <= 0) {
            close(client_fd);
            continue;
        }

        control_response_t res;
        memset(&res, 0, sizeof(res));

        if (req.kind == CMD_START || req.kind == CMD_RUN) {
            int pipefd[2];
            pipe(pipefd);

            child_config_t *cfg = malloc(sizeof(child_config_t));
            strncpy(cfg->id, req.container_id, CONTAINER_ID_LEN);
            strncpy(cfg->rootfs, req.rootfs, PATH_MAX);
            strncpy(cfg->command, req.command, CHILD_COMMAND_LEN);
            cfg->nice_value = req.nice_value;
            cfg->log_write_fd = pipefd[1];

            char *stack = malloc(STACK_SIZE);
            char *stack_top = stack + STACK_SIZE;

            pid_t pid = clone(child_fn, stack_top, CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, cfg);
            close(pipefd[1]);

            if (pid > 0) {
                if (ctx.monitor_fd >= 0) {
                    register_with_monitor(ctx.monitor_fd, req.container_id, pid, req.soft_limit_bytes, req.hard_limit_bytes);
                }

                producer_ctx_t *prod = malloc(sizeof(producer_ctx_t));
                prod->log_read_fd = pipefd[0];
                prod->buffer = &ctx.log_buffer;
                strncpy(prod->container_id, req.container_id, CONTAINER_ID_LEN);
                
                pthread_t pt;
                pthread_create(&pt, NULL, producer_thread, prod);
                pthread_detach(pt);

                container_record_t *rec = malloc(sizeof(container_record_t));
                memset(rec, 0, sizeof(*rec));
                strncpy(rec->id, req.container_id, CONTAINER_ID_LEN);
                rec->host_pid = pid;
                rec->started_at = time(NULL);
                rec->state = CONTAINER_RUNNING;
                rec->soft_limit_bytes = req.soft_limit_bytes;
                rec->hard_limit_bytes = req.hard_limit_bytes;
                snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req.container_id);

                pthread_mutex_lock(&ctx.metadata_lock);
                rec->next = ctx.containers;
                ctx.containers = rec;
                pthread_mutex_unlock(&ctx.metadata_lock);

                res.status = 0;
                snprintf(res.message, CONTROL_MESSAGE_LEN, "Successfully launched '%s' with PID %d", req.container_id, pid);
                
                if (req.kind == CMD_RUN) {
                    // Block execution until exit
                    while (!g_stop) {
                        reap_children(&ctx);
                        int exited = 0;
                        int current_status = 0;
                        
                        pthread_mutex_lock(&ctx.metadata_lock);
                        container_record_t *curr = ctx.containers;
                        while (curr) {
                            if (curr->host_pid == pid) {
                                if (curr->state == CONTAINER_EXITED || curr->state == CONTAINER_KILLED || curr->state == CONTAINER_STOPPED) {
                                    exited = 1;
                                    current_status = (curr->state == CONTAINER_EXITED) ? curr->exit_code : (128 + curr->exit_signal);
                                }
                                break;
                            }
                            curr = curr->next;
                        }
                        pthread_mutex_unlock(&ctx.metadata_lock);
                        
                        if (exited) {
                            res.status = current_status;
                            break;
                        }
                        usleep(100000); // Poll every 100ms
                    }
                }
            } else {
                res.status = 1;
                snprintf(res.message, CONTROL_MESSAGE_LEN, "Clone failed");
                close(pipefd[0]);
                free(stack);
                free(cfg);
            }
        }
        else if (req.kind == CMD_PS) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *curr = ctx.containers;
            char *p = res.message;
            int rem = CONTROL_MESSAGE_LEN - 1;
            int n_chars = snprintf(p, rem, "CONTAINER ID\tHOST PID\tSTATE\t\tHARD LIMIT\n");
            p += n_chars; rem -= n_chars;
            while (curr && rem > 0) {
                n_chars = snprintf(p, rem, "%s\t%d\t\t%s\t\t%lu\n", 
                                   curr->id, curr->host_pid, state_to_string(curr->state), curr->hard_limit_bytes);
                if (n_chars > rem) break;
                p += n_chars; rem -= n_chars;
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }
        else if (req.kind == CMD_LOGS) {
            char log_p[PATH_MAX] = "";
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *curr = ctx.containers;
            while (curr) {
                if (strcmp(curr->id, req.container_id) == 0) {
                    strncpy(log_p, curr->log_path, PATH_MAX);
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            
            if (log_p[0]) {
                snprintf(res.message, CONTROL_MESSAGE_LEN, "Log file located at: %s", log_p);
            } else {
                snprintf(res.message, CONTROL_MESSAGE_LEN, "Container '%s' not found.", req.container_id);
            }
        }
        else if (req.kind == CMD_STOP) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *curr = ctx.containers;
            int handled = 0;
            while (curr) {
                if (strcmp(curr->id, req.container_id) == 0 && curr->state == CONTAINER_RUNNING) {
                    curr->state = CONTAINER_STOPPED;
                    kill(curr->host_pid, SIGTERM);
                    handled = 1;
                    if (ctx.monitor_fd >= 0) {
                        unregister_from_monitor(ctx.monitor_fd, curr->id, curr->host_pid);
                    }
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            
            if (handled) {
                snprintf(res.message, CONTROL_MESSAGE_LEN, "Sent SIGTERM to container '%s'", req.container_id);
            } else {
                snprintf(res.message, CONTROL_MESSAGE_LEN, "Container '%s' not found or not running", req.container_id);
            }
        }

        send(client_fd, &res, sizeof(res), 0);
        close(client_fd);
    }

    printf("\nSupervisor shutting down...\n");
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    container_record_t *cr = ctx.containers;
    while (cr) {
        if (cr->state == CONTAINER_RUNNING) {
            kill(cr->host_pid, SIGKILL);
        }
        container_record_t *nxt = cr->next;
        free(cr);
        cr = nxt;
    }
    
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("Failed to connect to supervisor (is it running?)");
        close(sock);
        return 1;
    }

    if (send(sock, req, sizeof(*req), 0) <= 0) {
        perror("send"); close(sock); return 1;
    }

    control_response_t res;
    if (recv(sock, &res, sizeof(res), 0) > 0) {
        if (strlen(res.message) > 0) {
            printf("%s\n", res.message);
        }
        close(sock);
        return res.status;
    }
    
    close(sock);
    return 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        usage(argv[0]); return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        usage(argv[0]); return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    // Client-side block handled cleanly by supervisor not closing socket until container exit.
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        usage(argv[0]); return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    
    // We send request, and it tells us the log path
    if (send_control_request(&req) == 0) {
        // Output from server told us where the log is, but we can also just cat it!
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char buf[1024];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0) {
                write(STDOUT_FILENO, buf, n);
            }
            close(fd);
        }
    }
    return 0;
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        usage(argv[0]); return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
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

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0) return cmd_ps();
    if (strcmp(argv[1], "logs") == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
