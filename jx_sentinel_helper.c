#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define JX_CONFIG_DIR "/opt/jx/etc/jx-sentinel"
#define JX_CONFIG_PATH "/opt/jx/etc/jx-sentinel/jx-sentinel.conf"
#define JX_GUARD_CONFIG_PATH "/opt/jx/etc/jx-sentinel/guard.conf"
#define JX_GUARD_DB_PATH "/opt/jx/var/lib/jx-sentinel/permissions.db"
#define JX_SERVICE_NAME "jx-sentinel.service"
#define JX_GUARD_SERVICE_NAME "jx-sentinel-guard.service"
#define JX_MAX_ITEMS 128
#define JX_MAX_CONFIG_SIZE (128 * 1024)

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "jx-sentinel-helper: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
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

static int parse_bool(const char *value)
{
    return strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
           strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0 ||
           strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0 ||
           strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0;
}

static int path_exists_as_dir(const char *path)
{
    struct stat st;
    return path && path[0] == '/' && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int value_seen(char values[][PATH_MAX], size_t count, const char *value)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(values[i], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static int validate_config(char *text)
{
    char paths[JX_MAX_ITEMS][PATH_MAX] = {{0}};
    char exts[JX_MAX_ITEMS][PATH_MAX] = {{0}};
    size_t path_count = 0;
    size_t ext_count = 0;
    int in_section = 0;
    int have_path = 0;
    unsigned long line_no = 0;

    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        line_no++;
        char *s = trim_space(line);
        if (*s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        if (*s == '[') {
            in_section = strcmp(s, "[JX Sentinel]") == 0;
            continue;
        }
        if (!in_section) {
            die("line %lu outside [JX Sentinel] section", line_no);
            return -1;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            die("line %lu is missing '='", line_no);
            return -1;
        }
        *eq = '\0';
        char *key = trim_space(s);
        char *value = trim_space(eq + 1);

        if (strcmp(key, "NotifyUser") == 0) {
            if (*value == '\0' || strchr(value, '/') || strchr(value, '\n')) {
                die("line %lu has invalid NotifyUser", line_no);
                return -1;
            }
        } else if (strcmp(key, "Notifications") == 0 || strcmp(key, "Verbose") == 0) {
            if (!parse_bool(value)) {
                die("line %lu has invalid boolean", line_no);
                return -1;
            }
        } else if (strcmp(key, "WatchPath") == 0) {
            if (!path_exists_as_dir(value)) {
                die("line %lu has invalid WatchPath", line_no);
                return -1;
            }
            if (!value_seen(paths, path_count, value)) {
                if (path_count >= JX_MAX_ITEMS) {
                    die("too many WatchPath entries");
                    return -1;
                }
                snprintf(paths[path_count++], PATH_MAX, "%s", value);
            }
            have_path = 1;
        } else if (strcmp(key, "Extension") == 0) {
            if (value[0] != '.' || value[1] == '\0' || strchr(value, '/') || strchr(value, ' ')) {
                die("line %lu has invalid Extension", line_no);
                return -1;
            }
            if (!value_seen(exts, ext_count, value)) {
                if (ext_count >= JX_MAX_ITEMS) {
                    die("too many Extension entries");
                    return -1;
                }
                snprintf(exts[ext_count++], PATH_MAX, "%s", value);
            }
        } else {
            die("line %lu has unknown key: %s", line_no, key);
            return -1;
        }
    }

    if (!have_path) {
        die("at least one WatchPath is required");
        return -1;
    }
    return 0;
}

static char *read_stdin_config(void)
{
    size_t cap = 8192;
    size_t len = 0;
    char *buf = calloc(1, cap);
    if (!buf) {
        return NULL;
    }

    for (;;) {
        if (len == cap) {
            if (cap >= JX_MAX_CONFIG_SIZE) {
                free(buf);
                errno = EFBIG;
                return NULL;
            }
            cap *= 2;
            char *grown = realloc(buf, cap + 1);
            if (!grown) {
                free(buf);
                return NULL;
            }
            buf = grown;
        }
        ssize_t n = read(STDIN_FILENO, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            return NULL;
        }
        if (n == 0) {
            buf[len] = '\0';
            return buf;
        }
        len += (size_t)n;
    }
}

static int save_config(void)
{
    char *config = read_stdin_config();
    if (!config) {
        die("failed to read config from stdin: %s", strerror(errno));
        return 1;
    }

    char *copy = strdup(config);
    if (!copy) {
        free(config);
        die("allocation failure");
        return 1;
    }
    if (validate_config(copy) != 0) {
        free(copy);
        free(config);
        return 1;
    }
    free(copy);

    if (mkdir("/opt/jx", 0755) != 0 && errno != EEXIST) {
        die("cannot create /opt/jx: %s", strerror(errno));
        free(config);
        return 1;
    }
    if (mkdir("/opt/jx/etc", 0755) != 0 && errno != EEXIST) {
        die("cannot create /opt/jx/etc: %s", strerror(errno));
        free(config);
        return 1;
    }
    if (mkdir(JX_CONFIG_DIR, 0755) != 0 && errno != EEXIST) {
        die("cannot create %s: %s", JX_CONFIG_DIR, strerror(errno));
        free(config);
        return 1;
    }

    char tmp_path[] = JX_CONFIG_DIR "/.jx-sentinel.conf.tmp.XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        die("cannot create temp config: %s", strerror(errno));
        free(config);
        return 1;
    }

    size_t len = strlen(config);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, config + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("cannot write temp config: %s", strerror(errno));
            close(fd);
            unlink(tmp_path);
            free(config);
            return 1;
        }
        written += (size_t)n;
    }

    if (fchmod(fd, 0644) != 0 || fsync(fd) != 0 || close(fd) != 0) {
        die("cannot finalize temp config: %s", strerror(errno));
        unlink(tmp_path);
        free(config);
        return 1;
    }

    if (rename(tmp_path, JX_CONFIG_PATH) != 0) {
        die("cannot install config: %s", strerror(errno));
        unlink(tmp_path);
        free(config);
        return 1;
    }

    int dir_fd = open(JX_CONFIG_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    free(config);
    return 0;
}

