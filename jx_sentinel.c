#include "jx_sentinel.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fanotify.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void warn_errno(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "jx-sentinel: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    va_end(ap);
}

void jx_print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--config FILE] [--verbose] [--no-notify] [--notify-user USER] [--ext EXT]... DIR...\n"
            "\n"
            "Monitor direct children of DIR with fanotify and log creation/move-in events.\n"
            "\n"
            "Options:\n"
            "  --config FILE       Load JX Sentinel config file.\n"
            "  --ext EXT           Log files with this extension, for example .conf. Repeatable.\n"
            "  --notify-user USER  Send GNOME notifications as USER via notify-send.\n"
            "  --no-notify         Disable desktop notifications.\n"
            "  --verbose           Print startup diagnostics to stderr.\n"
            "  --help              Show this help.\n",
            program);
}

static int add_string(char **items, size_t *count, size_t max, const char *value)
{
    if (*count >= max) {
        return -1;
    }
    items[*count] = strdup(value);
    if (!items[*count]) {
        return -1;
    }
    (*count)++;
    return 0;
}

static char *trim_space(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }

    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }

    return s;
}

static int parse_bool_value(const char *value, int *out)
{
    if (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int string_exists(char **items, size_t count, const char *value)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static int add_unique_string(char **items, size_t *count, size_t max, const char *value)
{
    if (string_exists(items, *count, value)) {
        return 0;
    }
    return add_string(items, count, max, value);
}

static int load_config_file(const char *path, struct jx_options *opts)
{
    FILE *fp = fopen(path, "re");
    if (!fp) {
        warn_errno("cannot open config file %s", path);
        return -1;
    }

    char line[4096];
    int in_section = 0;
    unsigned long line_no = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        char *trimmed = trim_space(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        if (trimmed[0] == '[') {
            in_section = strcmp(trimmed, "[JX Sentinel]") == 0;
            continue;
        }
        if (!in_section) {
            continue;
        }

        char *eq = strchr(trimmed, '=');
        if (!eq) {
            fprintf(stderr, "jx-sentinel: invalid config line %lu in %s\n", line_no, path);
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        char *key = trim_space(trimmed);
        char *value = trim_space(eq + 1);

        if (strcmp(key, "NotifyUser") == 0) {
            free(opts->notify_user);
            opts->notify_user = strdup(value);
            if (!opts->notify_user) {
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "Notifications") == 0) {
            if (parse_bool_value(value, &opts->notify_enabled) != 0) {
                fprintf(stderr, "jx-sentinel: invalid Notifications value on line %lu\n", line_no);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "Verbose") == 0) {
            if (parse_bool_value(value, &opts->verbose) != 0) {
                fprintf(stderr, "jx-sentinel: invalid Verbose value on line %lu\n", line_no);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "WatchPath") == 0) {
            if (add_unique_string(opts->watch_paths, &opts->watch_count,
                                  JX_MAX_WATCHES, value) != 0) {
                fprintf(stderr, "jx-sentinel: too many WatchPath entries\n");
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "Extension") == 0) {
            if (add_unique_string(opts->extensions, &opts->extension_count,
                                  JX_MAX_EXTENSIONS, value) != 0) {
                fprintf(stderr, "jx-sentinel: too many Extension entries\n");
                fclose(fp);
                return -1;
            }
        }
    }

    fclose(fp);
    return 0;
}

int jx_parse_args(int argc, char **argv, struct jx_options *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->notify_enabled = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            jx_print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--config") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "jx-sentinel: --config requires a file path\n");
                return -1;
            }
            free(opts->config_path);
            opts->config_path = strdup(argv[i]);
            if (!opts->config_path) {
                return -1;
            }
            if (load_config_file(opts->config_path, opts) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--verbose") == 0) {
            opts->verbose = 1;
        } else if (strcmp(argv[i], "--no-notify") == 0) {
            opts->notify_enabled = 0;
            free(opts->notify_user);
            opts->notify_user = NULL;
        } else if (strcmp(argv[i], "--notify-user") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "jx-sentinel: --notify-user requires a username\n");
                return -1;
            }
            free(opts->notify_user);
            opts->notify_user = strdup(argv[i]);
            if (!opts->notify_user) {
                return -1;
            }
            opts->notify_enabled = 1;
        } else if (strcmp(argv[i], "--ext") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "jx-sentinel: --ext requires an extension\n");
                return -1;
            }
            if (add_string(opts->extensions, &opts->extension_count,
                           JX_MAX_EXTENSIONS, argv[i]) != 0) {
                fprintf(stderr, "jx-sentinel: too many extensions or allocation failure\n");
                return -1;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "jx-sentinel: unknown option: %s\n", argv[i]);
            return -1;
        } else {
            if (add_string(opts->watch_paths, &opts->watch_count,
                           JX_MAX_WATCHES, argv[i]) != 0) {
                fprintf(stderr, "jx-sentinel: too many watch paths or allocation failure\n");
                return -1;
            }
        }
    }

    return 0;
}

