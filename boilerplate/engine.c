#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/select.h>
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
    int stop_requested;
    int producer_finished;
    char log_path[PATH_MAX];
    pthread_cond_t exit_cv;
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

typedef struct {
    supervisor_ctx_t *ctx;
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
} producer_arg_t;

static volatile sig_atomic_t g_supervisor_stop = 0;
static volatile sig_atomic_t g_sigchld_seen = 0;

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

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

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

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

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

static container_record_t *find_container_by_id_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (strncmp(cur->id, id, sizeof(cur->id)) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static container_record_t *find_container_by_pid_locked(supervisor_ctx_t *ctx, pid_t pid)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (cur->host_pid == pid)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static void write_all_fd(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
}

static void send_formatted(int fd, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    write_all_fd(fd, buf, (size_t)n);
}

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX] = {0};
        int fd;

        pthread_mutex_lock(&ctx->metadata_lock);
        {
            container_record_t *rec = find_container_by_id_locked(ctx, item.container_id);
            if (rec)
                strncpy(path, rec->log_path, sizeof(path) - 1);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (path[0] == '\0')
            continue;

        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write_all_fd(fd, item.data, item.length);
            close(fd);
        }
    }

    return NULL;
}

static int ensure_directory(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 0;
        errno = ENOTDIR;
        return -1;
    }

    return mkdir(path, 0755);
}

static void mark_producer_finished(supervisor_ctx_t *ctx, const char *container_id)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    {
        container_record_t *rec = find_container_by_id_locked(ctx, container_id);
        if (rec)
            rec->producer_finished = 1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void *producer_thread(void *arg)
{
    producer_arg_t *parg = (producer_arg_t *)arg;
    char buf[LOG_CHUNK_SIZE];

    while (1) {
        ssize_t n = read(parg->read_fd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, parg->container_id, sizeof(item.container_id) - 1);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        if (bounded_buffer_push(&parg->ctx->log_buffer, &item) != 0)
            break;
    }

    close(parg->read_fd);
    mark_producer_finished(parg->ctx, parg->container_id);
    free(parg);
    return NULL;
}

static void handle_signal(int signo)
{
    if (signo == SIGCHLD)
        g_sigchld_seen = 1;
    else
        g_supervisor_stop = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handle_signal;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int safe_mount_proc(void)
{
    if (mkdir("/proc", 0555) < 0 && errno != EEXIST)
        return -1;
    if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY)
        return -1;
    return 0;
}

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (sethostname(cfg->id, strlen(cfg->id)) < 0) {
        perror("sethostname");
        return 1;
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount-private");
        return 1;
    }

    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    if (safe_mount_proc() < 0)
        perror("mount proc");

    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }

    close(cfg->log_write_fd);

    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 127;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    if (monitor_fd < 0)
        return 0;

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

    if (monitor_fd < 0)
        return 0;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static void set_response(control_response_t *resp, int status, const char *fmt, ...)
{
    va_list ap;
    resp->status = status;
    va_start(ap, fmt);
    vsnprintf(resp->message, sizeof(resp->message), fmt, ap);
    va_end(ap);
}