static int run_command(char *const argv[])
{
    pid_t child = fork();
    if (child < 0) {
        die("fork failed: %s", strerror(errno));
        return 1;
    }

    if (child == 0) {
        char *const envp[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL};
        execve(argv[0], argv, envp);
        _exit(127);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) {
        die("waitpid failed: %s", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        die("command killed by signal %d", WTERMSIG(status));
    }
    return 1;
}

static int systemctl_action_for(const char *action, const char *service)
{
    char *const argv[] = {
        "/usr/bin/systemctl",
        (char *)action,
        (char *)service,
        NULL
    };
    return run_command(argv);
}

static int systemctl_action(const char *action)
{
    return systemctl_action_for(action, JX_SERVICE_NAME);
}

static int guard_systemctl_action(const char *action)
{
    return systemctl_action_for(action, JX_GUARD_SERVICE_NAME);
}

static int valid_process_allowlist_value(const char *value)
{
    if (!value || *value == '\0' || strlen(value) >= PATH_MAX) {
        return 0;
    }
    if (value[0] == '/' && value[1] == '\0') {
        return 0;
    }
    if (value[0] == '/' && access(value, F_OK) != 0) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p <= ' ' || *p == '=' || *p == ';' || *p == '#') {
            return 0;
        }
    }
    return 1;
}

