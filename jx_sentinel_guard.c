#include "jx_guard_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fanotify.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef FAN_OPEN_EXEC_PERM
#define FAN_OPEN_EXEC_PERM 0
#endif

#define JX_GUARD_MAX_PATHS 128
#define JX_GUARD_MAX_ALLOWLIST 128
#define JX_GUARD_READ_BUFFER (256 * 1024)
#define JX_GUARD_SESSION_CACHE_MAX 256
#define JX_GUARD_SESSION_CACHE_TTL 60
#define JX_GUARD_CREATE_CACHE_MAX 256
#define JX_GUARD_CREATE_CACHE_TTL 120

typedef enum GuardMode {
    MODE_OFF,
    MODE_LOG,
    MODE_AUDIT,
    MODE_PROMPT,
    MODE_STRICT,
    MODE_BLOCK
} GuardMode;

typedef struct GuardConfig {
    GuardMode mode;
    int prompt_timeout;
    int default_allow;
    char notify_user[128];
    char protected_paths[JX_GUARD_MAX_PATHS][PATH_MAX];
    char protected_mark_paths[JX_GUARD_MAX_PATHS][PATH_MAX];
    GuardMode protected_modes[JX_GUARD_MAX_PATHS];
    size_t protected_count;
    char allowlist[JX_GUARD_MAX_ALLOWLIST][PATH_MAX];
    size_t allowlist_count;
    char process_allowlist[JX_GUARD_MAX_ALLOWLIST][PATH_MAX];
    size_t process_allowlist_count;
} GuardConfig;

typedef struct ProcInfo {
    pid_t pid;
    pid_t ppid;
    uid_t uid;
    int uid_known;
    char exe[PATH_MAX];
    char cmdline[1024];
} ProcInfo;

typedef struct AccessEvent {
    unsigned long request_seq;
    char request_id[32];
    char path[PATH_MAX];
    char protected_root[PATH_MAX];
    char action[32];
    int write_intent;
    ProcInfo proc;
} AccessEvent;

typedef struct SessionDecision {
    char subject[PATH_MAX];
    uid_t uid;
    char protected_root[PATH_MAX];
    int allow;
    time_t expires_at;
} SessionDecision;

typedef struct CreateCandidate {
    char path[PATH_MAX];
    char protected_root[PATH_MAX];
    uid_t uid;
    int uid_known;
    time_t expires_at;
} CreateCandidate;

static volatile sig_atomic_t g_stop = 0;
static int g_agent_fd = -1;
static int g_listen_fd = -1;
static int g_suppress_path_resolved_log = 0;
static pthread_mutex_t g_agent_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_accept_thread;
static int g_accept_thread_started = 0;
static SessionDecision g_session_cache[JX_GUARD_SESSION_CACHE_MAX];
static CreateCandidate g_create_cache[JX_GUARD_CREATE_CACHE_MAX];

