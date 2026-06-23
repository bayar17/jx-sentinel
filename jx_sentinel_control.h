#ifndef JX_SENTINEL_CONTROL_H
#define JX_SENTINEL_CONTROL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <gtk/gtk.h>
#include <limits.h>
#include <stddef.h>

#include "jx_module.h"

#define JX_CONTROL_APP_ID "org.jx.sentinel.control"
#define JX_HELPER_PATH "/opt/jx/libexec/jx-sentinel-helper"
#define JX_GUI_PATH "/opt/jx/bin/jx-sentinel-control"
#define JX_DAEMON_PATH "/opt/jx/sbin/jx-sentinel"
#define JX_GUARD_DAEMON_PATH "/opt/jx/sbin/jx-sentinel-guard"
#define JX_CONFIG_PATH "/opt/jx/etc/jx-sentinel/jx-sentinel.conf"
#define JX_GUARD_CONFIG_PATH "/opt/jx/etc/jx-sentinel/guard.conf"
#define JX_GUARD_DB_PATH "/opt/jx/var/lib/jx-sentinel/permissions.db"
#define JX_GUARD_SOCKET_PATH "/run/jx-sentinel/guard.sock"
#define JX_GUARD_AGENT_STATE_PATH "/run/jx-sentinel/agent.connected"
#define JX_PERMISSION_AGENT_PATH "/opt/jx/bin/jx-permission-agent"
#define JX_SERVICE_NAME "jx-sentinel.service"
#define JX_GUARD_SERVICE_NAME "jx-sentinel-guard.service"
#define JX_CONTROL_SHARE_DIR "/opt/jx/share/jx-sentinel-control"
#define JX_MAX_CONTROL_ITEMS 128

typedef struct JxWatchRoot {
    char path[PATH_MAX];
    int active;
    unsigned long event_count;
    char last_event[64];
} JxWatchRoot;

typedef struct JxExtensionRule {
    char extension[64];
    int enabled;
    int notify;
    char severity[16];
    unsigned long matched_events;
} JxExtensionRule;

typedef struct JxRule {
    char name[96];
    char path_scope[PATH_MAX];
    char extensions[256];
    int notify;
    char severity[16];
    int enabled;
} JxRule;

typedef struct JxEvent {
    char time[64];
    char event[32];
    char type[32];
    char path[PATH_MAX];
    char process[128];
    int pid;
    int uid;
    char rule[96];
    char severity[16];
    int notified;
} JxEvent;

typedef struct JxProcess {
    char name[128];
    int pid;
    int uid;
    char exe[PATH_MAX];
    char cmdline[512];
    char last_created_file[PATH_MAX];
} JxProcess;

typedef struct JxServiceStatus {
    char state[32];
    char enabled[32];
    int pid;
    size_t watched_root_count;
    size_t active_rule_count;
    char last_event_time[64];
} JxServiceStatus;

typedef struct JxDiagnosticCheck {
    char name[128];
    char state[16];
    char detail[256];
} JxDiagnosticCheck;

typedef struct JxProfile {
    char name[64];
    char description[256];
} JxProfile;

struct jx_control_config {
    int notifications;
    int verbose;
    char notify_user[128];
    char watch_paths[JX_MAX_CONTROL_ITEMS][PATH_MAX];
    size_t watch_count;
    char extensions[JX_MAX_CONTROL_ITEMS][64];
    size_t extension_count;
};

struct jx_control_app {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *stack;
    GtkWidget *header_status_label;
    GtkWidget *status_label;
    GtkWidget *enabled_label;
    GtkWidget *pid_label;
    GtkWidget *roots_label;
    GtkWidget *rules_label;
    GtkWidget *events_today_label;
    GtkWidget *dashboard_guard_rules_label;
    GtkWidget *last_event_label;
    GtkWidget *watch_list;
    GtkWidget *extension_list;
    GtkWidget *log_all_switch;
    GtkWidget *notify_switch;
    GtkWidget *verbose_switch;
    GtkWidget *notify_user_entry;
    GtkWidget *logs_view;
    GtkWidget *guard_status_label;
    GtkWidget *guard_agent_label;
    GtkWidget *guard_paths_label;
    GtkWidget *guard_rules_label;
    GtkWidget *guard_last_label;
    GtkWidget *guard_logs_view;
    GtkWidget *guard_paths_list;
    GtkWidget *guard_process_allowlist;
    GtkStringList *events_model;
    GtkWidget *event_view;
    GtkWidget *diagnostics_list;
    GtkWidget *inspector;
    GtkWidget *inspector_title;
    GtkWidget *inspector_details;
    GtkWidget *timeline;
    GtkWidget *helper_label;
    struct jx_control_config config;
    JxServiceStatus service;
    JxModule *modules;
    size_t module_count;
};

int jx_control_load_config(const char *path, struct jx_control_config *config);
char *jx_control_config_to_text(const struct jx_control_config *config);
int jx_control_validate_path(const char *path, char *error, size_t error_len);
int jx_control_validate_extension(const char *ext, char *error, size_t error_len);

#endif