void jx_free_options(struct jx_options *opts)
{
    for (size_t i = 0; i < opts->extension_count; i++) {
        free(opts->extensions[i]);
    }
    for (size_t i = 0; i < opts->watch_count; i++) {
        free(opts->watch_paths[i]);
    }
    free(opts->notify_user);
    free(opts->config_path);
}

int jx_validate_options(const struct jx_options *opts)
{
    if (opts->watch_count == 0) {
        fprintf(stderr, "jx-sentinel: at least one directory is required\n");
        return -1;
    }

    for (size_t i = 0; i < opts->extension_count; i++) {
        const char *ext = opts->extensions[i];
        if (!ext || ext[0] != '.' || ext[1] == '\0') {
            fprintf(stderr, "jx-sentinel: extension must start with a dot and include a suffix: %s\n",
                    ext ? ext : "(null)");
            return -1;
        }
    }

    for (size_t i = 0; i < opts->watch_count; i++) {
        struct stat st;
        if (!opts->watch_paths[i] || opts->watch_paths[i][0] != '/') {
            fprintf(stderr, "jx-sentinel: watch path must be absolute: %s\n",
                    opts->watch_paths[i] ? opts->watch_paths[i] : "(null)");
            return -1;
        }
        if (stat(opts->watch_paths[i], &st) != 0) {
            warn_errno("cannot stat watch path %s", opts->watch_paths[i]);
            return -1;
        }
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "jx-sentinel: watch path is not a directory: %s\n",
                    opts->watch_paths[i]);
            return -1;
        }
    }

    return 0;
}

static ssize_t retry_read(int fd, void *buf, size_t len)
{
    for (;;) {
        ssize_t n = read(fd, buf, len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n;
    }
}

static void trim_newline(char *s)
{
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) {
        s[--len] = '\0';
    }
}

static void read_exe(pid_t pid, char *out, size_t out_len)
{
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)pid);
    ssize_t n;
    do {
        n = readlink(proc_path, out, out_len - 1);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        snprintf(out, out_len, "unknown");
        return;
    }
    out[n] = '\0';
}

static void read_cmdline(pid_t pid, char *out, size_t out_len)
{
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%ld/cmdline", (long)pid);
    int fd = open(proc_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(out, out_len, "unknown");
        return;
    }

    ssize_t n = retry_read(fd, out, out_len - 1);
    close(fd);
    if (n <= 0) {
        snprintf(out, out_len, "unknown");
        return;
    }

    out[n] = '\0';
    for (ssize_t i = 0; i < n - 1; i++) {
        if (out[i] == '\0') {
            out[i] = ' ';
        }
    }
    trim_newline(out);
}

static int read_uid(pid_t pid, uid_t *uid)
{
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%ld/status", (long)pid);
    FILE *fp = fopen(proc_path, "re");
    if (!fp) {
        return -1;
    }

    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            unsigned long real_uid;
            if (sscanf(line + 4, "%lu", &real_uid) == 1) {
                *uid = (uid_t)real_uid;
                found = 0;
            }
            break;
        }
    }

    fclose(fp);
    return found;
}