static void accept_agent(int listen_fd);
static int path_has_prefix(const char *path, const char *root);

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void warn_errno(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "jx-sentinel-guard: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    va_end(ap);
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

static int parse_bool_decision(const char *value, int *allow)
{
    if (strcasecmp(value, "allow") == 0 || strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0) {
        *allow = 1;
        return 0;
    }
    if (strcasecmp(value, "deny") == 0 || strcasecmp(value, "no") == 0 || strcmp(value, "0") == 0) {
        *allow = 0;
        return 0;
    }
    return -1;
}

static int path_exists_in_list(char items[][PATH_MAX], size_t count, const char *path)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

static int parse_mode_value(const char *value, GuardMode *mode)
{
    if (strcasecmp(value, "off") == 0) {
        *mode = MODE_OFF;
        return 0;
    }
    if (strcasecmp(value, "log") == 0) {
        *mode = MODE_LOG;
        return 0;
    }
    if (strcasecmp(value, "audit") == 0) {
        *mode = MODE_AUDIT;
        return 0;
    }
    if (strcasecmp(value, "prompt") == 0) {
        *mode = MODE_PROMPT;
        return 0;
    }
    if (strcasecmp(value, "strict") == 0 || strcasecmp(value, "deny") == 0) {
        *mode = MODE_STRICT;
        return 0;
    }
    if (strcasecmp(value, "block") == 0) {
        *mode = MODE_BLOCK;
        return 0;
    }
    return -1;
}

static void strip_quotes(char *value)
{
    size_t len = strlen(value);
    if (len >= 2 && ((value[0] == '"' && value[len - 1] == '"') ||
                     (value[0] == '\'' && value[len - 1] == '\''))) {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
    }
}

static void normalize_path_value(char *path)
{
    strip_quotes(path);
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

static int parent_dir_for_path(const char *path, char *out, size_t out_len)
{
    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", path);
    char *slash = strrchr(copy, '/');
    if (!slash || slash == copy) {
        snprintf(out, out_len, "/");
        return 0;
    }
    *slash = '\0';
    snprintf(out, out_len, "%s", copy);
    return 0;
}

static int add_protected_path_with_mode(GuardConfig *cfg, const char *path, GuardMode mode)
{
    if (!path || path[0] != '/' || cfg->protected_count >= JX_GUARD_MAX_PATHS) {
        return -1;
    }

    char configured[PATH_MAX];
    snprintf(configured, sizeof(configured), "%s", path);
    normalize_path_value(configured);

    char resolved[PATH_MAX];
    char stored_path[PATH_MAX];
    char mark_path[PATH_MAX];
    snprintf(stored_path, sizeof(stored_path), "%s", configured);
    snprintf(mark_path, sizeof(mark_path), "%s", configured);

    struct stat st;
    if (realpath(configured, resolved) && stat(resolved, &st) == 0) {
        snprintf(stored_path, sizeof(stored_path), "%s", resolved);
        if (S_ISDIR(st.st_mode)) {
            snprintf(mark_path, sizeof(mark_path), "%s", resolved);
        } else {
            parent_dir_for_path(resolved, mark_path, sizeof(mark_path));
        }
    } else {
        char parent[PATH_MAX];
        char resolved_parent[PATH_MAX];
        parent_dir_for_path(configured, parent, sizeof(parent));
        if (realpath(parent, resolved_parent)) {
            const char *base = strrchr(configured, '/');
            const char *name = base ? base + 1 : configured;
            size_t parent_len = strlen(resolved_parent);
            size_t name_len = strlen(name);
            if (parent_len + 1 + name_len < sizeof(stored_path)) {
                memcpy(stored_path, resolved_parent, parent_len);
                stored_path[parent_len] = '/';
                memcpy(stored_path + parent_len + 1, name, name_len + 1);
            }
            snprintf(mark_path, sizeof(mark_path), "%s", resolved_parent);
        }
    }

    if (path_exists_in_list(cfg->protected_paths, cfg->protected_count, stored_path)) {
        return 0;
    }
    snprintf(cfg->protected_paths[cfg->protected_count], PATH_MAX, "%s", stored_path);
    snprintf(cfg->protected_mark_paths[cfg->protected_count], PATH_MAX, "%s", mark_path);
    cfg->protected_modes[cfg->protected_count] = mode;
    cfg->protected_count++;
    if (!g_suppress_path_resolved_log && strcmp(configured, stored_path) != 0) {
        fprintf(stdout, "JX_GUARD_PATH_RESOLVED configured=\"%s\" resolved=\"%s\"\n", configured, stored_path);
    }
    return 0;
}

static int add_protected_path(GuardConfig *cfg, const char *path)
{
    return add_protected_path_with_mode(cfg, path, cfg->mode);
}

static int add_allowlist_path(GuardConfig *cfg, const char *path)
{
    if (!path || path[0] != '/' || cfg->allowlist_count >= JX_GUARD_MAX_ALLOWLIST) {
        return -1;
    }
    if (path_exists_in_list(cfg->allowlist, cfg->allowlist_count, path)) {
        return 0;
    }
    snprintf(cfg->allowlist[cfg->allowlist_count++], PATH_MAX, "%s", path);
    return 0;
}

static int process_allowlist_exists(const GuardConfig *cfg, const char *pattern)
{
    for (size_t i = 0; i < cfg->process_allowlist_count; i++) {
        if (strcmp(cfg->process_allowlist[i], pattern) == 0) {
            return 1;
        }
    }
    return 0;
}

static int add_process_allowlist(GuardConfig *cfg, const char *pattern)
{
    if (!pattern || !*pattern || cfg->process_allowlist_count >= JX_GUARD_MAX_ALLOWLIST) {
        return -1;
    }
    if (process_allowlist_exists(cfg, pattern)) {
        return 0;
    }
    snprintf(cfg->process_allowlist[cfg->process_allowlist_count++], PATH_MAX, "%s", pattern);
    return 0;
}

static void add_builtin_allowlist(GuardConfig *cfg)
{
    add_allowlist_path(cfg, "/usr/lib/systemd/systemd");
    add_allowlist_path(cfg, "/usr/bin/gnome-shell");
    add_allowlist_path(cfg, "/usr/bin/nautilus");
    add_allowlist_path(cfg, "/usr/libexec/gvfsd");
    add_allowlist_path(cfg, "/usr/libexec/tracker-miner-fs-3");
    add_allowlist_path(cfg, "/usr/bin/systemctl");
    add_allowlist_path(cfg, "/usr/bin/pkexec");
    add_allowlist_path(cfg, "/usr/bin/sudo");
    add_allowlist_path(cfg, "/opt/jx/libexec/jx-sentinel-helper");
    add_allowlist_path(cfg, "/opt/jx/bin/jx-sentinel-control");
    add_allowlist_path(cfg, "/opt/jx/bin/jx-permission-agent");
    add_allowlist_path(cfg, "/opt/jx/bin/jx-sentinel-applet");
    add_allowlist_path(cfg, "/opt/jx/sbin/jx-sentinel-guard");
    add_process_allowlist(cfg, "systemd");
    add_process_allowlist(cfg, "systemd-logind");
    add_process_allowlist(cfg, "dbus-daemon");
    add_process_allowlist(cfg, "gdm");
    add_process_allowlist(cfg, "gdm-session-worker");
    add_process_allowlist(cfg, "gnome-shell");
    add_process_allowlist(cfg, "gnome-session-binary");
    add_process_allowlist(cfg, "gnome-keyring-daemon");
    add_process_allowlist(cfg, "nautilus");
}

static void config_defaults(GuardConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = MODE_PROMPT;
    cfg->prompt_timeout = 40;
    cfg->default_allow = 1;
    snprintf(cfg->notify_user, sizeof(cfg->notify_user), "jackson");
    g_suppress_path_resolved_log = 1;
    add_protected_path(cfg, "/home/jackson/Desktop");
    add_protected_path(cfg, "/home/jackson/Documents");
    add_protected_path(cfg, "/home/jackson/Downloads");
    add_protected_path(cfg, "/home/jackson/Pictures");
    g_suppress_path_resolved_log = 0;
    add_builtin_allowlist(cfg);
}

static void add_comma_executables(GuardConfig *cfg, const char *value)
{
    char copy[4096];
    snprintf(copy, sizeof(copy), "%s", value);
    char *saveptr = NULL;
    for (char *item = strtok_r(copy, ",", &saveptr); item; item = strtok_r(NULL, ",", &saveptr)) {
        char *path = trim_space(item);
        normalize_path_value(path);
        add_allowlist_path(cfg, path);
    }
}

typedef struct PendingTarget {
    char paths[JX_GUARD_MAX_PATHS][PATH_MAX];
    size_t count;
    GuardMode mode;
    int mode_set;
} PendingTarget;

static void pending_target_reset(PendingTarget *target, GuardMode default_mode)
{
    memset(target, 0, sizeof(*target));
    target->mode = default_mode;
}

static void pending_target_add_paths(PendingTarget *target, const char *value)
{
    char copy[4096];
    snprintf(copy, sizeof(copy), "%s", value);
    char *saveptr = NULL;
    for (char *item = strtok_r(copy, ",", &saveptr); item; item = strtok_r(NULL, ",", &saveptr)) {
        if (target->count >= JX_GUARD_MAX_PATHS) {
            break;
        }
        char *path = trim_space(item);
        normalize_path_value(path);
        if (path[0] == '/') {
            snprintf(target->paths[target->count++], PATH_MAX, "%s", path);
        }
    }
}

static void flush_pending_target(GuardConfig *cfg, PendingTarget *target)
{
    GuardMode mode = target->mode_set ? target->mode : cfg->mode;
    for (size_t i = 0; i < target->count; i++) {
        add_protected_path_with_mode(cfg, target->paths[i], mode);
    }
    pending_target_reset(target, cfg->mode);
}

static int load_config(const char *path, GuardConfig *cfg)
{
    config_defaults(cfg);
    FILE *fp = fopen(path, "re");
    if (!fp) {
        warn_errno("cannot open config %s, using defaults", path);
        return -1;
    }

    cfg->protected_count = 0;
    cfg->allowlist_count = 0;
    cfg->process_allowlist_count = 0;
    char line[4096];
    enum {
        SECTION_NONE,
        SECTION_LEGACY,
        SECTION_GLOBAL,
        SECTION_TARGET,
        SECTION_ALLOWLIST
    } section = SECTION_NONE;
    PendingTarget target;
    pending_target_reset(&target, cfg->mode);
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim_space(line);
        if (*s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        if (*s == '[') {
            if (section == SECTION_TARGET) {
                flush_pending_target(cfg, &target);
            }
            if (strcmp(s, "[JX Sentinel Guard]") == 0) {
                section = SECTION_LEGACY;
            } else if (strcmp(s, "[Global]") == 0) {
                section = SECTION_GLOBAL;
            } else if (strncmp(s, "[Target:", 8) == 0) {
                section = SECTION_TARGET;
                pending_target_reset(&target, cfg->mode);
            } else if (strcmp(s, "[Allowlist]") == 0) {
                section = SECTION_ALLOWLIST;
            } else {
                section = SECTION_NONE;
            }
            continue;
        }
        if (section == SECTION_NONE) {
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim_space(s);
        char *value = trim_space(eq + 1);
        strip_quotes(value);
        if (section == SECTION_GLOBAL && strcmp(key, "DefaultMode") == 0) {
            parse_mode_value(value, &cfg->mode);
            target.mode = cfg->mode;
        } else if ((section == SECTION_LEGACY || section == SECTION_TARGET) && strcmp(key, "Mode") == 0) {
            GuardMode parsed;
            if (parse_mode_value(value, &parsed) == 0) {
                if (section == SECTION_TARGET) {
                    target.mode = parsed;
                    target.mode_set = 1;
                } else {
                    cfg->mode = parsed;
                }
            }
        } else if ((section == SECTION_LEGACY || section == SECTION_GLOBAL) && strcmp(key, "PromptTimeout") == 0) {
            int timeout = atoi(value);
            if (timeout > 0 && timeout <= 300) cfg->prompt_timeout = timeout;
        } else if ((section == SECTION_LEGACY || section == SECTION_GLOBAL) && strcmp(key, "DefaultDecision") == 0) {
            parse_bool_decision(value, &cfg->default_allow);
        } else if ((section == SECTION_LEGACY || section == SECTION_GLOBAL) && strcmp(key, "NotifyUser") == 0) {
            snprintf(cfg->notify_user, sizeof(cfg->notify_user), "%s", value);
        } else if (section == SECTION_LEGACY && strcmp(key, "ProtectedPath") == 0) {
            add_protected_path(cfg, value);
        } else if (section == SECTION_LEGACY && strcmp(key, "Allowlist") == 0) {
            add_allowlist_path(cfg, value);
        } else if (section == SECTION_LEGACY && strcmp(key, "ProcessAllowlist") == 0) {
            add_process_allowlist(cfg, value);
        } else if (section == SECTION_TARGET && strcmp(key, "Paths") == 0) {
            pending_target_add_paths(&target, value);
        } else if (section == SECTION_ALLOWLIST && strcmp(key, "Executables") == 0) {
            add_comma_executables(cfg, value);
        } else if (section == SECTION_ALLOWLIST && strcmp(key, "Processes") == 0) {
            char copy[4096];
            snprintf(copy, sizeof(copy), "%s", value);
            char *saveptr = NULL;
            for (char *item = strtok_r(copy, ",", &saveptr); item; item = strtok_r(NULL, ",", &saveptr)) {
                add_process_allowlist(cfg, trim_space(item));
            }
        }
    }
    if (section == SECTION_TARGET) {
        flush_pending_target(cfg, &target);
    }
    fclose(fp);
    add_builtin_allowlist(cfg);
    return 0;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGINT, &sa, NULL) == 0 && sigaction(SIGTERM, &sa, NULL) == 0 ? 0 : -1;
}

static void close_agent(void)
{
    pthread_mutex_lock(&g_agent_lock);
    if (g_agent_fd >= 0) {
        close(g_agent_fd);
        g_agent_fd = -1;
    }
    unlink(JX_GUARD_AGENT_STATE_PATH);
    pthread_mutex_unlock(&g_agent_lock);
}

static int current_agent_fd(void)
{
    pthread_mutex_lock(&g_agent_lock);
    int fd = g_agent_fd;
    pthread_mutex_unlock(&g_agent_lock);
    return fd;
}

static void close_agent_if_current(int fd)
{
    pthread_mutex_lock(&g_agent_lock);
    if (g_agent_fd == fd) {
        close(g_agent_fd);
        g_agent_fd = -1;
        unlink(JX_GUARD_AGENT_STATE_PATH);
    }
    pthread_mutex_unlock(&g_agent_lock);
}

static int retry_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        if (g_stop) {
            return -1;
        }
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static void read_exe(pid_t pid, char *out, size_t out_len)
{
    char p[64];
    snprintf(p, sizeof(p), "/proc/%ld/exe", (long)pid);
    ssize_t n = readlink(p, out, out_len - 1);
    if (n < 0) snprintf(out, out_len, "unknown");
    else out[n] = '\0';
}

static void read_cmdline(pid_t pid, char *out, size_t out_len)
{
    char p[64];
    snprintf(p, sizeof(p), "/proc/%ld/cmdline", (long)pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(out, out_len, "unknown");
        return;
    }
    ssize_t n;
    do {
        n = read(fd, out, out_len - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0) {
        snprintf(out, out_len, "unknown");
        return;
    }
    out[n] = '\0';
    for (ssize_t i = 0; i < n - 1; i++) {
        if (out[i] == '\0') out[i] = ' ';
    }
}

static void read_status(pid_t pid, ProcInfo *proc)
{
    char p[64];
    snprintf(p, sizeof(p), "/proc/%ld/status", (long)pid);
    FILE *fp = fopen(p, "re");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            unsigned long uid = 0;
            if (sscanf(line + 4, "%lu", &uid) == 1) {
                proc->uid = (uid_t)uid;
                proc->uid_known = 1;
            }
        } else if (strncmp(line, "PPid:", 5) == 0) {
            long ppid = 0;
            if (sscanf(line + 5, "%ld", &ppid) == 1) proc->ppid = (pid_t)ppid;
        }
    }
    fclose(fp);
}

static void read_proc_info(pid_t pid, ProcInfo *proc)
{
    memset(proc, 0, sizeof(*proc));
    proc->pid = pid;
    snprintf(proc->exe, sizeof(proc->exe), "unknown");
    snprintf(proc->cmdline, sizeof(proc->cmdline), "unknown");
    if (pid <= 0) return;
    read_exe(pid, proc->exe, sizeof(proc->exe));
    read_cmdline(pid, proc->cmdline, sizeof(proc->cmdline));
    read_status(pid, proc);
}

static int read_fd_path(int fd, char *out, size_t out_len)
{
    char p[64];
    snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, out, out_len - 1);
    if (n < 0) return -1;
    out[n] = '\0';
    return 0;
}

