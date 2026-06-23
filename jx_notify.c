#include "jx_sentinel.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int make_env(char **slot, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        return -1;
    }

    char *value = calloc(1, (size_t)needed + 1);
    if (!value) {
        return -1;
    }

    va_start(ap, fmt);
    int written = vsnprintf(value, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    if (written != needed) {
        free(value);
        return -1;
    }

    *slot = value;
    return 0;
}

static void free_env(char **env)
{
    for (size_t i = 0; env[i]; i++) {
        free(env[i]);
    }
}

static int build_env(char **env, size_t env_count, const struct passwd *pw)
{
    if (env_count < 12) {
        return -1;
    }

    if (make_env(&env[0], "DISPLAY=:0") != 0 ||
        make_env(&env[1], "WAYLAND_DISPLAY=wayland-0") != 0 ||
        make_env(&env[2], "XDG_RUNTIME_DIR=/run/user/%lu", (unsigned long)pw->pw_uid) != 0 ||
        make_env(&env[3], "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%lu/bus",
                 (unsigned long)pw->pw_uid) != 0 ||
        make_env(&env[4], "HOME=%s", pw->pw_dir) != 0 ||
        make_env(&env[5], "USER=%s", pw->pw_name) != 0 ||
        make_env(&env[6], "LOGNAME=%s", pw->pw_name) != 0 ||
        make_env(&env[7], "USERNAME=%s", pw->pw_name) != 0 ||
        make_env(&env[8], "XDG_CURRENT_DESKTOP=ubuntu:GNOME") != 0 ||
        make_env(&env[9], "DESKTOP_SESSION=ubuntu") != 0 ||
        make_env(&env[10], "PATH=/usr/bin:/bin") != 0) {
        free_env(env);
        return -1;
    }
    env[11] = NULL;

    return 0;
}

int jx_send_notification(const char *user, const char *summary, const char *body)
{
    if (!user || !*user || !summary || !body) {
        return -1;
    }

    struct passwd *pw = getpwnam(user);
    if (!pw) {
        fprintf(stderr, "jx-sentinel: notify user not found: %s\n", user);
        return -1;
    }

    char *env[12] = {0};
    if (build_env(env, sizeof(env) / sizeof(env[0]), pw) != 0) {
        fprintf(stderr, "jx-sentinel: failed to build notify environment\n");
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "jx-sentinel: fork for notify-send failed: %s\n", strerror(errno));
        free_env(env);
        return -1;
    }

    if (child == 0) {
        if (initgroups(pw->pw_name, pw->pw_gid) != 0) {
            fprintf(stderr, "jx-sentinel: initgroups for notify user %s failed: %s\n",
                    pw->pw_name, strerror(errno));
            _exit(126);
        }
        if (setgid(pw->pw_gid) != 0) {
            fprintf(stderr, "jx-sentinel: setgid for notify user %s failed: %s\n",
                    pw->pw_name, strerror(errno));
            _exit(126);
        }
        if (setuid(pw->pw_uid) != 0) {
            fprintf(stderr, "jx-sentinel: setuid for notify user %s failed: %s\n",
                    pw->pw_name, strerror(errno));
            _exit(127);
        }

        char *const argv[] = {
            "/usr/bin/notify-send",
            "--app-name=jx-sentinel",
            (char *)summary,
            (char *)body,
            NULL
        };
        execve("/usr/bin/notify-send", argv, env);
        fprintf(stderr, "jx-sentinel: execve /usr/bin/notify-send failed: %s\n", strerror(errno));
        _exit(127);
    }

    free_env(env);

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) {
        fprintf(stderr, "jx-sentinel: waitpid for notify-send failed: %s\n", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        fprintf(stderr, "jx-sentinel: notify-send exited with status %d\n", WEXITSTATUS(status));
        return -1;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "jx-sentinel: notify-send killed by signal %d\n", WTERMSIG(status));
        return -1;
    }

    return 0;
}