int jx_read_process_info(pid_t pid, struct jx_process_info *info)
{
    memset(info, 0, sizeof(*info));
    info->pid = pid;
    snprintf(info->exe, sizeof(info->exe), "unknown");
    snprintf(info->cmdline, sizeof(info->cmdline), "unknown");

    if (pid <= 0) {
        return -1;
    }

    read_exe(pid, info->exe, sizeof(info->exe));
    read_cmdline(pid, info->cmdline, sizeof(info->cmdline));
    if (read_uid(pid, &info->uid) == 0) {
        info->uid_known = 1;
    }

    return 0;
}

const char *jx_event_name(uint64_t mask)
{
    if (mask & FAN_CREATE) {
        return "CREATE";
    }
    if (mask & FAN_MOVED_TO) {
        return "MOVED_TO";
    }
    return "UNKNOWN";
}

int jx_path_has_allowed_extension(const struct jx_options *opts, const char *path, int is_dir)
{
    if (is_dir || opts->extension_count == 0) {
        return 1;
    }

    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot) {
        return 0;
    }

    for (size_t i = 0; i < opts->extension_count; i++) {
        if (strcmp(dot, opts->extensions[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static void append_escaped(char *dst, size_t dst_len, const char *src)
{
    size_t pos = strlen(dst);
    for (size_t i = 0; src[i] && pos + 2 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[pos++] = '\\';
            dst[pos++] = (char)c;
        } else if (c < 32 || c == 127) {
            dst[pos++] = '?';
        } else {
            dst[pos++] = (char)c;
        }
        dst[pos] = '\0';
    }
}

static void current_time_iso8601(char *out, size_t out_len)
{
    struct timespec ts;
    struct tm tm_utc;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || !gmtime_r(&ts.tv_sec, &tm_utc)) {
        snprintf(out, out_len, "unknown");
        return;
    }

    char base[32];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    snprintf(out, out_len, "%s.%03ldZ", base, ts.tv_nsec / 1000000L);
}

static void quoted_field(char *line, size_t line_len, const char *key, const char *value)
{
    strncat(line, " ", line_len - strlen(line) - 1);
    strncat(line, key, line_len - strlen(line) - 1);
    strncat(line, "=\"", line_len - strlen(line) - 1);
    append_escaped(line, line_len, value ? value : "unknown");
    strncat(line, "\"", line_len - strlen(line) - 1);
}

void jx_log_event(const char *event, const struct jx_process_info *proc,
                  const char *path, int is_dir)
{
    char time_buf[64];
    char pid_buf[32];
    char uid_buf[32];
    char line[8192];

    current_time_iso8601(time_buf, sizeof(time_buf));
    snprintf(pid_buf, sizeof(pid_buf), "%ld", (long)proc->pid);
    if (proc->uid_known) {
        snprintf(uid_buf, sizeof(uid_buf), "%lu", (unsigned long)proc->uid);
    } else {
        snprintf(uid_buf, sizeof(uid_buf), "unknown");
    }

    snprintf(line, sizeof(line), "%s", JX_LOG_PREFIX);
    quoted_field(line, sizeof(line), "time", time_buf);
    quoted_field(line, sizeof(line), "event", event);
    quoted_field(line, sizeof(line), "pid", pid_buf);
    quoted_field(line, sizeof(line), "uid", uid_buf);
    quoted_field(line, sizeof(line), "exe", proc->exe);
    quoted_field(line, sizeof(line), "cmd", proc->cmdline);
    quoted_field(line, sizeof(line), "path", path);
    quoted_field(line, sizeof(line), "type", is_dir ? "directory" : "file");
    puts(line);
    fflush(stdout);
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }

    return 0;
}

static int canonicalize_watch_path(const char *path, char *out, size_t out_len)
{
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        return -1;
    }
    if (strlen(resolved) >= out_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(out, resolved);
    return 0;
}

static int fill_watch_handle(struct jx_watch *watch)
{
    struct file_handle *fh = NULL;
    int mount_id = 0;
    size_t alloc_size = sizeof(struct file_handle) + MAX_HANDLE_SZ;

    fh = calloc(1, alloc_size);
    if (!fh) {
        return -1;
    }
    fh->handle_bytes = MAX_HANDLE_SZ;

    if (name_to_handle_at(AT_FDCWD, watch->path, fh, &mount_id, 0) != 0) {
        free(fh);
        return -1;
    }

    if (fh->handle_bytes > sizeof(watch->handle)) {
        free(fh);
        errno = EOVERFLOW;
        return -1;
    }

    watch->mount_id = mount_id;
    watch->handle_type = (unsigned int)fh->handle_type;
    watch->handle_bytes = (unsigned int)fh->handle_bytes;
    memcpy(watch->handle, fh->f_handle, fh->handle_bytes);
    free(fh);
    return 0;
}

static int setup_watches(int fan_fd, const struct jx_options *opts,
                         struct jx_watch *watches, size_t *watch_count)
{
    *watch_count = 0;
    for (size_t i = 0; i < opts->watch_count; i++) {
        struct jx_watch watch;
        memset(&watch, 0, sizeof(watch));

        if (canonicalize_watch_path(opts->watch_paths[i], watch.path, sizeof(watch.path)) != 0) {
            warn_errno("cannot resolve watch path %s", opts->watch_paths[i]);
            return -1;
        }

        if (fill_watch_handle(&watch) != 0) {
            warn_errno("cannot read file handle for watch path %s", watch.path);
            return -1;
        }

        uint64_t mask = FAN_CREATE | FAN_MOVED_TO | FAN_EVENT_ON_CHILD | FAN_ONDIR;
        if (fanotify_mark(fan_fd, FAN_MARK_ADD | FAN_MARK_ONLYDIR, mask,
                          AT_FDCWD, watch.path) != 0) {
            warn_errno("fanotify_mark failed for %s", watch.path);
            return -1;
        }

        watches[*watch_count] = watch;
        (*watch_count)++;
        if (opts->verbose) {
            fprintf(stderr, "jx-sentinel: watching %s\n", watch.path);
        }
    }

    return 0;
}

static const struct jx_watch *find_watch_for_handle(const struct jx_watch *watches,
                                                    size_t watch_count,
                                                    const struct file_handle *fh)
{
    if (!fh) {
        return NULL;
    }

    for (size_t i = 0; i < watch_count; i++) {
        if (watches[i].handle_type == (unsigned int)fh->handle_type &&
            watches[i].handle_bytes == (unsigned int)fh->handle_bytes &&
            memcmp(watches[i].handle, fh->f_handle, fh->handle_bytes) == 0) {
            return &watches[i];
        }
    }

    return NULL;
}

static int build_event_path(const struct fanotify_event_metadata *metadata,
                            const struct jx_watch *watches, size_t watch_count,
                            char *out, size_t out_len)
{
    const char *event_end = (const char *)metadata + metadata->event_len;
    const char *ptr = (const char *)metadata + metadata->metadata_len;

    while (ptr + sizeof(struct fanotify_event_info_header) <= event_end) {
        const struct fanotify_event_info_header *hdr =
            (const struct fanotify_event_info_header *)ptr;
        if (hdr->len < sizeof(*hdr) || ptr + hdr->len > event_end) {
            break;
        }

        if (hdr->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
            const struct fanotify_event_info_fid *fid =
                (const struct fanotify_event_info_fid *)ptr;
            if (hdr->len < sizeof(*fid) + sizeof(struct file_handle)) {
                return -1;
            }

            const struct file_handle *fh = (const struct file_handle *)fid->handle;
            size_t fixed = sizeof(*fid) + sizeof(struct file_handle);
            if (fh->handle_bytes > MAX_HANDLE_SZ || fixed + fh->handle_bytes >= hdr->len) {
                return -1;
            }

            const char *name = (const char *)fh->f_handle + fh->handle_bytes;
            const char *name_end = memchr(name, '\0', hdr->len - fixed - fh->handle_bytes);
            if (!name_end || name_end == name) {
                return -1;
            }

            const struct jx_watch *watch = find_watch_for_handle(watches, watch_count, fh);
            if (!watch) {
                return -1;
            }

            int n = snprintf(out, out_len, "%s/%s", watch->path, name);
            return (n > 0 && (size_t)n < out_len) ? 0 : -1;
        }

        ptr += hdr->len;
    }

    return -1;
}

static pid_t pid_from_pidfd_info(int pidfd)
{
    if (pidfd < 0) {
        return -1;
    }

    char fdinfo_path[64];
    snprintf(fdinfo_path, sizeof(fdinfo_path), "/proc/self/fdinfo/%d", pidfd);
    FILE *fp = fopen(fdinfo_path, "re");
    if (!fp) {
        return -1;
    }

    char line[256];
    pid_t pid = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Pid:", 4) == 0) {
            long value = -1;
            if (sscanf(line + 4, "%ld", &value) == 1 && value > 0) {
                pid = (pid_t)value;
            }
            break;
        }
    }

    fclose(fp);
    return pid;
}