static int path_has_prefix(const char *path, const char *root)
{
    size_t n = strlen(root);
    return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static int protected_root_for(const GuardConfig *cfg, const char *path, char *root, size_t root_len)
{
    size_t best = 0;
    for (size_t i = 0; i < cfg->protected_count; i++) {
        size_t len = strlen(cfg->protected_paths[i]);
        if (len > best && path_has_prefix(path, cfg->protected_paths[i])) {
            best = len;
            strncpy(root, cfg->protected_paths[i], root_len - 1);
            root[root_len - 1] = '\0';
        }
    }
    return best > 0 ? 0 : -1;
}

static GuardMode protected_mode_for_root(const GuardConfig *cfg, const char *root)
{
    for (size_t i = 0; i < cfg->protected_count; i++) {
        if (strcmp(cfg->protected_paths[i], root) == 0) {
            return cfg->protected_modes[i];
        }
    }
    return cfg->mode;
}

static const char *basename_for_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void first_cmd_token(const char *cmdline, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", cmdline && *cmdline ? cmdline : "");
    char *space = strchr(out, ' ');
    if (space) {
        *space = '\0';
    }
}

static void policy_subject_for(const ProcInfo *proc, char *out, size_t out_len)
{
    if (proc->exe[0] && strcmp(proc->exe, "unknown") != 0) {
        snprintf(out, out_len, "%s", proc->exe);
        return;
    }

    char token[PATH_MAX];
    first_cmd_token(proc->cmdline, token, sizeof(token));
    if (token[0] && strcmp(token, "unknown") != 0) {
        const char *base = basename_for_path(token);
        snprintf(out, out_len, "cmd:%.*s", (int)(out_len > 5 ? out_len - 5 : 0), base);
        return;
    }

    snprintf(out, out_len, "pid:%ld", (long)proc->pid);
}

static int process_matches_allow_pattern(const ProcInfo *proc, const char *pattern)
{
    if (!pattern || !*pattern) {
        return 0;
    }
    if (pattern[0] == '/') {
        return strcmp(proc->exe, pattern) == 0;
    }

    if (proc->exe[0] && strcmp(proc->exe, "unknown") != 0) {
        if (!path_has_prefix(proc->exe, "/usr/bin") &&
            !path_has_prefix(proc->exe, "/usr/sbin") &&
            !path_has_prefix(proc->exe, "/usr/lib") &&
            !path_has_prefix(proc->exe, "/usr/libexec") &&
            !path_has_prefix(proc->exe, "/opt/jx/bin") &&
            !path_has_prefix(proc->exe, "/opt/jx/sbin") &&
            !path_has_prefix(proc->exe, "/opt/jx/libexec")) {
            return 0;
        }
        return strcmp(basename_for_path(proc->exe), pattern) == 0;
    }

    char token[PATH_MAX];
    first_cmd_token(proc->cmdline, token, sizeof(token));
    return strcmp(basename_for_path(token), pattern) == 0;
}

static int is_system_allowed(const GuardConfig *cfg, const ProcInfo *proc)
{
    for (size_t i = 0; i < cfg->allowlist_count; i++) {
        if (strcmp(cfg->allowlist[i], proc->exe) == 0) return 1;
    }
    for (size_t i = 0; i < cfg->process_allowlist_count; i++) {
        if (process_matches_allow_pattern(proc, cfg->process_allowlist[i])) return 1;
    }
    return 0;
}

static int is_guard_management_target(const char *path)
{
    static const char *targets[] = {
        "/opt/jx/libexec/jx-sentinel-helper",
        "/opt/jx/bin/jx-sentinel-control",
        "/opt/jx/bin/jx-permission-agent",
        "/opt/jx/bin/jx-sentinel-applet",
        "/opt/jx/sbin/jx-sentinel-guard",
        "/opt/jx/etc/jx-sentinel/guard.conf",
        "/opt/jx/var/lib/jx-sentinel/permissions.db",
        "/etc/systemd/system/jx-sentinel-guard.service"
    };
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        if (strcmp(path, targets[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int proc_exe_is(const ProcInfo *proc, const char *path)
{
    return proc->exe[0] && strcmp(proc->exe, path) == 0;
}

static int is_guard_management_process(const ProcInfo *proc)
{
    if (!proc->uid_known || proc->uid != 0) {
        return 0;
    }
    return proc_exe_is(proc, "/opt/jx/libexec/jx-sentinel-helper") ||
           proc_exe_is(proc, "/usr/bin/install");
}

static const char *action_from_mask(uint64_t mask)
{
    if (mask & FAN_OPEN_EXEC_PERM) return "execute";
    if (mask & FAN_OPEN_PERM) return "open";
    if (mask & FAN_ACCESS_PERM) return "access";
    if (mask & FAN_CLOSE_WRITE) return "write";
    if (mask & FAN_OPEN) return "open";
    if (mask & FAN_ACCESS) return "access";
    return "unknown";
}

static int action_is_read_only(const AccessEvent *ev)
{
    return !ev->write_intent &&
           (strcmp(ev->action, "open") == 0 || strcmp(ev->action, "access") == 0);
}

static int action_is_write_like(const AccessEvent *ev)
{
    return ev->write_intent || strcmp(ev->action, "open_write") == 0 ||
           strcmp(ev->action, "write") == 0 ||
           strcmp(ev->action, "execute") == 0;
}

static int fd_has_write_intent(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return 0;
    }
    int access_mode = flags & O_ACCMODE;
    return access_mode == O_WRONLY || access_mode == O_RDWR;
}

static int create_candidate_matches(const AccessEvent *ev)
{
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return 0;
    }

    for (size_t i = 0; i < JX_GUARD_CREATE_CACHE_MAX; i++) {
        CreateCandidate *entry = &g_create_cache[i];
        if (entry->expires_at <= now) {
            continue;
        }
        if (strcmp(entry->path, ev->path) != 0 ||
            strcmp(entry->protected_root, ev->protected_root) != 0) {
            continue;
        }
        if (entry->uid_known && ev->proc.uid_known &&
            entry->uid != ev->proc.uid && ev->proc.uid != 0) {
            continue;
        }
        return 1;
    }
    return 0;
}

static void remember_create_candidate(const AccessEvent *ev)
{
    if (!ev->path[0] || !ev->protected_root[0] || !path_has_prefix(ev->path, ev->protected_root)) {
        return;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return;
    }

    size_t slot = 0;
    time_t oldest = g_create_cache[0].expires_at;
    for (size_t i = 0; i < JX_GUARD_CREATE_CACHE_MAX; i++) {
        CreateCandidate *entry = &g_create_cache[i];
        if (entry->expires_at <= now || strcmp(entry->path, ev->path) == 0) {
            slot = i;
            break;
        }
        if (entry->expires_at < oldest) {
            oldest = entry->expires_at;
            slot = i;
        }
    }

    CreateCandidate *entry = &g_create_cache[slot];
    snprintf(entry->path, sizeof(entry->path), "%s", ev->path);
    snprintf(entry->protected_root, sizeof(entry->protected_root), "%s", ev->protected_root);
    entry->uid = ev->proc.uid;
    entry->uid_known = ev->proc.uid_known;
    entry->expires_at = now + JX_GUARD_CREATE_CACHE_TTL;
}

static void remember_create_candidate_if_empty_file(const AccessEvent *ev)
{
    if (!ev->path[0] || !ev->protected_root[0] || !path_has_prefix(ev->path, ev->protected_root)) {
        return;
    }

    struct stat st;
    if (lstat(ev->path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 0) {
        return;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1 || st.st_ctime + JX_GUARD_CREATE_CACHE_TTL < now ||
        st.st_mtime + JX_GUARD_CREATE_CACHE_TTL < now) {
        return;
    }

    remember_create_candidate(ev);
}

static void escape_value(char *dst, size_t dst_len, const char *src)
{
    size_t pos = strlen(dst);
    for (size_t i = 0; src && src[i] && pos + 2 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' || c == '\t' || c == '\n' || c == '\r') {
            dst[pos++] = '\\';
            dst[pos++] = c == '\t' ? 't' : c == '\n' ? 'n' : c == '\r' ? 'r' : '\\';
        } else if (c < 32 || c == 127) {
            dst[pos++] = '?';
        } else {
            dst[pos++] = (char)c;
        }
        dst[pos] = '\0';
    }
}

static void kv_append(char *line, size_t line_len, const char *key, const char *value)
{
    if (line[0]) strncat(line, "\t", line_len - strlen(line) - 1);
    strncat(line, key, line_len - strlen(line) - 1);
    strncat(line, "=", line_len - strlen(line) - 1);
    escape_value(line, line_len, value);
}

static void now_iso(char *out, size_t out_len)
{
    time_t t = time(NULL);
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%S%z", &tm_local);
}

static void log_decision(const AccessEvent *ev, const char *decision, const char *source, int allow)
{
    char time_buf[64];
    now_iso(time_buf, sizeof(time_buf));
    printf("JX_GUARD_DECISION time=\"%s\" pid=\"%ld\" uid=\"%lu\" exe=\"%s\" cmd=\"%s\" action=\"%s\" path=\"%s\" protected_root=\"%s\" decision=\"%s\" source=\"%s\" result=\"%s\"\n",
           time_buf, (long)ev->proc.pid, (unsigned long)ev->proc.uid, ev->proc.exe,
           ev->proc.cmdline, ev->action, ev->path, ev->protected_root, decision, source,
           allow ? "allow" : "deny");
    fflush(stdout);
}

static void ensure_db_parent(void)
{
    mkdir("/opt/jx", 0755);
    mkdir("/opt/jx/var", 0755);
    mkdir("/opt/jx/var/lib", 0755);
    mkdir("/opt/jx/var/lib/jx-sentinel", 0755);
}

static int policy_lookup(const AccessEvent *ev, int *allow)
{
    FILE *fp = fopen(JX_GUARD_DB_PATH, "re");
    if (!fp) return -1;
    char subject[PATH_MAX];
    policy_subject_for(&ev->proc, subject, sizeof(subject));
    char line[4096];
    int found = -1;
    while (fgets(line, sizeof(line), fp)) {
        char stored_subject[PATH_MAX], path[PATH_MAX], decision[16], scope[32];
        unsigned long uid = 0;
        if (sscanf(line, "%4095[^\t]\t%lu\t%4095[^\t]\t%15s\t%31s", stored_subject, &uid, path, decision, scope) != 5) {
            continue;
        }
        if (uid != (unsigned long)ev->proc.uid || strcmp(stored_subject, subject) != 0) {
            continue;
        }
        int path_match = strcmp(scope, "recursive") == 0 ? path_has_prefix(ev->protected_root, path) : strcmp(ev->protected_root, path) == 0;
        if (!path_match) continue;
        if (strcmp(decision, "allow") == 0) {
            *allow = 1;
            found = 0;
            break;
        }
        if (strcmp(decision, "deny") == 0) {
            *allow = 0;
            found = 0;
            break;
        }
    }
    fclose(fp);
    return found;
}

static void policy_store(const AccessEvent *ev, int allow)
{
    ensure_db_parent();
    FILE *fp = fopen(JX_GUARD_DB_PATH, "ae");
    if (!fp) {
        warn_errno("cannot open policy db %s", JX_GUARD_DB_PATH);
        return;
    }
    char subject[PATH_MAX];
    policy_subject_for(&ev->proc, subject, sizeof(subject));
    fprintf(fp, "%s\t%lu\t%s\t%s\trecursive\n",
            subject, (unsigned long)ev->proc.uid, ev->protected_root, allow ? "allow" : "deny");
    fclose(fp);
}

static int session_lookup(const AccessEvent *ev, int *allow)
{
    time_t now = time(NULL);
    char subject[PATH_MAX];
    policy_subject_for(&ev->proc, subject, sizeof(subject));
    for (size_t i = 0; i < JX_GUARD_SESSION_CACHE_MAX; i++) {
        SessionDecision *entry = &g_session_cache[i];
        if (entry->expires_at <= now) {
            continue;
        }
        if (entry->uid == ev->proc.uid &&
            strcmp(entry->subject, subject) == 0 &&
            strcmp(entry->protected_root, ev->protected_root) == 0) {
            *allow = entry->allow;
            return 0;
        }
    }
    return -1;
}

static void session_store(const AccessEvent *ev, int allow)
{
    time_t now = time(NULL);
    size_t slot = 0;
    time_t oldest = g_session_cache[0].expires_at;
    char subject[PATH_MAX];
    policy_subject_for(&ev->proc, subject, sizeof(subject));

    for (size_t i = 0; i < JX_GUARD_SESSION_CACHE_MAX; i++) {
        SessionDecision *entry = &g_session_cache[i];
        if (entry->expires_at <= now ||
            (entry->uid == ev->proc.uid &&
             strcmp(entry->subject, subject) == 0 &&
             strcmp(entry->protected_root, ev->protected_root) == 0)) {
            slot = i;
            break;
        }
        if (entry->expires_at < oldest) {
            oldest = entry->expires_at;
            slot = i;
        }
    }

    SessionDecision *entry = &g_session_cache[slot];
    snprintf(entry->subject, sizeof(entry->subject), "%s", subject);
    entry->uid = ev->proc.uid;
    snprintf(entry->protected_root, sizeof(entry->protected_root), "%s", ev->protected_root);
    entry->allow = allow;
    entry->expires_at = now + JX_GUARD_SESSION_CACHE_TTL;
}

static int response_decision(const char *line, const char *request_id, char *decision, size_t decision_len)
{
    char copy[JX_GUARD_MAX_LINE];
    snprintf(copy, sizeof(copy), "%s", line);
    int id_ok = 0;
    int decision_ok = 0;
    for (char *tok = strtok(copy, "\t\n"); tok; tok = strtok(NULL, "\t\n")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = tok;
        char *value = eq + 1;
        if (strcmp(key, "request_id") == 0 && strcmp(value, request_id) == 0) id_ok = 1;
        if (strcmp(key, "decision") == 0) {
            snprintf(decision, decision_len, "%s", value);
            decision_ok = 1;
        }
    }
    return id_ok && decision_ok ? 0 : -1;
}

static int read_agent_response(int fd, int timeout_sec, char *line, size_t line_len)
{
    size_t len = 0;
    while (len + 1 < line_len) {
        if (g_stop) {
            return -1;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, timeout_sec * 1000);
        if (ready < 0 && errno == EINTR) {
            if (g_stop) {
                return -1;
            }
            continue;
        }
        if (ready <= 0) return 0;
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) return -1;
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        line[len++] = c;
        if (c == '\n') {
            line[len] = '\0';
            return 1;
        }
    }
    line[len] = '\0';
    return -1;
}

static int ask_agent(const GuardConfig *cfg, const AccessEvent *ev, char *decision, size_t decision_len)
{
    if (g_stop) return -1;
    int agent_fd = current_agent_fd();
    if (agent_fd < 0) {
        if (g_listen_fd >= 0) {
            accept_agent(g_listen_fd);
        }
        agent_fd = current_agent_fd();
    }
    if (agent_fd < 0 && g_listen_fd >= 0) {
        for (int waited_ms = 0; waited_ms < 3000 && !g_stop && agent_fd < 0; waited_ms += 250) {
            struct pollfd pfd = {.fd = g_listen_fd, .events = POLLIN};
            int ready = poll(&pfd, 1, 250);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready > 0 && (pfd.revents & POLLIN)) {
                accept_agent(g_listen_fd);
            }
            agent_fd = current_agent_fd();
        }
    }
    if (agent_fd < 0) {
        fprintf(stdout, "JX_GUARD_AGENT_REQUEST request_id=\"%s\" status=\"no_agent\" exe=\"%s\" path=\"%s\"\n",
                ev->request_id, ev->proc.exe, ev->path);
        fflush(stdout);
        return -1;
    }
    char line[JX_GUARD_MAX_LINE] = "";
    char num[64];
    kv_append(line, sizeof(line), "type", "request");
    kv_append(line, sizeof(line), "request_id", ev->request_id);
    snprintf(num, sizeof(num), "%ld", (long)ev->proc.pid);
    kv_append(line, sizeof(line), "pid", num);
    snprintf(num, sizeof(num), "%lu", (unsigned long)ev->proc.uid);
    kv_append(line, sizeof(line), "uid", num);
    kv_append(line, sizeof(line), "exe", ev->proc.exe);
    kv_append(line, sizeof(line), "cmd", ev->proc.cmdline);
    kv_append(line, sizeof(line), "path", ev->path);
    kv_append(line, sizeof(line), "action", ev->action);
    kv_append(line, sizeof(line), "protected_root", ev->protected_root);
    strncat(line, "\n", sizeof(line) - strlen(line) - 1);

    fprintf(stdout, "JX_GUARD_AGENT_REQUEST request_id=\"%s\" status=\"sending\" exe=\"%s\" path=\"%s\"\n",
            ev->request_id, ev->proc.exe, ev->path);
    fflush(stdout);
    if (retry_write_all(agent_fd, line, strlen(line)) != 0) {
        fprintf(stdout, "JX_GUARD_AGENT_REQUEST request_id=\"%s\" status=\"write_failed\"\n", ev->request_id);
        fflush(stdout);
        close_agent_if_current(agent_fd);
        return -1;
    }

    for (;;) {
        char response[JX_GUARD_MAX_LINE];
        int rc = read_agent_response(agent_fd, cfg->prompt_timeout, response, sizeof(response));
        if (rc < 0) {
            fprintf(stdout, "JX_GUARD_AGENT_REQUEST request_id=\"%s\" status=\"read_failed\"\n", ev->request_id);
            fflush(stdout);
            close_agent_if_current(agent_fd);
            return -1;
        }
        if (rc == 0) {
            fprintf(stdout, "JX_GUARD_AGENT_REQUEST request_id=\"%s\" status=\"timeout\"\n", ev->request_id);
            fflush(stdout);
            return 0;
        }
        if (response_decision(response, ev->request_id, decision, decision_len) == 0) {
            break;
        }
        fprintf(stdout, "JX_GUARD_AGENT_REQUEST request_id=\"%s\" status=\"ignored_response\" line=\"%s\"\n",
                ev->request_id, response);
        fflush(stdout);
    }
    fprintf(stdout, "JX_GUARD_AGENT_REQUEST request_id=\"%s\" status=\"response\" decision=\"%s\"\n",
            ev->request_id, decision);
    fflush(stdout);
    return 1;
}

static int decide_event(const GuardConfig *cfg, AccessEvent *ev, const char **source, char *decision, size_t decision_len)
{
    GuardMode mode = protected_mode_for_root(cfg, ev->protected_root);
    if (g_stop) {
        snprintf(decision, decision_len, "allow_once");
        *source = "shutdown";
        return 1;
    }
    if (is_guard_management_target(ev->path)) {
        if (action_is_read_only(ev)) {
            snprintf(decision, decision_len, "allow_once");
            *source = "guard_read";
            return 1;
        }
        if (is_guard_management_process(&ev->proc)) {
            snprintf(decision, decision_len, "allow_once");
            *source = "guard_admin";
            return 1;
        }
        snprintf(decision, decision_len, "deny_once");
        *source = "guard_self_protect";
        return 0;
    }
    if (action_is_read_only(ev)) {
        snprintf(decision, decision_len, "allow_once");
        *source = "read_observe";
        return 1;
    }
    if (mode == MODE_OFF || mode == MODE_LOG || mode == MODE_AUDIT) {
        snprintf(decision, decision_len, "allow_once");
        *source = mode == MODE_OFF ? "system_allow" : (mode == MODE_AUDIT ? "audit" : "default");
        return 1;
    }
    if (is_system_allowed(cfg, &ev->proc)) {
        snprintf(decision, decision_len, "allow_once");
        *source = "system_allow";
        return 1;
    }
    if (mode == MODE_BLOCK) {
        snprintf(decision, decision_len, "deny_once");
        *source = "target_block";
        return 0;
    }
    int allow = 0;
    if (policy_lookup(ev, &allow) == 0) {
        snprintf(decision, decision_len, "%s_always", allow ? "allow" : "deny");
        *source = "policy";
        return allow;
    }
    if (session_lookup(ev, &allow) == 0) {
        snprintf(decision, decision_len, "%s_once", allow ? "allow" : "deny");
        *source = "session_cache";
        return allow;
    }
    if (mode == MODE_STRICT) {
        snprintf(decision, decision_len, "deny_once");
        *source = "default";
        return 0;
    }

    int prompt_rc = ask_agent(cfg, ev, decision, decision_len);
    if (prompt_rc > 0) {
        *source = "user_prompt";
        if (strcmp(decision, "allow_always") == 0) {
            policy_store(ev, 1);
            return 1;
        }
        if (strcmp(decision, "deny_always") == 0) {
            policy_store(ev, 0);
            return 0;
        }
        if (strcmp(decision, "allow_once") == 0) {
            session_store(ev, 1);
            return 1;
        }
        if (strcmp(decision, "deny_once") == 0) {
            session_store(ev, 0);
            return 0;
        }
    } else if (prompt_rc == 0) {
        snprintf(decision, decision_len, cfg->default_allow ? "allow_once" : "deny_once");
        *source = "timeout";
        return cfg->default_allow;
    }
    snprintf(decision, decision_len, "allow_once");
    *source = "agent_unavailable";
    return 1;
}

static void respond_permission(int fan_fd, int event_fd, uint32_t response)
{
    struct fanotify_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.fd = event_fd;
    resp.response = response;
    if (write(fan_fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        warn_errno("failed to write fanotify response");
    }
}

static void cleanup_denied_create_residue(const AccessEvent *ev)
{
    if (!ev->path[0] || !ev->protected_root[0]) {
        return;
    }
    if (!path_has_prefix(ev->path, ev->protected_root)) {
        return;
    }
    if (!create_candidate_matches(ev)) {
        return;
    }

    struct stat st;
    if (lstat(ev->path, &st) != 0) {
        return;
    }
    if (!S_ISREG(st.st_mode) || st.st_size != 0) {
        return;
    }
    if (ev->proc.uid_known && ev->proc.uid != 0 && st.st_uid != ev->proc.uid) {
        return;
    }
    time_t now = time(NULL);
    if (now == (time_t)-1 || st.st_ctime + JX_GUARD_CREATE_CACHE_TTL < now ||
        st.st_mtime + JX_GUARD_CREATE_CACHE_TTL < now) {
        return;
    }
    if (unlink(ev->path) != 0) {
        warn_errno("failed to remove denied create residue %s", ev->path);
        return;
    }
    fprintf(stdout, "JX_GUARD_CLEANUP path=\"%s\" reason=\"denied_create_residue\"\n", ev->path);
    fflush(stdout);
}

static int should_prompt_observed_event(const GuardConfig *cfg, const AccessEvent *ev)
{
    GuardMode mode = protected_mode_for_root(cfg, ev->protected_root);
    if (mode != MODE_PROMPT || g_agent_fd < 0) {
        return 0;
    }
    if (!action_is_write_like(ev)) {
        return 0;
    }
    if (is_guard_management_target(ev->path)) {
        return 0;
    }
    if (is_system_allowed(cfg, &ev->proc)) {
        return 0;
    }
    int allow = 0;
    return policy_lookup(ev, &allow) != 0;
}

static void handle_observed_event(const GuardConfig *cfg, const struct fanotify_event_metadata *md, unsigned long *seq)
{
    AccessEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.request_seq = ++(*seq);
    snprintf(ev.request_id, sizeof(ev.request_id), "req-%06lu", ev.request_seq);
    snprintf(ev.action, sizeof(ev.action), "%s", action_from_mask(md->mask));
    read_proc_info(md->pid, &ev.proc);
    if (md->fd < 0 || read_fd_path(md->fd, ev.path, sizeof(ev.path)) != 0 ||
        protected_root_for(cfg, ev.path, ev.protected_root, sizeof(ev.protected_root)) != 0) {
        return;
    }

    if (should_prompt_observed_event(cfg, &ev)) {
        const char *source = "notification_prompt";
        char decision[32] = "allow_once";
        int allow = decide_event(cfg, &ev, &source, decision, sizeof(decision));
        log_decision(&ev, decision, source, allow);
        return;
    }

    log_decision(&ev, "observed", "notification", 1);
}

static void process_fan_event(int fan_fd, const GuardConfig *cfg, const struct fanotify_event_metadata *md, unsigned long *seq)
{
    (void)fan_fd;
    if (md->vers != FANOTIFY_METADATA_VERSION || (md->mask & FAN_Q_OVERFLOW)) return;
    if (!(md->mask & (FAN_OPEN_PERM | FAN_ACCESS_PERM | FAN_OPEN_EXEC_PERM))) {
        if (md->mask & (FAN_OPEN | FAN_ACCESS | FAN_CLOSE_WRITE)) {
            handle_observed_event(cfg, md, seq);
        }
        if (md->fd >= 0) {
            close(md->fd);
        }
        return;
    }

    int allow = 1;
    const char *source = "system_allow";
    char decision[32] = "allow_once";
    AccessEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.request_seq = ++(*seq);
    snprintf(ev.request_id, sizeof(ev.request_id), "req-%06lu", ev.request_seq);
    snprintf(ev.action, sizeof(ev.action), "%s", action_from_mask(md->mask));
    read_proc_info(md->pid, &ev.proc);

    if (md->fd < 0 || read_fd_path(md->fd, ev.path, sizeof(ev.path)) != 0 ||
        protected_root_for(cfg, ev.path, ev.protected_root, sizeof(ev.protected_root)) != 0) {
        respond_permission(fan_fd, md->fd, FAN_ALLOW);
        if (md->fd >= 0) close(md->fd);
        return;
    }
    ev.write_intent = fd_has_write_intent(md->fd);
    if (ev.write_intent && strcmp(ev.action, "open") == 0) {
        snprintf(ev.action, sizeof(ev.action), "open_write");
    }
    if (md->mask & FAN_OPEN_PERM) {
        remember_create_candidate_if_empty_file(&ev);
    }

    if (g_stop) {
        log_decision(&ev, "allow_once", "shutdown", 1);
        respond_permission(fan_fd, md->fd, FAN_ALLOW);
        close(md->fd);
        return;
    }

    allow = decide_event(cfg, &ev, &source, decision, sizeof(decision));
    log_decision(&ev, decision, source, allow);
    if (!allow) {
        cleanup_denied_create_residue(&ev);
    }
    respond_permission(fan_fd, md->fd, allow ? FAN_ALLOW : FAN_DENY);
    close(md->fd);
}

static int setup_socket(void)
{
    mkdir("/run/jx-sentinel", 0755);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    unlink(JX_GUARD_SOCKET_PATH);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", JX_GUARD_SOCKET_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    chmod(JX_GUARD_SOCKET_PATH, 0666);
    unlink(JX_GUARD_AGENT_STATE_PATH);
    return fd;
}

static void accept_agent(int listen_fd)
{
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                warn_errno("agent accept failed");
            }
            return;
        }
        pthread_mutex_lock(&g_agent_lock);
        if (g_agent_fd >= 0) {
            pthread_mutex_unlock(&g_agent_lock);
            close(fd);
            fprintf(stdout, "JX_GUARD_AGENT duplicate=\"closed\"\n");
            fflush(stdout);
            continue;
        }
        g_agent_fd = fd;
        int state_fd = open(JX_GUARD_AGENT_STATE_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (state_fd >= 0) {
            const char connected[] = "connected\n";
            if (write(state_fd, connected, sizeof(connected) - 1) < 0) {
                warn_errno("cannot write agent state");
            }
            close(state_fd);
        } else {
            warn_errno("cannot create agent state");
        }
        pthread_mutex_unlock(&g_agent_lock);
        fprintf(stdout, "JX_GUARD_AGENT connected=\"true\"\n");
        fflush(stdout);
    }
}