static int valid_guard_protected_path_value(const char *value)
{
    struct stat st;
    return value && value[0] == '/' && stat(value, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *basename_for_display(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    const char *backslash = path ? strrchr(path, '\\') : NULL;
    const char *sep = slash && backslash ? (slash > backslash ? slash : backslash) : (slash ? slash : backslash);
    return sep ? sep + 1 : path;
}

static int policy_subject_matches_app(const char *subject, const char *app)
{
    if (!subject || !app || !*app || strcmp(subject, "unknown") == 0 ||
        strncmp(subject, "pid:", 4) == 0) {
        return 0;
    }
    const char *name = strncmp(subject, "cmd:", 4) == 0 ?
        basename_for_display(subject + 4) : basename_for_display(subject);
    return name && strcmp(name, app) == 0;
}

static char *read_stdin_value(void)
{
    char *value = read_stdin_config();
    if (!value) {
        return NULL;
    }
    char *trimmed = trim_space(value);
    if (trimmed != value) {
        memmove(value, trimmed, strlen(trimmed) + 1);
    }
    char *newline = strpbrk(value, "\r\n");
    if (newline) {
        *newline = '\0';
    }
    return value;
}

static int update_guard_process_allowlist(int add)
{
    char *value = read_stdin_value();
    if (!value) {
        die("failed to read allowlist value from stdin: %s", strerror(errno));
        return 1;
    }
    if (!valid_process_allowlist_value(value)) {
        die("invalid ProcessAllowlist value");
        free(value);
        return 1;
    }

    FILE *in = fopen(JX_GUARD_CONFIG_PATH, "re");
    if (!in) {
        die("cannot open %s: %s", JX_GUARD_CONFIG_PATH, strerror(errno));
        free(value);
        return 1;
    }

    char tmp_path[] = JX_CONFIG_DIR "/.guard.conf.tmp.XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        fclose(in);
        die("cannot create temp guard config: %s", strerror(errno));
        free(value);
        return 1;
    }
    if (fchmod(fd, 0644) != 0) {
        close(fd);
        unlink(tmp_path);
        fclose(in);
        die("cannot set temp guard config permissions: %s", strerror(errno));
        free(value);
        return 1;
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(tmp_path);
        fclose(in);
        die("cannot open temp guard config stream: %s", strerror(errno));
        free(value);
        return 1;
    }

    char line[4096];
    int in_section = 0;
    int saw_section = 0;
    int saw_value = 0;
    int wrote_value = 0;
    while (fgets(line, sizeof(line), in)) {
        char copy[4096];
        snprintf(copy, sizeof(copy), "%s", line);
        char *s = trim_space(copy);
        if (*s == '[') {
            if (in_section && add && !saw_value && !wrote_value) {
                fprintf(out, "ProcessAllowlist=%s\n", value);
                wrote_value = 1;
            }
            in_section = strcmp(s, "[JX Sentinel Guard]") == 0;
            if (in_section) {
                saw_section = 1;
            }
        }

        if (in_section && strncmp(s, "ProcessAllowlist=", 17) == 0) {
            char *existing = trim_space(s + 17);
            if (strcmp(existing, value) == 0) {
                saw_value = 1;
                if (!add) {
                    continue;
                }
            }
        }
        fputs(line, out);
    }

    if (saw_section && in_section && add && !saw_value && !wrote_value) {
        fprintf(out, "ProcessAllowlist=%s\n", value);
        wrote_value = 1;
    } else if (!saw_section && add) {
        fprintf(out, "\n[JX Sentinel Guard]\nProcessAllowlist=%s\n", value);
        wrote_value = 1;
    }

    fclose(in);
    if (fflush(out) != 0 || fsync(fd) != 0 || fclose(out) != 0) {
        unlink(tmp_path);
        die("cannot finalize guard config: %s", strerror(errno));
        free(value);
        return 1;
    }
    if (rename(tmp_path, JX_GUARD_CONFIG_PATH) != 0) {
        unlink(tmp_path);
        die("cannot install guard config: %s", strerror(errno));
        free(value);
        return 1;
    }

    free(value);
    return guard_systemctl_action("restart");
}

static int remove_guard_policy_app(void)
{
    char *value = read_stdin_value();
    if (!value) {
        die("failed to read policy app value from stdin: %s", strerror(errno));
        return 1;
    }
    if (!valid_process_allowlist_value(value)) {
        die("invalid policy app value");
        free(value);
        return 1;
    }

    FILE *in = fopen(JX_GUARD_DB_PATH, "re");
    if (!in) {
        if (errno == ENOENT) {
            free(value);
            return 0;
        }
        die("cannot open %s: %s", JX_GUARD_DB_PATH, strerror(errno));
        free(value);
        return 1;
    }

    char tmp_path[] = "/opt/jx/var/lib/jx-sentinel/.permissions.db.tmp.XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        fclose(in);
        die("cannot create temp policy db: %s", strerror(errno));
        free(value);
        return 1;
    }
    if (fchmod(fd, 0644) != 0) {
        close(fd);
        unlink(tmp_path);
        fclose(in);
        die("cannot set temp policy db permissions: %s", strerror(errno));
        free(value);
        return 1;
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(tmp_path);
        fclose(in);
        die("cannot open temp policy db stream: %s", strerror(errno));
        free(value);
        return 1;
    }

    char line[4096];
    while (fgets(line, sizeof(line), in)) {
        char subject[PATH_MAX];
        char protected_root[PATH_MAX];
        char decision[16];
        char scope[32];
        unsigned long uid = 0;
        if (sscanf(line, "%4095[^\t]\t%lu\t%4095[^\t]\t%15s\t%31s",
                   subject, &uid, protected_root, decision, scope) == 5 &&
            strcmp(decision, "allow") == 0 &&
            policy_subject_matches_app(subject, value)) {
            continue;
        }
        fputs(line, out);
    }

    fclose(in);
    if (fflush(out) != 0 || fsync(fd) != 0 || fclose(out) != 0) {
        unlink(tmp_path);
        die("cannot finalize policy db: %s", strerror(errno));
        free(value);
        return 1;
    }
    if (rename(tmp_path, JX_GUARD_DB_PATH) != 0) {
        unlink(tmp_path);
        die("cannot install policy db: %s", strerror(errno));
        free(value);
        return 1;
    }

    free(value);
    return 0;
}