static int start_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           control_response_t *resp)
{
    container_record_t *rec;
    child_config_t *cfg;
    void *stack;
    int pipefd[2];
    pid_t pid;
    producer_arg_t *parg;
    pthread_t producer_tid;

    if (ensure_directory(LOG_DIR) < 0) {
        set_response(resp, -1, "failed to create logs directory: %s", strerror(errno));
        return -1;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_by_id_locked(ctx, req->container_id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        set_response(resp, -1, "container id already exists");
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    rec = (container_record_t *)calloc(1, sizeof(*rec));
    cfg = (child_config_t *)calloc(1, sizeof(*cfg));
    stack = malloc(STACK_SIZE);
    if (!rec || !cfg || !stack) {
        free(rec);
        free(cfg);
        free(stack);
        set_response(resp, -1, "out of memory");
        return -1;
    }

    if (pipe(pipefd) < 0) {
        free(rec);
        free(cfg);
        free(stack);
        set_response(resp, -1, "pipe failed: %s", strerror(errno));
        return -1;
    }

    memset(rec, 0, sizeof(*rec));
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->started_at = time(NULL);
    rec->state = CONTAINER_STARTING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->exit_code = -1;
    rec->exit_signal = 0;
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req->container_id);
    pthread_cond_init(&rec->exit_cv, NULL);

    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id, req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command, sizeof(cfg->command) - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    pid = clone(child_fn,
                (char *)stack + STACK_SIZE,
                CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                cfg);
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        pthread_cond_destroy(&rec->exit_cv);
        free(rec);
        free(cfg);
        free(stack);
        set_response(resp, -1, "clone failed: %s", strerror(errno));
        return -1;
    }

    close(pipefd[1]);
    free(cfg);
    free(stack);

    rec->host_pid = pid;
    rec->state = CONTAINER_RUNNING;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (register_with_monitor(ctx->monitor_fd, rec->id, rec->host_pid,
                              rec->soft_limit_bytes, rec->hard_limit_bytes) < 0) {
        /* non-fatal for runtime demos */
    }

    parg = (producer_arg_t *)calloc(1, sizeof(*parg));
    if (!parg) {
        close(pipefd[0]);
        set_response(resp, 0, "container started pid=%d (logging degraded)", pid);
        return 0;
    }

    parg->ctx = ctx;
    parg->read_fd = pipefd[0];
    strncpy(parg->container_id, req->container_id, sizeof(parg->container_id) - 1);

    if (pthread_create(&producer_tid, NULL, producer_thread, parg) == 0)
        pthread_detach(producer_tid);
    else {
        close(pipefd[0]);
        free(parg);
    }

    set_response(resp, 0, "container started pid=%d", pid);
    return 0;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        {
            container_record_t *rec = find_container_by_pid_locked(ctx, pid);
            if (rec) {
                if (WIFEXITED(status)) {
                    rec->exit_code = WEXITSTATUS(status);
                    rec->exit_signal = 0;
                    rec->state = rec->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    rec->exit_code = 128 + WTERMSIG(status);
                    rec->exit_signal = WTERMSIG(status);
                    rec->state = rec->stop_requested ? CONTAINER_STOPPED : CONTAINER_KILLED;
                }
                pthread_cond_broadcast(&rec->exit_cv);
                unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
    g_sigchld_seen = 0;
}

static void stop_all_containers(supervisor_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    {
        container_record_t *cur = ctx->containers;
        while (cur) {
            if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) {
                cur->stop_requested = 1;
                kill(cur->host_pid, SIGTERM);
            }
            cur = cur->next;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void free_all_metadata(supervisor_ctx_t *ctx)
{
    container_record_t *cur, *next;
    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    ctx->containers = NULL;
    pthread_mutex_unlock(&ctx->metadata_lock);

    while (cur) {
        next = cur->next;
        pthread_cond_destroy(&cur->exit_cv);
        free(cur);
        cur = next;
    }
}

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    control_response_t resp;
    pthread_mutex_lock(&ctx->metadata_lock);
    set_response(&resp, 0, "ok");
    write_all_fd(client_fd, &resp, sizeof(resp));
    send_formatted(client_fd, "ID\tPID\tSTATE\tSOFT\tHARD\tEXIT\tLOG\n");
    {
        container_record_t *cur = ctx->containers;
        while (cur) {
            send_formatted(client_fd,
                           "%s\t%d\t%s\t%lu\t%lu\t%d\t%s\n",
                           cur->id,
                           cur->host_pid,
                           state_to_string(cur->state),
                           cur->soft_limit_bytes,
                           cur->hard_limit_bytes,
                           cur->exit_code,
                           cur->log_path);
            cur = cur->next;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void handle_logs(supervisor_ctx_t *ctx, int client_fd, const char *id)
{
    control_response_t resp;
    char path[PATH_MAX] = {0};
    int fd;
    char buf[1024];
    ssize_t n;

    pthread_mutex_lock(&ctx->metadata_lock);
    {
        container_record_t *rec = find_container_by_id_locked(ctx, id);
        if (rec)
            strncpy(path, rec->log_path, sizeof(path) - 1);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (path[0] == '\0') {
        set_response(&resp, -1, "unknown container");
        write_all_fd(client_fd, &resp, sizeof(resp));
        return;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_response(&resp, -1, "failed to open log: %s", strerror(errno));
        write_all_fd(client_fd, &resp, sizeof(resp));
        return;
    }

    set_response(&resp, 0, "ok");
    write_all_fd(client_fd, &resp, sizeof(resp));
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write_all_fd(client_fd, buf, (size_t)n);
    close(fd);
}

static void handle_stop(supervisor_ctx_t *ctx, int client_fd, const char *id)
{
    control_response_t resp;
    pthread_mutex_lock(&ctx->metadata_lock);
    {
        container_record_t *rec = find_container_by_id_locked(ctx, id);
        if (!rec) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            set_response(&resp, -1, "unknown container");
            write_all_fd(client_fd, &resp, sizeof(resp));
            return;
        }
        if (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING) {
            rec->stop_requested = 1;
            kill(rec->host_pid, SIGTERM);
            set_response(&resp, 0, "stop signal sent");
        } else {
            set_response(&resp, -1, "container not running");
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    write_all_fd(client_fd, &resp, sizeof(resp));
}

static void handle_run_wait(supervisor_ctx_t *ctx, int client_fd, const char *id)
{
    control_response_t resp;
    pthread_mutex_lock(&ctx->metadata_lock);
    {
        container_record_t *rec = find_container_by_id_locked(ctx, id);
        while (rec && (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING))
            pthread_cond_wait(&rec->exit_cv, &ctx->metadata_lock);

        if (!rec)
            set_response(&resp, -1, "container disappeared");
        else
            set_response(&resp, rec->exit_code, "container finished exit=%d state=%s",
                         rec->exit_code, state_to_string(rec->state));
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    write_all_fd(client_fd, &resp, sizeof(resp));
}

static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    memset(&req, 0, sizeof(req));
    n = read(client_fd, &req, sizeof(req));
    if (n != (ssize_t)sizeof(req))
        return;

    switch (req.kind) {
    case CMD_START:
        start_container(ctx, &req, &resp);
        write_all_fd(client_fd, &resp, sizeof(resp));
        break;
    case CMD_RUN:
        if (start_container(ctx, &req, &resp) == 0)
            handle_run_wait(ctx, client_fd, req.container_id);
        else
            write_all_fd(client_fd, &resp, sizeof(resp));
        break;
    case CMD_PS:
        handle_ps(ctx, client_fd);
        break;
    case CMD_LOGS:
        handle_logs(ctx, client_fd, req.container_id);
        break;
    case CMD_STOP:
        handle_stop(ctx, client_fd, req.container_id);
        break;
    default:
        set_response(&resp, -1, "unknown command");
        write_all_fd(client_fd, &resp, sizeof(resp));
        break;
    }
}

static int create_server_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

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

    install_signal_handlers();

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    ctx.server_fd = create_server_socket();
    if (ctx.server_fd < 0) {
        perror("create_server_socket");
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    printf("Supervisor listening on %s\n", CONTROL_PATH);

    while (!g_supervisor_stop) {
        fd_set rfds;
        struct timeval tv;
        int ready;

        if (g_sigchld_seen)
            reap_children(&ctx);

        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ready = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        if (ready > 0 && FD_ISSET(ctx.server_fd, &rfds)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                handle_client(&ctx, client_fd);
                close(client_fd);
            }
        }
    }

    stop_all_containers(&ctx);
    sleep(1);
    reap_children(&ctx);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    free_all_metadata(&ctx);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;
    char buf[1024];

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    write_all_fd(fd, req, sizeof(*req));

    n = read(fd, &resp, sizeof(resp));
    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Failed to read supervisor response\n");
        close(fd);
        return 1;
    }

    if (resp.message[0] != '\0')
        fprintf(resp.status == 0 ? stdout : stderr, "%s\n", resp.message);

    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write_all_fd(STDOUT_FILENO, buf, (size_t)n);

    close(fd);
    return resp.status == 0 ? 0 : (resp.status > 0 ? resp.status : 1);
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