static pid_t event_pid(const struct fanotify_event_metadata *metadata)
{
    if (metadata->pid > 0) {
        return metadata->pid;
    }

    const char *event_end = (const char *)metadata + metadata->event_len;
    const char *ptr = (const char *)metadata + metadata->metadata_len;

    while (ptr + sizeof(struct fanotify_event_info_header) <= event_end) {
        const struct fanotify_event_info_header *hdr =
            (const struct fanotify_event_info_header *)ptr;
        if (hdr->len < sizeof(*hdr) || ptr + hdr->len > event_end) {
            break;
        }

        if (hdr->info_type == FAN_EVENT_INFO_TYPE_PIDFD &&
            hdr->len >= sizeof(struct fanotify_event_info_pidfd)) {
            const struct fanotify_event_info_pidfd *pidfd_info =
                (const struct fanotify_event_info_pidfd *)ptr;
            return pid_from_pidfd_info(pidfd_info->pidfd);
        }

        ptr += hdr->len;
    }

    return metadata->pid;
}

static void close_event_fds(const struct fanotify_event_metadata *metadata)
{
    if (metadata->fd >= 0) {
        close(metadata->fd);
    }

    const char *event_end = (const char *)metadata + metadata->event_len;
    const char *ptr = (const char *)metadata + metadata->metadata_len;

    while (ptr + sizeof(struct fanotify_event_info_header) <= event_end) {
        const struct fanotify_event_info_header *hdr =
            (const struct fanotify_event_info_header *)ptr;
        if (hdr->len < sizeof(*hdr) || ptr + hdr->len > event_end) {
            break;
        }

        if (hdr->info_type == FAN_EVENT_INFO_TYPE_PIDFD &&
            hdr->len >= sizeof(struct fanotify_event_info_pidfd)) {
            const struct fanotify_event_info_pidfd *pidfd_info =
                (const struct fanotify_event_info_pidfd *)ptr;
            if (pidfd_info->pidfd >= 0) {
                close(pidfd_info->pidfd);
            }
        }

        ptr += hdr->len;
    }
}