static int update_guard_protected_path(int add)
{
    char *value = read_stdin_value();
    if (!value) {
        die("failed to read protected path from stdin: %s", strerror(errno));
        return 1;
    }
    if (!valid_guard_protected_path_value(value)) {
        die("invalid ProtectedPath value");
        free(value);
        return 1;
    }

    FILE *in = fopen(JX_GUARD_CONFIG_PATH, "re");
    if (!in) {
        die("cannot open %s: %s", JX_GUARD_CONFIG_PATH, strerror(errno));
        free(value);
        return 1;
    }

    char tmp_path[] = JX_CONFIG_DIR "/.guard.conf.tmp.XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        fclose(in);
        die("cannot create temp guard config: %s", strerror(errno));
        free(value);
        return 1;
    }
    if (fchmod(fd, 0644) != 0) {
        close(fd);
        unlink(tmp_path);
        fclose(in);
        die("cannot set temp guard config permissions: %s", strerror(errno));
        free(value);
        return 1;
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(tmp_path);
        fclose(in);
        die("cannot open temp guard config stream: %s", strerror(errno));
        free(value);
        return 1;
    }

    char line[4096];
    int in_section = 0;
    int saw_section = 0;
    int saw_value = 0;
    int wrote_value = 0;
    while (fgets(line, sizeof(line), in)) {
        char copy[4096];
        snprintf(copy, sizeof(copy), "%s", line);
        char *s = trim_space(copy);
        if (*s == '[') {
            if (in_section && add && !saw_value && !wrote_value) {
                fprintf(out, "ProtectedPath=%s\n", value);
                wrote_value = 1;
            }
            in_section = strcmp(s, "[JX Sentinel Guard]") == 0;
            if (in_section) {
                saw_section = 1;
            }
        }

        if (in_section && strncmp(s, "ProtectedPath=", 14) == 0) {
            char *existing = trim_space(s + 14);
            if (strcmp(existing, value) == 0) {
                saw_value = 1;
                if (!add) {
                    continue;
                }
            }
        }
        fputs(line, out);
    }

    if (saw_section && in_section && add && !saw_value && !wrote_value) {
        fprintf(out, "ProtectedPath=%s\n", value);
    } else if (!saw_section && add) {
        fprintf(out, "\n[JX Sentinel Guard]\nProtectedPath=%s\n", value);
    }

    fclose(in);
    if (fflush(out) != 0 || fsync(fd) != 0 || fclose(out) != 0) {
        unlink(tmp_path);
        die("cannot finalize guard config: %s", strerror(errno));
        free(value);
        return 1;
    }
    if (rename(tmp_path, JX_GUARD_CONFIG_PATH) != 0) {
        unlink(tmp_path);
        die("cannot install guard config: %s", strerror(errno));
        free(value);
        return 1;
    }

    free(value);
    return guard_systemctl_action("restart");
}