static void *accept_agent_thread_main(void *arg)
{
    int listen_fd = *(int *)arg;
    while (!g_stop) {
        struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
        int ready = poll(&pfd, 1, 250);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready > 0 && (pfd.revents & POLLIN)) {
            accept_agent(listen_fd);
        }
    }
    return NULL;
}

static int start_accept_thread(int listen_fd)
{
    if (pthread_create(&g_accept_thread, NULL, accept_agent_thread_main, &g_listen_fd) != 0) {
        warn_errno("cannot start agent accept thread");
        return -1;
    }
    g_accept_thread_started = 1;
    fprintf(stdout, "JX_GUARD_AGENT_ACCEPT_THREAD started=\"true\"\n");
    fflush(stdout);
    (void)listen_fd;
    return 0;
}

static int mark_directory(int fan_fd, const char *path, uint64_t mask)
{
    if (g_stop) {
        return -1;
    }
    if (fanotify_mark(fan_fd, FAN_MARK_ADD | FAN_MARK_ONLYDIR, mask, AT_FDCWD, path) != 0) {
        warn_errno("fanotify_mark failed for %s", path);
        return -1;
    }
    return 0;
}

static int setup_fanotify(const GuardConfig *cfg)
{
    int fan_fd = fanotify_init(FAN_CLASS_CONTENT | FAN_CLOEXEC | FAN_NONBLOCK, O_RDONLY | O_LARGEFILE | O_CLOEXEC);
    if (fan_fd < 0) return -1;
    uint64_t mask = FAN_OPEN_PERM | FAN_ACCESS_PERM | FAN_CLOSE_WRITE | FAN_EVENT_ON_CHILD | FAN_ONDIR;
    int marks = 0;
    char marked[JX_GUARD_MAX_PATHS][PATH_MAX] = {{0}};
    size_t marked_count = 0;
    for (size_t i = 0; i < cfg->protected_count; i++) {
        const char *mark_path = cfg->protected_mark_paths[i][0] ?
            cfg->protected_mark_paths[i] : cfg->protected_paths[i];
        if (path_exists_in_list(marked, marked_count, mark_path)) {
            continue;
        }
        struct stat st;
        if (lstat(mark_path, &st) != 0) {
            warn_errno("cannot stat protected mark path %s", mark_path);
            continue;
        }
        if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            fprintf(stderr, "jx-sentinel-guard: protected mark path is not a directory: %s\n", mark_path);
            continue;
        }
        if (mark_directory(fan_fd, mark_path, mask) == 0) {
            marks++;
            snprintf(marked[marked_count++], PATH_MAX, "%s", mark_path);
        }
    }
    if (marks <= 0) {
        fprintf(stderr, "jx-sentinel-guard: no protected directories were marked; enforcement is inactive\n");
        close(fan_fd);
        return -1;
    }
    fprintf(stdout, "JX_GUARD_MARKS count=\"%d\"\n", marks);
    fprintf(stdout, "JX_GUARD_RECURSIVE_MARKING mode=\"disabled\" reason=\"avoid startup permission-event deadlock\"\n");
    fflush(stdout);
    return fan_fd;
}