static int path_is_directory(const char *path, uint64_t mask)
{
    if (mask & FAN_ONDIR) {
        return 1;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    return 0;
}

static void notify_event(const struct jx_options *opts, const char *event,
                         const struct jx_process_info *proc, const char *path, int is_dir)
{
    if (!opts->notify_enabled || !opts->notify_user) {
        return;
    }

    char summary[128];
    char body[9000];
    snprintf(summary, sizeof(summary), "jx-sentinel %s", event);
    snprintf(body, sizeof(body), "pid=%ld type=%s\n%s\n%s",
             (long)proc->pid, is_dir ? "directory" : "file", path, proc->exe);
    (void)jx_send_notification(opts->notify_user, summary, body);
}

static void process_event(const struct jx_options *opts,
                          const struct fanotify_event_metadata *metadata,
                          const struct jx_watch *watches, size_t watch_count)
{
    if (metadata->vers != FANOTIFY_METADATA_VERSION) {
        fprintf(stderr, "jx-sentinel: fanotify metadata version mismatch\n");
        g_stop = 1;
        return;
    }

    if (metadata->mask & FAN_Q_OVERFLOW) {
        fprintf(stderr, "jx-sentinel: fanotify queue overflow\n");
        return;
    }

    if (!(metadata->mask & (FAN_CREATE | FAN_MOVED_TO))) {
        return;
    }

    char path[PATH_MAX];
    if (build_event_path(metadata, watches, watch_count, path, sizeof(path)) != 0) {
        snprintf(path, sizeof(path), "unknown");
    }

    int is_dir = path_is_directory(path, metadata->mask);
    if (!jx_path_has_allowed_extension(opts, path, is_dir)) {
        return;
    }

    struct jx_process_info proc;
    jx_read_process_info(event_pid(metadata), &proc);

    const char *event = jx_event_name(metadata->mask);
    jx_log_event(event, &proc, path, is_dir);
    notify_event(opts, event, &proc, path, is_dir);
}

static int event_loop(int fan_fd, const struct jx_options *opts,
                      const struct jx_watch *watches, size_t watch_count)
{
    char buffer[JX_READ_BUFFER_SIZE] __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));
    struct pollfd pfd = {
        .fd = fan_fd,
        .events = POLLIN
    };

    while (!g_stop) {
        int ready = poll(&pfd, 1, 1000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            warn_errno("poll failed");
            return -1;
        }
        if (ready == 0) {
            continue;
        }

        for (;;) {
            ssize_t len = read(fan_fd, buffer, sizeof(buffer));
            if (len < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN) {
                    break;
                }
                warn_errno("fanotify read failed");
                return -1;
            }
            if (len == 0) {
                break;
            }

            struct fanotify_event_metadata *metadata =
                (struct fanotify_event_metadata *)buffer;
            ssize_t remaining = len;
            while (FAN_EVENT_OK(metadata, remaining)) {
                process_event(opts, metadata, watches, watch_count);
                close_event_fds(metadata);
                metadata = FAN_EVENT_NEXT(metadata, remaining);
            }
            if (remaining != 0) {
                fprintf(stderr, "jx-sentinel: short or malformed fanotify event read\n");
            }
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct jx_options opts;
    if (jx_parse_args(argc, argv, &opts) != 0 || jx_validate_options(&opts) != 0) {
        jx_print_usage(argv[0]);
        jx_free_options(&opts);
        return 2;
    }

    if (install_signal_handlers() != 0) {
        warn_errno("failed to install signal handlers");
        jx_free_options(&opts);
        return 1;
    }

    int fan_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK |
                                   FAN_REPORT_DFID_NAME | FAN_REPORT_PIDFD,
                               O_RDONLY | O_LARGEFILE | O_CLOEXEC);
    if (fan_fd < 0 && (errno == EINVAL || errno == EPERM)) {
        fan_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_NONBLOCK |
                                   FAN_REPORT_DFID_NAME,
                               O_RDONLY | O_LARGEFILE | O_CLOEXEC);
    }
    if (fan_fd < 0) {
        warn_errno("fanotify_init failed; run as root with CAP_SYS_ADMIN");
        jx_free_options(&opts);
        return 1;
    }

    struct jx_watch watches[JX_MAX_WATCHES];
    size_t watch_count = 0;
    if (setup_watches(fan_fd, &opts, watches, &watch_count) != 0) {
        close(fan_fd);
        jx_free_options(&opts);
        return 1;
    }

    if (opts.verbose) {
        fprintf(stderr,
                "jx-sentinel: started with %zu watch(es), %zu extension filter(s), notify=%s\n",
                watch_count, opts.extension_count, opts.notify_enabled ? "on" : "off");
    }

    int rc = event_loop(fan_fd, &opts, watches, watch_count);

    close(fan_fd);
    jx_free_options(&opts);
    return rc == 0 ? 0 : 1;
}