static int set_guard_mode(const char *mode)
{
    if (strcmp(mode, "prompt") != 0 && strcmp(mode, "log") != 0 &&
        strcmp(mode, "off") != 0 && strcmp(mode, "strict") != 0 &&
        strcmp(mode, "audit") != 0 && strcmp(mode, "block") != 0) {
        die("invalid guard mode");
        return 1;
    }

    FILE *in = fopen(JX_GUARD_CONFIG_PATH, "re");
    if (!in) {
        die("cannot open %s: %s", JX_GUARD_CONFIG_PATH, strerror(errno));
        return 1;
    }

    char tmp_path[] = JX_CONFIG_DIR "/.guard.conf.tmp.XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        fclose(in);
        die("cannot create temp guard config: %s", strerror(errno));
        return 1;
    }
    if (fchmod(fd, 0644) != 0) {
        close(fd);
        unlink(tmp_path);
        fclose(in);
        die("cannot set temp guard config permissions: %s", strerror(errno));
        return 1;
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(tmp_path);
        fclose(in);
        die("cannot open temp guard config stream: %s", strerror(errno));
        return 1;
    }

    char line[4096];
    int wrote_mode = 0;
    int in_section = 0;
    while (fgets(line, sizeof(line), in)) {
        char copy[4096];
        snprintf(copy, sizeof(copy), "%s", line);
        char *s = trim_space(copy);
        if (*s == '[') {
            if (in_section && !wrote_mode) {
                fprintf(out, "Mode=%s\n", mode);
                wrote_mode = 1;
            }
            in_section = strcmp(s, "[JX Sentinel Guard]") == 0;
        }
        if (in_section && strncmp(s, "Mode=", 5) == 0) {
            fprintf(out, "Mode=%s\n", mode);
            wrote_mode = 1;
        } else {
            fputs(line, out);
        }
    }
    if (in_section && !wrote_mode) {
        fprintf(out, "Mode=%s\n", mode);
    }
    fclose(in);
    if (fflush(out) != 0 || fsync(fd) != 0 || fclose(out) != 0) {
        unlink(tmp_path);
        die("cannot finalize guard config: %s", strerror(errno));
        return 1;
    }
    if (rename(tmp_path, JX_GUARD_CONFIG_PATH) != 0) {
        unlink(tmp_path);
        die("cannot install guard config: %s", strerror(errno));
        return 1;
    }
    return guard_systemctl_action("restart");
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s --save-config | --apply-config | --start | --stop | --restart | --enable | --disable | --guard-start | --guard-stop | --guard-restart | --guard-enable | --guard-disable | --guard-mode-prompt | --guard-mode-log | --guard-mode-audit | --guard-mode-block | --guard-add-app | --guard-remove-app | --guard-remove-policy-app | --guard-add-folder | --guard-remove-folder\n",
            program);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "--save-config") == 0) {
        return save_config();
    }
    if (strcmp(argv[1], "--start") == 0) {
        return systemctl_action("start");
    }
    if (strcmp(argv[1], "--stop") == 0) {
        return systemctl_action("stop");
    }
    if (strcmp(argv[1], "--restart") == 0) {
        return systemctl_action("restart");
    }
    if (strcmp(argv[1], "--apply-config") == 0) {
        return systemctl_action("restart");
    }
    if (strcmp(argv[1], "--enable") == 0) {
        return systemctl_action("enable");
    }
    if (strcmp(argv[1], "--disable") == 0) {
        return systemctl_action("disable");
    }
    if (strcmp(argv[1], "--guard-start") == 0) {
        return guard_systemctl_action("start");
    }
    if (strcmp(argv[1], "--guard-stop") == 0) {
        return guard_systemctl_action("stop");
    }
    if (strcmp(argv[1], "--guard-restart") == 0) {
        return guard_systemctl_action("restart");
    }
    if (strcmp(argv[1], "--guard-enable") == 0) {
        return guard_systemctl_action("enable");
    }
    if (strcmp(argv[1], "--guard-disable") == 0) {
        return guard_systemctl_action("disable");
    }
    if (strcmp(argv[1], "--guard-mode-prompt") == 0) {
        return set_guard_mode("prompt");
    }
    if (strcmp(argv[1], "--guard-mode-log") == 0) {
        return set_guard_mode("log");
    }
    if (strcmp(argv[1], "--guard-mode-audit") == 0) {
        return set_guard_mode("audit");
    }
    if (strcmp(argv[1], "--guard-mode-block") == 0) {
        return set_guard_mode("block");
    }
    if (strcmp(argv[1], "--guard-add-app") == 0) {
        return update_guard_process_allowlist(1);
    }
    if (strcmp(argv[1], "--guard-remove-app") == 0) {
        return update_guard_process_allowlist(0);
    }
    if (strcmp(argv[1], "--guard-remove-policy-app") == 0) {
        return remove_guard_policy_app();
    }
    if (strcmp(argv[1], "--guard-add-folder") == 0) {
        return update_guard_protected_path(1);
    }
    if (strcmp(argv[1], "--guard-remove-folder") == 0) {
        return update_guard_protected_path(0);
    }

    usage(argv[0]);
    return 2;
}
