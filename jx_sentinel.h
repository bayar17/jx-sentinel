#ifndef JX_SENTINEL_H
#define JX_SENTINEL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define JX_LOG_PREFIX "JX_SENTINEL_EVENT"
#define JX_DEFAULT_CONFIG_PATH "/opt/jx/etc/jx-sentinel/jx-sentinel.conf"
#define JX_MAX_EXTENSIONS 128
#define JX_MAX_WATCHES 128
#define JX_READ_BUFFER_SIZE 65536

struct jx_options {
    int verbose;
    int notify_enabled;
    char *notify_user;
    char *config_path;
    char *extensions[JX_MAX_EXTENSIONS];
    size_t extension_count;
    char *watch_paths[JX_MAX_WATCHES];
    size_t watch_count;
};

struct jx_process_info {
    pid_t pid;
    uid_t uid;
    int uid_known;
    char exe[PATH_MAX];
    char cmdline[4096];
};

struct jx_watch {
    char path[PATH_MAX];
    int mount_id;
    unsigned int handle_type;
    unsigned int handle_bytes;
    unsigned char handle[128];
};

int jx_send_notification(const char *user, const char *summary, const char *body);

void jx_print_usage(const char *program);
int jx_parse_args(int argc, char **argv, struct jx_options *opts);
void jx_free_options(struct jx_options *opts);
int jx_validate_options(const struct jx_options *opts);
int jx_read_process_info(pid_t pid, struct jx_process_info *info);
const char *jx_event_name(uint64_t mask);
int jx_path_has_allowed_extension(const struct jx_options *opts, const char *path, int is_dir);
void jx_log_event(const char *event, const struct jx_process_info *proc,
                  const char *path, int is_dir);

#endif