static int event_loop(int fan_fd, int listen_fd, const GuardConfig *cfg)
{
    char buffer[JX_GUARD_READ_BUFFER] __attribute__((aligned(__alignof__(struct fanotify_event_metadata))));
    unsigned long seq = 0;
    accept_agent(listen_fd);
    while (!g_stop) {
        struct pollfd pfds[2] = {{.fd = fan_fd, .events = POLLIN}, {.fd = listen_fd, .events = POLLIN}};
        int ready = poll(pfds, 2, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        accept_agent(listen_fd);
        if (!(pfds[0].revents & POLLIN)) continue;
        for (;;) {
            ssize_t len = read(fan_fd, buffer, sizeof(buffer));
            if (len < 0 && errno == EINTR) continue;
            if (len < 0 && errno == EAGAIN) break;
            if (len <= 0) break;
            struct fanotify_event_metadata *md = (struct fanotify_event_metadata *)buffer;
            ssize_t remaining = len;
            while (FAN_EVENT_OK(md, remaining)) {
                process_fan_event(fan_fd, cfg, md, &seq);
                md = FAN_EVENT_NEXT(md, remaining);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *config_path = JX_GUARD_CONFIG_PATH;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--config FILE]\n", argv[0]);
            return 0;
        }
    }

    GuardConfig cfg;
    load_config(config_path, &cfg);
    if (install_signal_handlers() != 0) {
        warn_errno("signal setup failed");
        return 1;
    }
    int listen_fd = setup_socket();
    if (listen_fd < 0) {
        warn_errno("socket setup failed");
        return 1;
    }
    g_listen_fd = listen_fd;
    if (start_accept_thread(listen_fd) != 0) {
        close(listen_fd);
        unlink(JX_GUARD_SOCKET_PATH);
        return 1;
    }
    int fan_fd = setup_fanotify(&cfg);
    if (fan_fd < 0) {
        warn_errno("fanotify_init failed; run as root with CAP_SYS_ADMIN");
        g_stop = 1;
        if (g_accept_thread_started) {
            pthread_join(g_accept_thread, NULL);
        }
        close(listen_fd);
        return 1;
    }
    fprintf(stdout, "JX_GUARD_START mode=\"%d\" protected_paths=\"%zu\" socket=\"%s\"\n",
            cfg.mode, cfg.protected_count, JX_GUARD_SOCKET_PATH);
    fflush(stdout);
    int rc = event_loop(fan_fd, listen_fd, &cfg);
    g_stop = 1;
    if (g_accept_thread_started) {
        pthread_join(g_accept_thread, NULL);
    }
    close_agent();
    close(fan_fd);
    close(listen_fd);
    g_listen_fd = -1;
    unlink(JX_GUARD_SOCKET_PATH);
    unlink(JX_GUARD_AGENT_STATE_PATH);
    return rc == 0 ? 0 : 1;
}
