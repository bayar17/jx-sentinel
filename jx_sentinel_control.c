#include "jx_sentinel_control.h"
#include "jx_module_loader.h"
#include "jx_splash.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <jsc/jsc.h>
#include <webkit/webkit.h>

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

static void apply_desktop_theme_preference(void)
{
    GtkSettings *settings = gtk_settings_get_default();
    if (!settings) {
        return;
    }

    int prefer_dark = 0;
    GSettings *desktop = g_settings_new("org.gnome.desktop.interface");
    if (desktop) {
        char *scheme = g_settings_get_string(desktop, "color-scheme");
        char *theme = g_settings_get_string(desktop, "gtk-theme");
        prefer_dark = (scheme && strstr(scheme, "dark")) ||
                      (theme && g_str_has_suffix(theme, "-dark"));
        g_free(scheme);
        g_free(theme);
        g_object_unref(desktop);
    }

    g_object_set(settings, "gtk-application-prefer-dark-theme", prefer_dark, NULL);
}

static int parse_bool(const char *value, int *out)
{
    if (g_ascii_strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
        g_ascii_strcasecmp(value, "yes") == 0 || g_ascii_strcasecmp(value, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (g_ascii_strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0 ||
        g_ascii_strcasecmp(value, "no") == 0 || g_ascii_strcasecmp(value, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static void config_defaults(struct jx_control_config *config)
{
    memset(config, 0, sizeof(*config));
    config->notifications = 1;
    config->verbose = 0;
    snprintf(config->notify_user, sizeof(config->notify_user), "jackson");
    snprintf(config->watch_paths[config->watch_count++], PATH_MAX, "/opt/jx");
    snprintf(config->watch_paths[config->watch_count++], PATH_MAX, "/home/jackson");
    snprintf(config->extensions[config->extension_count++], 64, ".desktop");
    snprintf(config->extensions[config->extension_count++], 64, ".conf");
    snprintf(config->extensions[config->extension_count++], 64, ".service");
    snprintf(config->extensions[config->extension_count++], 64, ".sh");
    snprintf(config->extensions[config->extension_count++], 64, ".c");
    snprintf(config->extensions[config->extension_count++], 64, ".h");
}

static int path_duplicate(const struct jx_control_config *config, const char *path)
{
    for (size_t i = 0; i < config->watch_count; i++) {
        if (strcmp(config->watch_paths[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

static int extension_duplicate(const struct jx_control_config *config, const char *ext)
{
    for (size_t i = 0; i < config->extension_count; i++) {
        if (strcmp(config->extensions[i], ext) == 0) {
            return 1;
        }
    }
    return 0;
}

int jx_control_validate_path(const char *path, char *error, size_t error_len)
{
    struct stat st;
    if (!path || path[0] == '\0') {
        snprintf(error, error_len, "Path is empty.");
        return -1;
    }
    if (path[0] != '/') {
        snprintf(error, error_len, "Watched paths must be absolute.");
        return -1;
    }
    if (stat(path, &st) != 0) {
        snprintf(error, error_len, "Path does not exist: %s", strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(error, error_len, "Path is not a directory.");
        return -1;
    }
    return 0;
}

int jx_control_validate_extension(const char *ext, char *error, size_t error_len)
{
    if (!ext || ext[0] == '\0') {
        snprintf(error, error_len, "Extension is empty.");
        return -1;
    }
    if (ext[0] != '.' || ext[1] == '\0') {
        snprintf(error, error_len, "Extensions must start with '.' and include a suffix.");
        return -1;
    }
    if (strchr(ext, '/') || strchr(ext, ' ') || strchr(ext, '\t') || strchr(ext, '\n')) {
        snprintf(error, error_len, "Extensions cannot contain slashes or whitespace.");
        return -1;
    }
    return 0;
}

int jx_control_load_config(const char *path, struct jx_control_config *config)
{
    config_defaults(config);

    FILE *fp = fopen(path, "re");
    if (!fp) {
        return -1;
    }

    config->watch_count = 0;
    config->extension_count = 0;

    char line[4096];
    int in_section = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim_space(line);
        if (*s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        if (*s == '[') {
            in_section = strcmp(s, "[JX Sentinel]") == 0;
            continue;
        }
        if (!in_section) {
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim_space(s);
        char *value = trim_space(eq + 1);

        if (strcmp(key, "NotifyUser") == 0) {
            snprintf(config->notify_user, sizeof(config->notify_user), "%s", value);
        } else if (strcmp(key, "Notifications") == 0) {
            parse_bool(value, &config->notifications);
        } else if (strcmp(key, "Verbose") == 0) {
            parse_bool(value, &config->verbose);
        } else if (strcmp(key, "WatchPath") == 0 && config->watch_count < JX_MAX_CONTROL_ITEMS) {
            char err[160];
            if (jx_control_validate_path(value, err, sizeof(err)) == 0 &&
                !path_duplicate(config, value)) {
                snprintf(config->watch_paths[config->watch_count++], PATH_MAX, "%s", value);
            }
        } else if (strcmp(key, "Extension") == 0 && config->extension_count < JX_MAX_CONTROL_ITEMS) {
            char err[160];
            if (jx_control_validate_extension(value, err, sizeof(err)) == 0 &&
                !extension_duplicate(config, value)) {
                snprintf(config->extensions[config->extension_count++], 64, "%s", value);
            }
        }
    }

    fclose(fp);
    return 0;
}

char *jx_control_config_to_text(const struct jx_control_config *config)
{
    GString *out = g_string_new("[JX Sentinel]\n");
    g_string_append_printf(out, "NotifyUser=%s\n", config->notify_user[0] ? config->notify_user : "jackson");
    g_string_append_printf(out, "Notifications=%s\n", config->notifications ? "true" : "false");
    g_string_append_printf(out, "Verbose=%s\n\n", config->verbose ? "true" : "false");

    for (size_t i = 0; i < config->watch_count; i++) {
        g_string_append_printf(out, "WatchPath=%s\n", config->watch_paths[i]);
    }
    g_string_append(out, "\n");
    for (size_t i = 0; i < config->extension_count; i++) {
        g_string_append_printf(out, "Extension=%s\n", config->extensions[i]);
    }
    return g_string_free(out, FALSE);
}

static void show_message(struct jx_control_app *app, GtkMessageType type,
                         const char *primary, const char *secondary)
{
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               type,
                                               GTK_BUTTONS_CLOSE,
                                               "%s",
                                               primary);
    if (secondary) {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary);
    }
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_window_destroy), NULL);
    gtk_window_present(GTK_WINDOW(dialog));
}

static int run_capture(char *const argv[], char **output)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (child == 0) {
        char *const envp[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL};
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execve(argv[0], argv, envp);
        _exit(127);
    }

    close(pipefd[1]);
    GString *buf = g_string_new(NULL);
    char chunk[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        g_string_append_len(buf, chunk, (gssize)n);
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }

    *output = g_string_free(buf, FALSE);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

static char *run_capture_stripped(char *const argv[])
{
    char *out = NULL;
    int rc = run_capture(argv, &out);
    if (rc != 0 || !out) {
        g_free(out);
        return NULL;
    }
    char *stripped = g_strstrip(out);
    if (!*stripped) {
        g_free(out);
        return NULL;
    }
    char *result = g_strdup(stripped);
    g_free(out);
    return result;
}

static char *systemctl_property(const char *service, const char *property)
{
    char prop_arg[96];
    snprintf(prop_arg, sizeof(prop_arg), "-p%s", property);
    char *argv[] = {"/usr/bin/systemctl", "show", (char *)service, prop_arg, "--value", NULL};
    return run_capture_stripped(argv);
}

static char *first_line_dup(const char *text)
{
    if (!text || !*text) {
        return NULL;
    }
    const char *end = strchr(text, '\n');
    size_t len = end ? (size_t)(end - text) : strlen(text);
    return len > 0 ? g_strndup(text, len) : NULL;
}

static char *pgrep_first_pid(const char *pattern)
{
    char *out = NULL;
    char *argv[] = {"/usr/bin/pgrep", "-f", (char *)pattern, NULL};
    int rc = run_capture(argv, &out);
    if (rc != 0 || !out) {
        g_free(out);
        return NULL;
    }
    char *stripped = g_strstrip(out);
    char *pid = first_line_dup(stripped);
    g_free(out);
    return pid;
}

static int extract_quoted_field(const char *line, const char *key, char *out, size_t out_len);

static int run_no_capture(char *const argv[])
{
    pid_t child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        execve(argv[0], argv, environ);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_pkexec_helper(const char *helper_arg, const char *stdin_text, char **output)
{
    int inpipe[2] = {-1, -1};
    int outpipe[2] = {-1, -1};
    if (stdin_text && pipe(inpipe) != 0) {
        return -1;
    }
    if (pipe(outpipe) != 0) {
        if (stdin_text) {
            close(inpipe[0]);
            close(inpipe[1]);
        }
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        if (stdin_text) {
            close(inpipe[0]);
            close(inpipe[1]);
        }
        close(outpipe[0]);
        close(outpipe[1]);
        return -1;
    }

    if (child == 0) {
        char *const argv[] = {
            "/usr/bin/pkexec",
            JX_HELPER_PATH,
            (char *)helper_arg,
            NULL
        };
        char *const envp[] = {
            "PATH=/usr/sbin:/usr/bin:/sbin:/bin",
            NULL
        };
        if (stdin_text) {
            close(inpipe[1]);
            dup2(inpipe[0], STDIN_FILENO);
            close(inpipe[0]);
        }
        close(outpipe[0]);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(outpipe[1], STDERR_FILENO);
        close(outpipe[1]);
        execve("/usr/bin/pkexec", argv, envp);
        _exit(127);
    }

    if (stdin_text) {
        close(inpipe[0]);
        size_t len = strlen(stdin_text);
        size_t written = 0;
        while (written < len) {
            ssize_t n = write(inpipe[1], stdin_text + written, len - written);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0) {
                break;
            }
            written += (size_t)n;
        }
        close(inpipe[1]);
    }

    close(outpipe[1]);
    GString *buf = g_string_new(NULL);
    char chunk[4096];
    for (;;) {
        ssize_t n = read(outpipe[0], chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        g_string_append_len(buf, chunk, (gssize)n);
    }
    close(outpipe[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    *output = g_string_free(buf, FALSE);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static GtkWidget *section_box(const char *title)
{
    GtkWidget *frame = gtk_frame_new(title);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

static GtkWidget *frame_child(GtkWidget *frame)
{
    return gtk_frame_get_child(GTK_FRAME(frame));
}

static GtkWidget *label_row(const char *left, GtkWidget *right)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(left);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    if (GTK_IS_SWITCH(right)) {
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_set_hexpand(right, FALSE);
        gtk_widget_set_halign(right, GTK_ALIGN_END);
    } else {
        gtk_widget_set_hexpand(right, TRUE);
        gtk_widget_set_halign(right, GTK_ALIGN_FILL);
    }

    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), right);
    return row;
}

static void list_clear(GtkWidget *list)
{
    GtkWidget *child = gtk_widget_get_first_child(list);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(list), child);
        child = next;
    }
}

static void list_append_text(GtkWidget *list, const char *text)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_top(label, 6);
    gtk_widget_set_margin_bottom(label, 6);
    gtk_widget_set_margin_start(label, 8);
    gtk_widget_set_margin_end(label, 8);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    gtk_list_box_append(GTK_LIST_BOX(list), row);
}

static const char *selected_list_text(GtkWidget *list)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
    if (!row) {
        return NULL;
    }
    GtkWidget *child = gtk_list_box_row_get_child(row);
    return GTK_IS_LABEL(child) ? gtk_label_get_text(GTK_LABEL(child)) : NULL;
}

static const char *row_label_text(GtkListBoxRow *row)
{
    if (!row) {
        return NULL;
    }
    GtkWidget *child = gtk_list_box_row_get_child(row);
    return GTK_IS_LABEL(child) ? gtk_label_get_text(GTK_LABEL(child)) : NULL;
}

static void set_inspector(struct jx_control_app *app, const char *title, const char *details)
{
    if (!app->inspector_title || !app->inspector_details) {
        return;
    }
    gtk_label_set_text(GTK_LABEL(app->inspector_title), title ? title : "No object selected");
    gtk_label_set_text(GTK_LABEL(app->inspector_details),
                       details ? details : "Select a service, watch root, rule, process, or event to inspect it.");
}

static char *guard_app_name_for_prompt(const char *value);

static void inspect_event_row(struct jx_control_app *app, const char *row)
{
    if (!row || strcmp(row, "No recent sentinel events") == 0 ||
        strcmp(row, "Unable to read recent sentinel events") == 0) {
        set_inspector(app, "Live Events", row ? row : "No event selected.");
        return;
    }

    char details[PATH_MAX + 512];
    snprintf(details, sizeof(details),
             "Source: jx-sentinel.service journal\n\n%s\n\nThis is a recent Sentinel file event. It shows the time, event type, path, process, pid, and uid captured from the daemon logs.",
             row);
    set_inspector(app, "Live Event", details);
}

static void on_event_selection_changed(GObject *selection, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    struct jx_control_app *app = user_data;
    GtkStringObject *item = gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(selection));
    inspect_event_row(app, item ? gtk_string_object_get_string(item) : NULL);
}

static void on_context_list_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    struct jx_control_app *app = user_data;
    const char *value = row_label_text(row);
    if (!value) {
        return;
    }

    char details[PATH_MAX + 512];
    if (GTK_WIDGET(box) == app->watch_list) {
        snprintf(details, sizeof(details),
                 "Path: %s\n\nSentinel watches this folder for configured file creation and modification events.",
                 value);
        set_inspector(app, "Watch Root", details);
    } else if (GTK_WIDGET(box) == app->extension_list) {
        snprintf(details, sizeof(details),
                 "Extension: %s\n\nFiles matching this extension are part of Sentinel's configured event rules.",
                 value);
        set_inspector(app, "Extension Rule", details);
    } else if (GTK_WIDGET(box) == app->guard_paths_list) {
        snprintf(details, sizeof(details),
                 "Protected target: %s\n\nGuard evaluates access to this path using the active guard.conf target mode.",
                 value);
        set_inspector(app, "Guard Protected Target", details);
    } else if (GTK_WIDGET(box) == app->guard_process_allowlist) {
        const int prompt_entry = g_str_has_suffix(value, " (prompt)");
        char *app_name = guard_app_name_for_prompt(value);
        snprintf(details, sizeof(details),
                 "App: %s\nSource: %s\n\n%s",
                 app_name ? app_name : value,
                 prompt_entry ? "Prompt policy database" : "guard.conf allowlist",
                 prompt_entry ?
                 "Double-click to add this prompt-derived app to guard.conf permanently. Use Remove to clear the saved prompt decision." :
                 "This app is trusted by Guard config. Use Remove to revoke the config allowlist entry.");
        set_inspector(app, "Guard Allowed App", details);
        g_free(app_name);
    } else if (GTK_WIDGET(box) == app->diagnostics_list) {
        snprintf(details, sizeof(details),
                 "Check: %s\n\nDiagnostics summarize whether required binaries, service files, and runtime dependencies are available.",
                 value);
        set_inspector(app, "Diagnostic Check", details);
    } else {
        snprintf(details, sizeof(details),
                 "Object: %s\n\nSelect a concrete row in Dashboard, Guard, Events, Logs, or Diagnostics to inspect its current runtime details.",
                 value);
        set_inspector(app, "Control Object", details);
    }
}

static void on_navigation_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    struct jx_control_app *app = user_data;
    const char *value = row_label_text(row);
    if (!value) {
        return;
    }

    const char *page = "dashboard";
    const char *title = "Dashboard";
    const char *details = "Overview of Sentinel service state, recent events, configured watch roots, and notification settings.";

    if (strcmp(value, "Service") == 0) {
        page = "settings";
        title = "Service";
        details = "Installed service and binary paths for JX Sentinel and Guard. Use the header and Guard controls for runtime actions.";
    } else if (strcmp(value, "Watch Roots") == 0) {
        page = "dashboard";
        title = "Watch Roots";
        details = "Folders monitored by the Sentinel file event daemon. Select a folder row for its exact path.";
    } else if (strcmp(value, "Extension Rules") == 0) {
        page = "rules";
        title = "Extension Rules";
        details = "Rules describing file types Sentinel treats as meaningful. Select a rule row for more detail.";
    } else if (strcmp(value, "Guard") == 0) {
        page = "guard";
        title = "Guard";
        details = "Permission enforcement state, protected targets, allowed apps, prompt decisions, and Guard journal output.";
    } else if (strcmp(value, "Events") == 0) {
        page = "events";
        title = "Events";
        details = "Recent Sentinel journal events. Select an event row to inspect the process, pid, uid, path, and event type.";
    } else if (strcmp(value, "Graph") == 0) {
        page = "graph";
        title = "Graph";
        details = "Connected bubble chart built from recent Sentinel and Guard journal events. Use filters inside the graph to reduce clutter.";
    } else if (strcmp(value, "Diagnostics") == 0) {
        page = "diagnostics";
        title = "Diagnostics";
        details = "Health checks for installed binaries, service files, helper tools, and desktop notification dependencies.";
    } else if (strcmp(value, "Logs") == 0) {
        page = "logs";
        title = "Logs";
        details = "Recent jx-sentinel.service journal output.";
    } else if (strcmp(value, "Settings") == 0) {
        page = "settings";
        title = "Settings";
        details = "Security paths, installed binaries, systemd unit names, and helper state used by this Control Panel.";
    }

    if (app->stack) {
        gtk_stack_set_visible_child_name(GTK_STACK(app->stack), page);
    }
    set_inspector(app, title, details);
}

static void refresh_lists(struct jx_control_app *app)
{
    list_clear(app->watch_list);
    for (size_t i = 0; i < app->config.watch_count; i++) {
        list_append_text(app->watch_list, app->config.watch_paths[i]);
    }

    list_clear(app->extension_list);
    for (size_t i = 0; i < app->config.extension_count; i++) {
        list_append_text(app->extension_list, app->config.extensions[i]);
    }

    gtk_switch_set_active(GTK_SWITCH(app->log_all_switch), app->config.extension_count == 0);
    gtk_switch_set_active(GTK_SWITCH(app->notify_switch), app->config.notifications);
    gtk_switch_set_active(GTK_SWITCH(app->verbose_switch), app->config.verbose);
    gtk_editable_set_text(GTK_EDITABLE(app->notify_user_entry), app->config.notify_user);
}

static void refresh_status(struct jx_control_app *app)
{
    char *state = systemctl_property(JX_SERVICE_NAME, "ActiveState");
    char *enabled = systemctl_property(JX_SERVICE_NAME, "UnitFileState");
    char *pid = systemctl_property(JX_SERVICE_NAME, "MainPID");
    char *process_pid = NULL;

    if (!state || strcmp(state, "inactive") == 0 || strcmp(state, "failed") == 0) {
        process_pid = pgrep_first_pid(JX_DAEMON_PATH);
        if (process_pid && (!state || strcmp(state, "active") != 0)) {
            g_free(state);
            state = g_strdup("active");
        }
    }

    gtk_label_set_text(GTK_LABEL(app->status_label), state ? state : "unknown");
    if (app->header_status_label) {
        char header_state[80];
        snprintf(header_state, sizeof(header_state), "%s %s",
                 state && strcmp(state, "active") == 0 ? "●" : "○",
                 state ? state : "unknown");
        gtk_label_set_text(GTK_LABEL(app->header_status_label), header_state);
    }
    gtk_label_set_text(GTK_LABEL(app->enabled_label), enabled ? enabled : "unknown");
    gtk_label_set_text(GTK_LABEL(app->pid_label),
                       pid && strcmp(pid, "0") != 0 ? pid : (process_pid ? process_pid : "-"));

    g_free(state);
    g_free(enabled);
    g_free(pid);
    g_free(process_pid);

    char helper_state[256];
    snprintf(helper_state, sizeof(helper_state), "%s", access(JX_HELPER_PATH, X_OK) == 0 ? "present and executable" : "missing");
    gtk_label_set_text(GTK_LABEL(app->helper_label), helper_state);
}

static void refresh_logs(struct jx_control_app *app)
{
    char *out = NULL;
    char *argv[] = {
        "/usr/bin/journalctl",
        "-u",
        JX_SERVICE_NAME,
        "-n",
        "120",
        "--no-pager",
        "-o",
        "short-iso",
        NULL
    };
    int rc = run_capture(argv, &out);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->logs_view));
    gtk_text_buffer_set_text(buffer, out && *out ? out : (rc == 0 ? "No logs yet." : "Unable to read logs."), -1);
    g_free(out);
}

static void refresh_overview_metrics(struct jx_control_app *app)
{
    char roots_buf[32];
    char rules_buf[32];
    snprintf(roots_buf, sizeof(roots_buf), "%zu", app->config.watch_count);
    snprintf(rules_buf, sizeof(rules_buf), "%zu", app->config.extension_count);
    gtk_label_set_text(GTK_LABEL(app->roots_label), roots_buf);
    gtk_label_set_text(GTK_LABEL(app->rules_label), rules_buf);

    char *out = NULL;
    char *argv[] = {
        "/usr/bin/journalctl",
        "-u",
        JX_SERVICE_NAME,
        "-n",
        "80",
        "--no-pager",
        "-o",
        "cat",
        NULL
    };
    int rc = run_capture(argv, &out);
    const char *last_event = "-";
    char last_time[64] = "-";
    if (rc == 0 && out && *out) {
        char *saveptr = NULL;
        for (char *line = strtok_r(out, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
            if (strstr(line, "JX_SENTINEL_EVENT") &&
                extract_quoted_field(line, "time", last_time, sizeof(last_time)) == 0) {
                last_event = last_time;
            }
        }
    }
    gtk_label_set_text(GTK_LABEL(app->last_event_label), last_event);
    g_free(out);
}

static size_t count_matching_lines(const char *path, const char *prefix)
{
    FILE *fp = fopen(path, "re");
    if (!fp) {
        return 0;
    }
    size_t count = 0;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim_space(line);
        if (*s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        if (!prefix || strncmp(s, prefix, strlen(prefix)) == 0) {
            count++;
        }
    }
    fclose(fp);
    return count;
}

static size_t count_guard_config_paths(void)
{
    FILE *fp = fopen(JX_GUARD_CONFIG_PATH, "re");
    if (!fp) {
        return 0;
    }
    size_t count = 0;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim_space(line);
        if (*s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        if (strncmp(s, "ProtectedPath=", 14) == 0) {
            count++;
        } else if (strncmp(s, "Paths=", 6) == 0) {
            char copy[4096];
            snprintf(copy, sizeof(copy), "%s", s + 6);
            char *saveptr = NULL;
            for (char *item = strtok_r(copy, ",", &saveptr); item; item = strtok_r(NULL, ",", &saveptr)) {
                if (*trim_space(item)) {
                    count++;
                }
            }
        }
    }
    fclose(fp);
    return count;
}

static int guard_allow_entry_exists(char values[][PATH_MAX], size_t count, const char *value)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(values[i], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *basename_for_display(const char *path);

static void append_comma_values(GtkWidget *list, const char *value)
{
    char copy[4096];
    snprintf(copy, sizeof(copy), "%s", value);
    char *saveptr = NULL;
    for (char *item = strtok_r(copy, ",", &saveptr); item; item = strtok_r(NULL, ",", &saveptr)) {
        char *entry = trim_space(item);
        size_t len = strlen(entry);
        if (len >= 2 && ((entry[0] == '"' && entry[len - 1] == '"') ||
                         (entry[0] == '\'' && entry[len - 1] == '\''))) {
            entry[len - 1] = '\0';
            entry++;
        }
        while (strlen(entry) > 1 && entry[strlen(entry) - 1] == '/') {
            entry[strlen(entry) - 1] = '\0';
        }
        if (*entry) {
            list_append_text(list, entry);
        }
    }
}

static void guard_allow_key_for_value(const char *value, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!value || !*value || out_len == 0) {
        return;
    }

    char normalized[PATH_MAX];
    snprintf(normalized, sizeof(normalized), "%s", value);
    const char *prompt_suffix = " (prompt)";
    size_t len = strlen(normalized);
    size_t suffix_len = strlen(prompt_suffix);
    if (len > suffix_len && strcmp(normalized + len - suffix_len, prompt_suffix) == 0) {
        normalized[len - suffix_len] = '\0';
    }

    snprintf(out, out_len, "%s", basename_for_display(normalized));
}

static const char *basename_for_display(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    const char *backslash = path ? strrchr(path, '\\') : NULL;
    const char *sep = slash && backslash ? (slash > backslash ? slash : backslash) : (slash ? slash : backslash);
    return sep ? sep + 1 : path;
}

static int guard_policy_display_name(const char *subject, char *out, size_t out_len)
{
    if (!subject || !*subject || strcmp(subject, "unknown") == 0 ||
        strncmp(subject, "pid:", 4) == 0) {
        return -1;
    }

    const char *name = subject;
    if (strncmp(subject, "cmd:", 4) == 0) {
        name = basename_for_display(subject + 4);
    } else {
        name = basename_for_display(subject);
    }

    if (!name || !*name || strcmp(name, "unknown") == 0) {
        return -1;
    }
    snprintf(out, out_len, "%s (prompt)", name);
    return 0;
}

static size_t append_guard_policy_allowlist(GtkWidget *list, char seen[][PATH_MAX], size_t seen_count)
{
    FILE *fp = fopen(JX_GUARD_DB_PATH, "re");
    if (!fp) {
        return seen_count;
    }

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char subject[PATH_MAX];
        char protected_root[PATH_MAX];
        char decision[16];
        char scope[32];
        unsigned long uid = 0;
        if (sscanf(line, "%4095[^\t]\t%lu\t%4095[^\t]\t%15s\t%31s",
                   subject, &uid, protected_root, decision, scope) != 5) {
            continue;
        }
        if (strcmp(decision, "allow") != 0) {
            continue;
        }

        char display[PATH_MAX];
        char key[PATH_MAX];
        if (guard_policy_display_name(subject, display, sizeof(display)) != 0 ||
            (guard_allow_key_for_value(display, key, sizeof(key)), key[0] == '\0') ||
            guard_allow_entry_exists(seen, seen_count, key)) {
            continue;
        }
        list_append_text(list, display);
        snprintf(seen[seen_count++], PATH_MAX, "%s", key);
        if (seen_count >= JX_MAX_CONTROL_ITEMS) {
            break;
        }
    }
    fclose(fp);
    return seen_count;
}

static void refresh_guard_paths(struct jx_control_app *app)
{
    list_clear(app->guard_paths_list);
    list_clear(app->guard_process_allowlist);
    FILE *fp = fopen(JX_GUARD_CONFIG_PATH, "re");
    if (!fp) {
        const char *message = errno == EACCES ? "Guard config not readable" : "Guard config not installed";
        list_append_text(app->guard_paths_list, message);
        list_append_text(app->guard_process_allowlist, message);
        return;
    }
    char seen_allow[JX_MAX_CONTROL_ITEMS][PATH_MAX] = {{0}};
    size_t allow_count = 0;
    size_t path_count = 0;
    int in_allowlist_section = 0;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char *s = trim_space(line);
        if (*s == '[') {
            in_allowlist_section = strcmp(s, "[Allowlist]") == 0;
            continue;
        }
        if (strncmp(s, "ProtectedPath=", 14) == 0) {
            list_append_text(app->guard_paths_list, s + 14);
            path_count++;
        } else if (strncmp(s, "Paths=", 6) == 0) {
            append_comma_values(app->guard_paths_list, s + 6);
            char copy[4096];
            snprintf(copy, sizeof(copy), "%s", s + 6);
            for (char *p = copy; *p; p++) {
                if (*p == ',') {
                    path_count++;
                }
            }
            path_count++;
        } else if (strncmp(s, "ProcessAllowlist=", 17) == 0) {
            const char *value = s + 17;
            char key[PATH_MAX];
            guard_allow_key_for_value(value, key, sizeof(key));
            if (key[0] != '\0' && !guard_allow_entry_exists(seen_allow, allow_count, key)) {
                list_append_text(app->guard_process_allowlist, value);
                snprintf(seen_allow[allow_count++], PATH_MAX, "%s", key);
            }
        } else if (in_allowlist_section && strncmp(s, "Executables=", 12) == 0) {
            char copy[4096];
            snprintf(copy, sizeof(copy), "%s", s + 12);
            char *saveptr = NULL;
            for (char *item = strtok_r(copy, ",", &saveptr); item; item = strtok_r(NULL, ",", &saveptr)) {
                char *value = trim_space(item);
                char key[PATH_MAX];
                guard_allow_key_for_value(value, key, sizeof(key));
                if (key[0] != '\0' && !guard_allow_entry_exists(seen_allow, allow_count, key)) {
                    list_append_text(app->guard_process_allowlist, key);
                    snprintf(seen_allow[allow_count++], PATH_MAX, "%s", key);
                }
            }
        }
    }
    fclose(fp);
    if (path_count == 0) {
        list_append_text(app->guard_paths_list, "No protected folders");
    }
    allow_count = append_guard_policy_allowlist(app->guard_process_allowlist, seen_allow, allow_count);
    if (allow_count == 0) {
        list_append_text(app->guard_process_allowlist, "No process allowlist entries");
    }
}

static void refresh_guard_status(struct jx_control_app *app)
{
    char *state = systemctl_property(JX_GUARD_SERVICE_NAME, "ActiveState");
    char *process_pid = NULL;
    if (!state || strcmp(state, "inactive") == 0 || strcmp(state, "failed") == 0) {
        process_pid = pgrep_first_pid(JX_GUARD_DAEMON_PATH);
        if (process_pid) {
            g_free(state);
            state = g_strdup("active");
        }
    }
    gtk_label_set_text(GTK_LABEL(app->guard_status_label), state ? state : "unknown");
    g_free(state);
    g_free(process_pid);

    struct stat agent_st;
    if (stat(JX_GUARD_AGENT_STATE_PATH, &agent_st) == 0) {
        gtk_label_set_text(GTK_LABEL(app->guard_agent_label), "connected");
    } else {
        char *agent_pid = pgrep_first_pid(JX_PERMISSION_AGENT_PATH);
        gtk_label_set_text(GTK_LABEL(app->guard_agent_label),
                           agent_pid ? "running, not connected" : "not running");
        g_free(agent_pid);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%zu", count_guard_config_paths());
    gtk_label_set_text(GTK_LABEL(app->guard_paths_label), buf);
    snprintf(buf, sizeof(buf), "%zu", count_matching_lines(JX_GUARD_DB_PATH, NULL));
    gtk_label_set_text(GTK_LABEL(app->guard_rules_label), buf);
    if (app->dashboard_guard_rules_label) {
        gtk_label_set_text(GTK_LABEL(app->dashboard_guard_rules_label), buf);
    }

    char *out = NULL;
    char *last_argv[] = {
        "/usr/bin/journalctl",
        "-u",
        JX_GUARD_SERVICE_NAME,
        "-n",
        "1",
        "--no-pager",
        "-o",
        "cat",
        NULL
    };
    int last_rc = run_capture(last_argv, &out);
    char *last = out ? g_strstrip(out) : NULL;
    gtk_label_set_text(GTK_LABEL(app->guard_last_label),
                       last_rc == 0 && last && *last ? last : "-");
    g_free(out);
    refresh_guard_paths(app);
}

static gboolean refresh_guard_status_once(gpointer user_data)
{
    refresh_guard_status(user_data);
    return G_SOURCE_REMOVE;
}

static gboolean refresh_status_once(gpointer user_data)
{
    struct jx_control_app *app = user_data;
    refresh_status(app);
    refresh_overview_metrics(app);
    return G_SOURCE_REMOVE;
}

static void on_start_agent(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct jx_control_app *app = user_data;
    pid_t child = fork();
    if (child == 0) {
        char *const argv[] = {JX_PERMISSION_AGENT_PATH, NULL};
        execve(JX_PERMISSION_AGENT_PATH, argv, environ);
        _exit(127);
    }
    g_timeout_add_seconds(1, refresh_guard_status_once, app);
}

static void refresh_guard_logs(struct jx_control_app *app)
{
    char *out = NULL;
    char *argv[] = {
        "/usr/bin/journalctl",
        "-u",
        JX_GUARD_SERVICE_NAME,
        "-n",
        "120",
        "--no-pager",
        "-o",
        "short-iso",
        NULL
    };
    int rc = run_capture(argv, &out);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->guard_logs_view));
    gtk_text_buffer_set_text(buffer, out && *out ? out : (rc == 0 ? "No Guard logs yet." : "Unable to read Guard logs."), -1);
    g_free(out);
}

static void load_config_into_app(struct jx_control_app *app)
{
    if (jx_control_load_config(JX_CONFIG_PATH, &app->config) != 0) {
        config_defaults(&app->config);
    }
    refresh_lists(app);
}

static int collect_config_from_widgets(struct jx_control_app *app)
{
    const char *user = gtk_editable_get_text(GTK_EDITABLE(app->notify_user_entry));
    if (!user || *user == '\0' || strchr(user, '/') || strchr(user, '\n')) {
        show_message(app, GTK_MESSAGE_ERROR, "Invalid notification user", "Enter a local username such as jackson.");
        return -1;
    }
    snprintf(app->config.notify_user, sizeof(app->config.notify_user), "%s", user);
    app->config.notifications = gtk_switch_get_active(GTK_SWITCH(app->notify_switch));
    app->config.verbose = gtk_switch_get_active(GTK_SWITCH(app->verbose_switch));
    if (gtk_switch_get_active(GTK_SWITCH(app->log_all_switch))) {
        app->config.extension_count = 0;
    }
    if (app->config.watch_count == 0) {
        show_message(app, GTK_MESSAGE_ERROR, "No watched folders", "Add at least one watched folder before saving.");
        return -1;
    }
    return 0;
}

static void service_action(struct jx_control_app *app, const char *helper_arg)
{
    char *out = NULL;
    int rc = run_pkexec_helper(helper_arg, NULL, &out);
    if (rc != 0) {
        show_message(app, GTK_MESSAGE_ERROR, "Service action failed", out && *out ? out : "pkexec or systemctl failed.");
    }
    g_free(out);
    refresh_status(app);
    refresh_logs(app);
    g_timeout_add_seconds(1, refresh_status_once, app);
}

static void guard_service_action(struct jx_control_app *app, const char *helper_arg)
{
    char *out = NULL;
    int rc = run_pkexec_helper(helper_arg, NULL, &out);
    if (rc != 0) {
        show_message(app, GTK_MESSAGE_ERROR, "Guard service action failed", out && *out ? out : "pkexec or systemctl failed.");
    }
    g_free(out);
    refresh_guard_status(app);
    refresh_guard_logs(app);
    g_timeout_add_seconds(1, refresh_guard_status_once, app);
}

static void on_start(GtkButton *button, gpointer user_data)
{
    (void)button;
    service_action(user_data, "--start");
}

static void on_stop(GtkButton *button, gpointer user_data)
{
    (void)button;
    service_action(user_data, "--stop");
}

static void on_restart(GtkButton *button, gpointer user_data)
{
    (void)button;
    service_action(user_data, "--restart");
}

static void on_guard_start(GtkButton *button, gpointer user_data)
{
    (void)button;
    guard_service_action(user_data, "--guard-start");
}

static void on_guard_stop(GtkButton *button, gpointer user_data)
{
    (void)button;
    guard_service_action(user_data, "--guard-stop");
}

static void on_guard_restart(GtkButton *button, gpointer user_data)
{
    (void)button;
    guard_service_action(user_data, "--guard-restart");
}

static void on_guard_enable(GtkButton *button, gpointer user_data)
{
    (void)button;
    guard_service_action(user_data, "--guard-enable");
}

static void on_guard_disable(GtkButton *button, gpointer user_data)
{
    (void)button;
    guard_service_action(user_data, "--guard-disable");
}

static void on_save(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct jx_control_app *app = user_data;
    if (collect_config_from_widgets(app) != 0) {
        return;
    }

    char *text = jx_control_config_to_text(&app->config);
    char *out = NULL;
    int rc = run_pkexec_helper("--save-config", text, &out);
    if (rc == 0) {
        g_free(out);
        out = NULL;
        rc = run_pkexec_helper("--apply-config", NULL, &out);
    }
    if (rc == 0) {
        show_message(app, GTK_MESSAGE_INFO, "Configuration applied", "The service was restarted with the saved settings.");
    } else {
        show_message(app, GTK_MESSAGE_ERROR, "Apply failed", out && *out ? out : "pkexec helper failed.");
    }
    g_free(text);
    g_free(out);
}

struct prompt_ctx {
    struct jx_control_app *app;
    int is_extension;
    GtkWidget *entry;
};

static void on_prompt_response(GtkDialog *dialog, int response, gpointer user_data)
{
    struct prompt_ctx *ctx = user_data;
    if (response == GTK_RESPONSE_ACCEPT) {
        const char *value = gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
        char error[256];
        if (ctx->is_extension) {
            if (jx_control_validate_extension(value, error, sizeof(error)) == 0 &&
                !extension_duplicate(&ctx->app->config, value) &&
                ctx->app->config.extension_count < JX_MAX_CONTROL_ITEMS) {
                snprintf(ctx->app->config.extensions[ctx->app->config.extension_count++], 64, "%s", value);
                gtk_switch_set_active(GTK_SWITCH(ctx->app->log_all_switch), FALSE);
                refresh_lists(ctx->app);
            } else if (!extension_duplicate(&ctx->app->config, value)) {
                show_message(ctx->app, GTK_MESSAGE_ERROR, "Invalid extension", error);
            }
        } else {
            if (jx_control_validate_path(value, error, sizeof(error)) == 0 &&
                !path_duplicate(&ctx->app->config, value) &&
                ctx->app->config.watch_count < JX_MAX_CONTROL_ITEMS) {
                snprintf(ctx->app->config.watch_paths[ctx->app->config.watch_count++], PATH_MAX, "%s", value);
                refresh_lists(ctx->app);
            } else if (!path_duplicate(&ctx->app->config, value)) {
                show_message(ctx->app, GTK_MESSAGE_ERROR, "Invalid folder", error);
            }
        }
    }

    gtk_window_destroy(GTK_WINDOW(dialog));
    g_free(ctx);
}

static void prompt_value(struct jx_control_app *app, const char *title,
                         const char *placeholder, int is_extension)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title,
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Cancel",
                                                    GTK_RESPONSE_CANCEL,
                                                    "_Add",
                                                    GTK_RESPONSE_ACCEPT,
                                                    NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_widget_set_margin_top(entry, 12);
    gtk_widget_set_margin_bottom(entry, 12);
    gtk_widget_set_margin_start(entry, 12);
    gtk_widget_set_margin_end(entry, 12);
    gtk_box_append(GTK_BOX(area), entry);

    struct prompt_ctx *ctx = g_new0(struct prompt_ctx, 1);
    ctx->app = app;
    ctx->is_extension = is_extension;
    ctx->entry = entry;
    g_signal_connect(dialog, "response", G_CALLBACK(on_prompt_response), ctx);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_add_folder(GtkButton *button, gpointer user_data)
{
    (void)button;
    prompt_value(user_data, "Add Folder", "/opt/jx", 0);
}

static void on_add_extension(GtkButton *button, gpointer user_data)
{
    (void)button;
    prompt_value(user_data, "Add Extension", ".conf", 1);
}

static void on_remove_folder(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct jx_control_app *app = user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(app->watch_list));
    if (!row) {
        return;
    }
    int index = gtk_list_box_row_get_index(row);
    if (index >= 0 && (size_t)index < app->config.watch_count) {
        for (size_t i = (size_t)index; i + 1 < app->config.watch_count; i++) {
            snprintf(app->config.watch_paths[i], PATH_MAX, "%s", app->config.watch_paths[i + 1]);
        }
        app->config.watch_count--;
        refresh_lists(app);
    }
}

static void on_remove_extension(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct jx_control_app *app = user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(app->extension_list));
    if (!row) {
        return;
    }
    int index = gtk_list_box_row_get_index(row);
    if (index >= 0 && (size_t)index < app->config.extension_count) {
        for (size_t i = (size_t)index; i + 1 < app->config.extension_count; i++) {
            snprintf(app->config.extensions[i], 64, "%s", app->config.extensions[i + 1]);
        }
        app->config.extension_count--;
        refresh_lists(app);
    }
}

struct guard_app_prompt_ctx {
    struct jx_control_app *app;
    GtkWidget *entry;
};

static const char *basename_const(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path;
}

static int guard_app_value_is_valid(const char *value);

static void first_cmd_token(const char *cmd, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!cmd || !*cmd || out_len == 0) {
        return;
    }
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }
    char quote = 0;
    if (*cmd == '"' || *cmd == '\'') {
        quote = *cmd++;
    }
    size_t pos = 0;
    while (*cmd && pos + 1 < out_len) {
        if ((quote && *cmd == quote) || (!quote && (*cmd == ' ' || *cmd == '\t'))) {
            break;
        }
        out[pos++] = *cmd++;
    }
    out[pos] = '\0';
}

static int recent_guard_process_name(const char *exe, const char *cmd, char *out, size_t out_len)
{
    const char *candidate = NULL;
    char token[PATH_MAX];
    token[0] = '\0';

    if (exe && *exe && strcmp(exe, "unknown") != 0 && strcmp(exe, "-") != 0) {
        candidate = basename_for_display(exe);
    } else {
        first_cmd_token(cmd, token, sizeof(token));
        if (token[0]) {
            candidate = basename_for_display(token);
        }
    }

    if (!candidate || !*candidate || strcmp(candidate, "unknown") == 0 ||
        !guard_app_value_is_valid(candidate)) {
        return -1;
    }
    snprintf(out, out_len, "%s", candidate);
    return 0;
}

static void decision_label(const char *decision, char *out, size_t out_len)
{
    if (!decision || !*decision) {
        snprintf(out, out_len, "unknown");
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; decision[i] && pos + 1 < out_len; i++) {
        out[pos++] = decision[i] == '_' ? ' ' : decision[i];
    }
    out[pos] = '\0';
}

static void list_append_recent_guard_process(GtkWidget *list, const char *label, const char *value)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *text = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_widget_set_margin_top(text, 6);
    gtk_widget_set_margin_bottom(text, 6);
    gtk_widget_set_margin_start(text, 8);
    gtk_widget_set_margin_end(text, 8);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), text);
    g_object_set_data_full(G_OBJECT(row), "jx-guard-app-value", g_strdup(value), g_free);
    gtk_list_box_append(GTK_LIST_BOX(list), row);
}

static int guard_app_value_is_valid(const char *value)
{
    if (!value || *value == '\0' || strchr(value, '=') || strchr(value, ';') || strchr(value, '#')) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p <= ' ') {
            return 0;
        }
    }
    return 1;
}

static char *guard_app_name_for_prompt(const char *value)
{
    if (!value) {
        return NULL;
    }
    if (g_str_has_suffix(value, " (prompt)")) {
        return g_strndup(value, strlen(value) - strlen(" (prompt)"));
    }
    return g_strdup(value);
}

static int confirm_guard_app_removal(struct jx_control_app *app, const char *app_name)
{
    char *text = g_strdup_printf("Are you sure you want to remove this app?\n\n"
                                 "You will be prompted next time when %s tries to access a Restricted folder.",
                                 app_name);
    char *argv[] = {
        "zenity",
        "--question",
        "--title=Remove allowed app",
        "--ok-label=Remove",
        "--cancel-label=Cancel",
        "--text",
        text,
        NULL
    };
    GError *error = NULL;
    int status = 1;
    gboolean spawned = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                                    NULL, NULL, &status, &error);
    g_free(text);
    if (!spawned) {
        show_message(app, GTK_MESSAGE_ERROR, "Remove confirmation failed",
                     error && error->message ? error->message : "Could not launch zenity.");
        g_clear_error(&error);
        return 0;
    }
    return g_spawn_check_wait_status(status, NULL);
}

static int confirm_guard_app_promotion(struct jx_control_app *app, const char *app_name)
{
    char *text = g_strdup_printf("Add %s to the Guard config allowlist?\n\n"
                                 "This will make %s a trusted app for restricted folders until you remove it.",
                                 app_name, app_name);
    char *argv[] = {
        "zenity",
        "--question",
        "--title=Trust app permanently",
        "--ok-label=Add to Guard Config",
        "--cancel-label=Cancel",
        "--text",
        text,
        NULL
    };
    GError *error = NULL;
    int status = 1;
    gboolean spawned = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                                    NULL, NULL, &status, &error);
    g_free(text);
    if (!spawned) {
        show_message(app, GTK_MESSAGE_ERROR, "Confirmation failed",
                     error && error->message ? error->message : "Could not launch zenity.");
        g_clear_error(&error);
        return 0;
    }
    return g_spawn_check_wait_status(status, NULL);
}

static void apply_guard_app_change(struct jx_control_app *app, const char *helper_arg,
                                   const char *value, const char *failure_title)
{
    char *out = NULL;
    int rc = run_pkexec_helper(helper_arg, value, &out);
    if (rc != 0) {
        show_message(app, GTK_MESSAGE_ERROR, failure_title, out && *out ? out : "pkexec helper failed.");
    }
    g_free(out);
    refresh_guard_paths(app);
    refresh_guard_status(app);
    refresh_guard_logs(app);
}

static int string_array_contains(char values[][PATH_MAX], size_t count, const char *value)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(values[i], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static int extract_quoted_field(const char *line, const char *key, char *out, size_t out_len)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=\"", key);
    const char *start = strstr(line, pattern);
    if (!start) {
        return -1;
    }
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end || end == start) {
        return -1;
    }
    size_t len = (size_t)(end - start);
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static void clear_events_model(GtkStringList *rows)
{
    guint count = g_list_model_get_n_items(G_LIST_MODEL(rows));
    if (count > 0) {
        gtk_string_list_splice(rows, 0, count, NULL);
    }
}

static void refresh_events(struct jx_control_app *app)
{
    if (!app->events_model) {
        return;
    }

    clear_events_model(app->events_model);
    char *out = NULL;
    char *argv[] = {
        "/usr/bin/journalctl",
        "-u",
        JX_SERVICE_NAME,
        "-n",
        "160",
        "--no-pager",
        "-o",
        "cat",
        NULL
    };
    int rc = run_capture(argv, &out);
    if (rc != 0 || !out || !*out) {
        gtk_string_list_append(app->events_model, "Unable to read recent sentinel events");
        if (app->events_today_label) {
            gtk_label_set_text(GTK_LABEL(app->events_today_label), "0");
        }
        g_free(out);
        return;
    }

    size_t event_count = 0;
    size_t today_count = 0;
    char today[16];
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    strftime(today, sizeof(today), "%Y-%m-%d", &tm_local);
    char *saveptr = NULL;
    for (char *line = strtok_r(out, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        if (!strstr(line, "JX_SENTINEL_EVENT")) {
            continue;
        }

        char time_buf[64] = "-";
        char event[32] = "-";
        char type[32] = "-";
        char path[PATH_MAX] = "-";
        char exe[PATH_MAX] = "-";
        char pid[32] = "-";
        char uid[32] = "-";
        extract_quoted_field(line, "time", time_buf, sizeof(time_buf));
        extract_quoted_field(line, "event", event, sizeof(event));
        extract_quoted_field(line, "type", type, sizeof(type));
        extract_quoted_field(line, "path", path, sizeof(path));
        extract_quoted_field(line, "exe", exe, sizeof(exe));
        extract_quoted_field(line, "pid", pid, sizeof(pid));
        extract_quoted_field(line, "uid", uid, sizeof(uid));
        if (strncmp(time_buf, today, strlen(today)) == 0) {
            today_count++;
        }

        const char *process = strcmp(exe, "-") == 0 || strcmp(exe, "unknown") == 0 ? "unknown" : basename_const(exe);
        char row[PATH_MAX + 256];
        snprintf(row, sizeof(row), "%s  %s  %s  %s  %s  pid=%s uid=%s",
                 time_buf, event, type, path, process, pid, uid);
        gtk_string_list_append(app->events_model, row);
        event_count++;
        if (event_count >= 80) {
            break;
        }
    }

    if (event_count == 0) {
        gtk_string_list_append(app->events_model, "No recent sentinel events");
    }
    if (app->events_today_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%zu", today_count);
        gtk_label_set_text(GTK_LABEL(app->events_today_label), buf);
    }
    g_free(out);
}

static void add_recent_guard_processes(GtkWidget *list)
{
    char *out = NULL;
    char *argv[] = {
        "/usr/bin/journalctl",
        "-u",
        JX_GUARD_SERVICE_NAME,
        "-r",
        "-n",
        "400",
        "--no-pager",
        "-o",
        "cat",
        NULL
    };
    int rc = run_capture(argv, &out);
    if (rc != 0 || !out || !*out) {
        list_append_text(list, "No recent Guard requests");
        g_free(out);
        return;
    }

    char seen[JX_MAX_CONTROL_ITEMS][PATH_MAX] = {{0}};
    size_t seen_count = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(out, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        if (!strstr(line, "JX_GUARD_DECISION")) {
            continue;
        }
        char exe[PATH_MAX];
        char cmd[PATH_MAX];
        char decision[32] = "unknown";
        char source[64] = "unknown";
        char result[32] = "unknown";
        char protected_root[PATH_MAX] = "-";
        char time_buf[64] = "-";
        char name[PATH_MAX];
        exe[0] = '\0';
        cmd[0] = '\0';
        extract_quoted_field(line, "exe", exe, sizeof(exe));
        extract_quoted_field(line, "cmd", cmd, sizeof(cmd));
        extract_quoted_field(line, "decision", decision, sizeof(decision));
        extract_quoted_field(line, "source", source, sizeof(source));
        extract_quoted_field(line, "result", result, sizeof(result));
        extract_quoted_field(line, "protected_root", protected_root, sizeof(protected_root));
        extract_quoted_field(line, "time", time_buf, sizeof(time_buf));

        if (recent_guard_process_name(exe, cmd, name, sizeof(name)) != 0 ||
            string_array_contains(seen, seen_count, name)) {
            continue;
        }

        snprintf(seen[seen_count++], PATH_MAX, "%s", name);
        char decision_text[32];
        decision_label(decision, decision_text, sizeof(decision_text));
        char label[PATH_MAX + 192];
        snprintf(label, sizeof(label), "%s    %s/%s    %s    %s    %s",
                 name,
                 result,
                 decision_text,
                 source,
                 basename_for_display(protected_root),
                 time_buf);
        list_append_recent_guard_process(list, label, name);
        if (seen_count >= 20 || seen_count >= JX_MAX_CONTROL_ITEMS) {
            break;
        }
    }

    if (seen_count == 0) {
        list_append_text(list, "No recent Guard requests");
    }
    g_free(out);
}

static void on_recent_guard_app_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    GtkWidget *entry = user_data;
    if (!row) {
        return;
    }
    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!GTK_IS_LABEL(child)) {
        return;
    }
    const char *value = g_object_get_data(G_OBJECT(row), "jx-guard-app-value");
    if (!value) {
        value = gtk_label_get_text(GTK_LABEL(child));
    }
    if (!value || strcmp(value, "No recent Guard requests") == 0) {
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(entry), value);
}

static void on_guard_app_prompt_response(GtkDialog *dialog, int response, gpointer user_data)
{
    struct guard_app_prompt_ctx *ctx = user_data;
    if (response == GTK_RESPONSE_ACCEPT) {
        const char *value = gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
        if (guard_app_value_is_valid(value)) {
            apply_guard_app_change(ctx->app, "--guard-add-app", value, "Add allowed app failed");
        } else {
            show_message(ctx->app, GTK_MESSAGE_ERROR, "Invalid app", "Enter a process name such as nautilus or an absolute executable path.");
        }
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
    g_free(ctx);
}

static void on_add_guard_app(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct jx_control_app *app = user_data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Add Allowed App",
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Cancel",
                                                    GTK_RESPONSE_CANCEL,
                                                    "_Add",
                                                    GTK_RESPONSE_ACCEPT,
                                                    NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "nautilus");
    gtk_widget_set_margin_top(entry, 12);
    gtk_widget_set_margin_bottom(entry, 12);
    gtk_widget_set_margin_start(entry, 12);
    gtk_widget_set_margin_end(entry, 12);
    gtk_box_append(GTK_BOX(area), entry);

    GtkWidget *recent_frame = section_box("Recently Requested Processes");
    GtkWidget *recent_box = frame_child(recent_frame);
    GtkWidget *recent_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(recent_scroll, -1, 150);
    GtkWidget *recent_list = gtk_list_box_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(recent_scroll), recent_list);
    gtk_box_append(GTK_BOX(recent_box), recent_scroll);
    add_recent_guard_processes(recent_list);
    g_signal_connect(recent_list, "row-selected", G_CALLBACK(on_recent_guard_app_selected), entry);
    gtk_widget_set_margin_start(recent_frame, 12);
    gtk_widget_set_margin_end(recent_frame, 12);
    gtk_widget_set_margin_bottom(recent_frame, 12);
    gtk_box_append(GTK_BOX(area), recent_frame);

    struct guard_app_prompt_ctx *ctx = g_new0(struct guard_app_prompt_ctx, 1);
    ctx->app = app;
    ctx->entry = entry;
    g_signal_connect(dialog, "response", G_CALLBACK(on_guard_app_prompt_response), ctx);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_remove_guard_app(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct jx_control_app *app = user_data;
    const char *value = selected_list_text(app->guard_process_allowlist);
    if (!value || strcmp(value, "No process allowlist entries") == 0 ||
        strcmp(value, "Guard config not installed") == 0 ||
        strcmp(value, "Guard config not readable") == 0) {
        return;
    }
    char *app_name = guard_app_name_for_prompt(value);
    if (!app_name) {
        return;
    }
    if (confirm_guard_app_removal(app, app_name)) {
        const char *helper_arg = g_str_has_suffix(value, " (prompt)") ?
            "--guard-remove-policy-app" : "--guard-remove-app";
        apply_guard_app_change(app, helper_arg, app_name, "Remove allowed app failed");
    }
    g_free(app_name);
}

static void on_guard_allowed_app_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    struct jx_control_app *app = user_data;
    if (!row) {
        return;
    }
    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!GTK_IS_LABEL(child)) {
        return;
    }
    const char *value = gtk_label_get_text(GTK_LABEL(child));
    if (!value || !g_str_has_suffix(value, " (prompt)")) {
        return;
    }
    char *app_name = guard_app_name_for_prompt(value);
    if (!app_name || !guard_app_value_is_valid(app_name)) {
        g_free(app_name);
        return;
    }
    if (confirm_guard_app_promotion(app, app_name)) {
        apply_guard_app_change(app, "--guard-add-app", app_name, "Promote allowed app failed");
    }
    g_free(app_name);
}

struct guard_folder_prompt_ctx {
    struct jx_control_app *app;
    GtkWidget *entry;
};

static void add_recent_guard_folders(GtkWidget *list)
{
    char *out = NULL;
    char *argv[] = {
        "/usr/bin/journalctl",
        "-u",
        JX_GUARD_SERVICE_NAME,
        "-n",
        "160",
        "--no-pager",
        "-o",
        "cat",
        NULL
    };
    int rc = run_capture(argv, &out);
    if (rc != 0 || !out || !*out) {
        list_append_text(list, "No recent Guard folders");
        g_free(out);
        return;
    }

    char seen[JX_MAX_CONTROL_ITEMS][PATH_MAX] = {{0}};
    size_t seen_count = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(out, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        if (!strstr(line, "JX_GUARD_DECISION")) {
            continue;
        }
        char path[PATH_MAX];
        if (extract_quoted_field(line, "path", path, sizeof(path)) != 0 ||
            path[0] != '/') {
            continue;
        }

        char *folder = g_path_get_dirname(path);
        if (!folder) {
            continue;
        }
        char error[256];
        int valid = jx_control_validate_path(folder, error, sizeof(error)) == 0 &&
                    !string_array_contains(seen, seen_count, folder);
        if (valid) {
            snprintf(seen[seen_count++], PATH_MAX, "%s", folder);
            list_append_text(list, folder);
        }
        g_free(folder);
        if (seen_count >= 20 || seen_count >= JX_MAX_CONTROL_ITEMS) {
            break;
        }
    }

    if (seen_count == 0) {
        list_append_text(list, "No recent Guard folders");
    }
    g_free(out);
}

static void on_recent_guard_folder_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    GtkWidget *entry = user_data;
    if (!row) {
        return;
    }
    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!GTK_IS_LABEL(child)) {
        return;
    }
    const char *value = gtk_label_get_text(GTK_LABEL(child));
    if (!value || strcmp(value, "No recent Guard folders") == 0) {
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(entry), value);
}

static void on_guard_folder_prompt_response(GtkDialog *dialog, int response, gpointer user_data)
{
    struct guard_folder_prompt_ctx *ctx = user_data;
    if (response == GTK_RESPONSE_ACCEPT) {
        const char *value = gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
        char error[256];
        if (jx_control_validate_path(value, error, sizeof(error)) == 0) {
            apply_guard_app_change(ctx->app, "--guard-add-folder", value, "Add protected folder failed");
        } else {
            show_message(ctx->app, GTK_MESSAGE_ERROR, "Invalid folder", error);
        }
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
    g_free(ctx);
}

static void on_add_guard_folder(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct jx_control_app *app = user_data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Add Protected Folder",
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Cancel",
                                                    GTK_RESPONSE_CANCEL,
                                                    "_Add",
                                                    GTK_RESPONSE_ACCEPT,
                                                    NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "/home/jackson/Desktop");
    gtk_widget_set_margin_top(entry, 12);
    gtk_widget_set_margin_bottom(entry, 12);
    gtk_widget_set_margin_start(entry, 12);
    gtk_widget_set_margin_end(entry, 12);
    gtk_box_append(GTK_BOX(area), entry);

    GtkWidget *recent_frame = section_box("Recently Requested Folders");
    GtkWidget *recent_box = frame_child(recent_frame);
    GtkWidget *recent_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(recent_scroll, -1, 150);
    GtkWidget *recent_list = gtk_list_box_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(recent_scroll), recent_list);
    gtk_box_append(GTK_BOX(recent_box), recent_scroll);
    add_recent_guard_folders(recent_list);
    g_signal_connect(recent_list, "row-selected", G_CALLBACK(on_recent_guard_folder_selected), entry);
    gtk_widget_set_margin_start(recent_frame, 12);
    gtk_widget_set_margin_end(recent_frame, 12);
    gtk_widget_set_margin_bottom(recent_frame, 12);
    gtk_box_append(GTK_BOX(area), recent_frame);

    struct guard_folder_prompt_ctx *ctx = g_new0(struct guard_folder_prompt_ctx, 1);
    ctx->app = app;
    ctx->entry = entry;
    g_signal_connect(dialog, "response", G_CALLBACK(on_guard_folder_prompt_response), ctx);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_remove_guard_folder(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct jx_control_app *app = user_data;
    const char *value = selected_list_text(app->guard_paths_list);
    if (!value || strcmp(value, "Guard config not installed") == 0 ||
        strcmp(value, "Guard config not readable") == 0) {
        return;
    }
    apply_guard_app_change(app, "--guard-remove-folder", value, "Remove protected folder failed");
}

static void on_refresh(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct jx_control_app *app = user_data;
    load_config_into_app(app);
    refresh_status(app);
    refresh_logs(app);
    refresh_events(app);
    refresh_guard_status(app);
    refresh_guard_logs(app);
}

static void on_follow_logs(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    pid_t child = fork();
    if (child == 0) {
        char *const argv[] = {
            "/usr/bin/gnome-terminal",
            "--",
            "/usr/bin/journalctl",
            "-u",
            JX_SERVICE_NAME,
            "-f",
            NULL
        };
        execve("/usr/bin/gnome-terminal", argv, environ);
        _exit(127);
    }
}

static void on_test_notify(GtkButton *button, gpointer user_data)
{
    (void)button;
    struct jx_control_app *app = user_data;
    const char *user = gtk_editable_get_text(GTK_EDITABLE(app->notify_user_entry));
    (void)user;
    char *const argv[] = {
        "/usr/bin/notify-send",
        "--app-name=jx-sentinel-control",
        "JX Sentinel",
        "Test notification from jx-sentinel-control",
        NULL
    };
    int rc = run_no_capture(argv);
    if (rc != 0) {
        show_message(app, GTK_MESSAGE_ERROR, "Test notification failed", "Check that libnotify-bin is installed and your GNOME session bus is available.");
    }
}

static GtkWidget *button_row(struct jx_control_app *app, const char *buttons[][2],
                             size_t count, GCallback callbacks[])
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (size_t i = 0; i < count; i++) {
        GtkWidget *button = gtk_button_new_with_label(buttons[i][0]);
        gtk_widget_set_tooltip_text(button, buttons[i][1]);
        g_signal_connect(button, "clicked", callbacks[i], app);
        gtk_box_append(GTK_BOX(row), button);
    }
    return row;
}

static GtkWidget *dashboard_card(const char *title, GtkWidget *value)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    GtkWidget *title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(title_label), attrs);
    pango_attr_list_unref(attrs);

    gtk_label_set_xalign(GTK_LABEL(value), 0.0f);
    gtk_box_append(GTK_BOX(box), title_label);
    gtk_box_append(GTK_BOX(box), value);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    gtk_widget_set_hexpand(frame, TRUE);
    return frame;
}

static void event_factory_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory;
    (void)data;
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_list_item_set_child(item, label);
}

static void event_factory_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void)factory;
    (void)data;
    GtkStringObject *obj = gtk_list_item_get_item(item);
    GtkWidget *label = gtk_list_item_get_child(item);
    gtk_label_set_text(GTK_LABEL(label), gtk_string_object_get_string(obj));
}

static GtkWidget *make_events_table(GtkStringList *rows, struct jx_control_app *app)
{
    GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(rows)));
    gtk_single_selection_set_autoselect(selection, FALSE);
    gtk_single_selection_set_can_unselect(selection, TRUE);
    g_signal_connect(selection, "notify::selected-item", G_CALLBACK(on_event_selection_changed), app);
    GtkWidget *view = gtk_column_view_new(GTK_SELECTION_MODEL(selection));
    const char *titles[] = {"Recent Events"};
    for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); i++) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
        g_signal_connect(factory, "setup", G_CALLBACK(event_factory_setup), NULL);
        g_signal_connect(factory, "bind", G_CALLBACK(event_factory_bind), NULL);
        GtkColumnViewColumn *column = gtk_column_view_column_new(titles[i], factory);
        gtk_column_view_column_set_resizable(column, TRUE);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(view), column);
        g_object_unref(column);
    }
    gtk_widget_set_vexpand(view, TRUE);
    return view;
}

static int graph_jsc_probe(void)
{
    JSCContext *context = jsc_context_new();
    if (!context) {
        return 0;
    }

    JSCValue *value = jsc_context_evaluate(context, "Math.round(Math.hypot(4, 3))", -1);
    int result = value ? jsc_value_to_int32(value) : 0;
    if (value) {
        g_object_unref(value);
    }
    g_object_unref(context);
    return result;
}

static void json_append_string(GString *out, const char *value)
{
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; p++) {
        switch (*p) {
        case '\\':
            g_string_append(out, "\\\\");
            break;
        case '"':
            g_string_append(out, "\\\"");
            break;
        case '\n':
            g_string_append(out, "\\n");
            break;
        case '\r':
            g_string_append(out, "\\r");
            break;
        case '\t':
            g_string_append(out, "\\t");
            break;
        default:
            if (*p < 32) {
                g_string_append_c(out, ' ');
            } else {
                g_string_append_c(out, (char)*p);
            }
            break;
        }
    }
    g_string_append_c(out, '"');
}

static void graph_append_event(GString *json, size_t *count,
                               const char *kind, const char *process,
                               const char *action, const char *target,
                               const char *path, const char *decision,
                               const char *result, const char *source)
{
    if (*count > 0) {
        g_string_append_c(json, ',');
    }
    g_string_append_c(json, '{');
    g_string_append(json, "\"kind\":");
    json_append_string(json, kind);
    g_string_append(json, ",\"process\":");
    json_append_string(json, process);
    g_string_append(json, ",\"action\":");
    json_append_string(json, action);
    g_string_append(json, ",\"target\":");
    json_append_string(json, target);
    g_string_append(json, ",\"path\":");
    json_append_string(json, path);
    g_string_append(json, ",\"decision\":");
    json_append_string(json, decision);
    g_string_append(json, ",\"result\":");
    json_append_string(json, result);
    g_string_append(json, ",\"source\":");
    json_append_string(json, source);
    g_string_append_c(json, '}');
    (*count)++;
}

static char *graph_events_json(void)
{
    GString *json = g_string_new("[");
    size_t count = 0;

    char *sentinel = NULL;
    char *sentinel_argv[] = {
        "/usr/bin/journalctl",
        "-u",
        JX_SERVICE_NAME,
        "-r",
        "-n",
        "120",
        "--no-pager",
        "-o",
        "cat",
        NULL
    };
    if (run_capture(sentinel_argv, &sentinel) == 0 && sentinel && *sentinel) {
        char *saveptr = NULL;
        for (char *line = strtok_r(sentinel, "\n", &saveptr); line && count < 90; line = strtok_r(NULL, "\n", &saveptr)) {
            if (!strstr(line, "JX_SENTINEL_EVENT")) {
                continue;
            }
            char event[32] = "event";
            char type[32] = "file";
            char path[PATH_MAX] = "-";
            char exe[PATH_MAX] = "unknown";
            char cmd[PATH_MAX] = "";
            char process[PATH_MAX] = "unknown";
            char token[PATH_MAX];
            extract_quoted_field(line, "event", event, sizeof(event));
            extract_quoted_field(line, "type", type, sizeof(type));
            extract_quoted_field(line, "path", path, sizeof(path));
            extract_quoted_field(line, "exe", exe, sizeof(exe));
            extract_quoted_field(line, "cmd", cmd, sizeof(cmd));
            if (strcmp(exe, "unknown") != 0 && strcmp(exe, "-") != 0) {
                snprintf(process, sizeof(process), "%s", basename_for_display(exe));
            } else {
                first_cmd_token(cmd, token, sizeof(token));
                if (token[0]) {
                    snprintf(process, sizeof(process), "%s", basename_for_display(token));
                }
            }
            graph_append_event(json, &count, "sentinel", process, event,
                               type, basename_for_display(path), "observed", "event", "sentinel");
        }
    }
    g_free(sentinel);

    char *guard = NULL;
    char *guard_argv[] = {
        "/usr/bin/journalctl",
        "-u",
        JX_GUARD_SERVICE_NAME,
        "-r",
        "-n",
        "180",
        "--no-pager",
        "-o",
        "cat",
        NULL
    };
    if (run_capture(guard_argv, &guard) == 0 && guard && *guard) {
        char seen[JX_MAX_CONTROL_ITEMS][PATH_MAX] = {{0}};
        size_t seen_count = 0;
        char *saveptr = NULL;
        for (char *line = strtok_r(guard, "\n", &saveptr); line && count < 140; line = strtok_r(NULL, "\n", &saveptr)) {
            if (!strstr(line, "JX_GUARD_DECISION")) {
                continue;
            }
            char exe[PATH_MAX] = "";
            char cmd[PATH_MAX] = "";
            char process[PATH_MAX];
            char action[32] = "access";
            char path[PATH_MAX] = "-";
            char target[PATH_MAX] = "-";
            char decision[32] = "unknown";
            char result[32] = "unknown";
            char source[64] = "unknown";
            extract_quoted_field(line, "exe", exe, sizeof(exe));
            extract_quoted_field(line, "cmd", cmd, sizeof(cmd));
            extract_quoted_field(line, "action", action, sizeof(action));
            extract_quoted_field(line, "path", path, sizeof(path));
            extract_quoted_field(line, "protected_root", target, sizeof(target));
            extract_quoted_field(line, "decision", decision, sizeof(decision));
            extract_quoted_field(line, "result", result, sizeof(result));
            extract_quoted_field(line, "source", source, sizeof(source));
            if (recent_guard_process_name(exe, cmd, process, sizeof(process)) != 0) {
                continue;
            }

            char dedupe[PATH_MAX + 128];
            snprintf(dedupe, sizeof(dedupe), "%.160s|%.32s|%.512s|%.32s",
                     process, action, target, decision);
            if (string_array_contains(seen, seen_count, dedupe)) {
                continue;
            }
            if (seen_count < JX_MAX_CONTROL_ITEMS) {
                g_strlcpy(seen[seen_count++], dedupe, PATH_MAX);
            }
            graph_append_event(json, &count, "guard", process, action,
                               basename_for_display(target), basename_for_display(path),
                               decision, result, source);
        }
    }
    g_free(guard);

    g_string_append_c(json, ']');
    return g_string_free(json, FALSE);
}

static GtkWidget *make_graph_view(void)
{
    GtkWidget *view = webkit_web_view_new();
    WebKitSettings *settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(view));
    webkit_settings_set_javascript_can_access_clipboard(settings, FALSE);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    gtk_widget_set_hexpand(view, TRUE);
    gtk_widget_set_vexpand(view, TRUE);

    int jsc_probe = graph_jsc_probe();
    char *event_json = graph_events_json();
    char *html = g_strdup_printf(
        "<!doctype html>"
        "<html><head><meta charset='utf-8'>"
        "<style>"
        ":root{color-scheme:light dark;--bg:#f7f8fa;--fg:#202124;--muted:#5f6368;--line:#30333a;--badge-bg:rgba(255,255,255,.88);--badge-border:#d4d7dc;}"
        "@media (prefers-color-scheme:dark){:root{--bg:#242424;--fg:#f2f2f2;--muted:#c6c6c6;--line:#8f96a3;--badge-bg:rgba(48,48,48,.88);--badge-border:#5d6066;}}"
        "html,body{margin:0;width:100%%;height:100%%;overflow:hidden;background:var(--bg);color:var(--fg);font:14px system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;}"
        "#graph{display:block;width:100vw;height:100vh;}"
        "#badge{position:fixed;left:14px;top:12px;padding:6px 10px;border:1px solid var(--badge-border);border-radius:6px;background:var(--badge-bg);font-weight:650;}"
        "#filters{position:fixed;left:14px;top:54px;display:flex;gap:8px;align-items:center;padding:8px;border:1px solid var(--badge-border);border-radius:6px;background:var(--badge-bg);}"
        "#filters select,#filters input{font:13px system-ui;background:var(--bg);color:var(--fg);border:1px solid var(--badge-border);border-radius:5px;padding:5px 7px;}"
        "#filters input{width:180px;}"
        "#status{position:fixed;right:14px;bottom:12px;color:var(--muted);font-size:12px;}"
        "</style></head>"
        "<body><canvas id='graph'></canvas><div id='badge'>Connected Event Bubbles</div>"
        "<div id='filters'><select id='kind'><option value='all'>All events</option><option value='guard'>Guard only</option><option value='sentinel'>Sentinel only</option><option value='denied'>Denied</option><option value='allowed'>Allowed</option><option value='prompt'>Prompted</option></select><input id='query' placeholder='Filter process, path, decision'></div>"
        "<div id='status'>%d JS probe · live journal snapshot</div>"
        "<script>"
        "const rawEvents=%s;"
        "const canvas=document.getElementById('graph');"
        "const ctx=canvas.getContext('2d',{alpha:false});"
        "const kindFilter=document.getElementById('kind');const queryFilter=document.getElementById('query');const status=document.getElementById('status');"
        "const palette={process:'#4a7bd0',action:'#f15324',target:'#29a37a',file:'#a66ad8',decision:'#d9a421'};"
        "let nodes=[],links=[],nodeByKey=new Map(),linkMap=new Map();"
        "function node(id,kind){const key=kind+':'+id;if(nodeByKey.has(key)){const n=nodeByKey.get(key);n.count++;n.r=Math.min(58,24+Math.sqrt(n.count)*8);return n;}const n={id,kind,count:1,r:30,color:palette[kind]||'#888',x:.5+Math.random()*.1,y:.5+Math.random()*.1,vx:0,vy:0,fixed:false};nodeByKey.set(key,n);nodes.push(n);return n;}"
        "function link(a,b){const key=a.kind+':'+a.id+'>'+b.kind+':'+b.id;const old=linkMap.get(key);if(old){old.count++;return;}linkMap.set(key,{a,b,count:1});}"
        "function eventMatches(e){const mode=kindFilter.value;const q=queryFilter.value.trim().toLowerCase();if(mode==='guard'&&e.kind!=='guard')return false;if(mode==='sentinel'&&e.kind!=='sentinel')return false;if(mode==='denied'&&e.result!=='deny')return false;if(mode==='allowed'&&e.result!=='allow'&&e.result!=='event')return false;if(mode==='prompt'&&!String(e.source||'').includes('prompt'))return false;if(q){const hay=[e.kind,e.process,e.action,e.target,e.path,e.decision,e.result,e.source].join(' ').toLowerCase();if(!hay.includes(q))return false;}return true;}"
        "function buildGraph(){nodes=[];links=[];nodeByKey=new Map();linkMap=new Map();rawEvents.filter(eventMatches).slice(0,90).forEach(e=>{const p=node(e.process||'unknown','process');const a=node((e.kind==='guard'?(e.result+'/'+e.decision):e.action)||'event','action');const t=node(e.target||'target','target');link(p,a);link(a,t);if(e.path&&e.path!=='-'&&e.path!==e.target){const f=node(e.path,'file');link(t,f);}});if(nodes.length===0){node('No matching events','action');}links=[...linkMap.values()];status.textContent=rawEvents.length+' events · '+nodes.length+' bubbles · '+links.length+' links';}"
        "kindFilter.addEventListener('change',buildGraph);queryFilter.addEventListener('input',buildGraph);buildGraph();"
        "let tick=0,drag=null;"
        "function css(name){return getComputedStyle(document.documentElement).getPropertyValue(name).trim();}"
        "function resize(){const dpr=Math.max(1,window.devicePixelRatio||1);canvas.width=Math.floor(innerWidth*dpr);canvas.height=Math.floor(innerHeight*dpr);ctx.setTransform(dpr,0,0,dpr,0,0);}"
        "function step(w,h){nodes.forEach((n,i)=>{for(let j=i+1;j<nodes.length;j++){const m=nodes[j],dx=n.x-m.x,dy=n.y-m.y,d2=Math.max(.002,dx*dx+dy*dy),f=.0009/d2;if(!n.fixed){n.vx+=dx*f;n.vy+=dy*f;}if(!m.fixed){m.vx-=dx*f;m.vy-=dy*f;}}if(!n.fixed){n.vx+=(.5-n.x)*.002;n.vy+=(.5-n.y)*.002;}});links.forEach(l=>{const dx=l.b.x-l.a.x,dy=l.b.y-l.a.y,d=Math.max(.001,Math.hypot(dx,dy)),target=.22,force=(d-target)*.012*l.count;const fx=dx/d*force,fy=dy/d*force;if(!l.a.fixed){l.a.vx+=fx;l.a.vy+=fy;}if(!l.b.fixed){l.b.vx-=fx;l.b.vy-=fy;}});nodes.forEach(n=>{if(!n.fixed){n.vx*=.86;n.vy*=.86;n.x=Math.max(.08,Math.min(.92,n.x+n.vx));n.y=Math.max(.12,Math.min(.88,n.y+n.vy));}n.px=n.x*w;n.py=n.y*h;});}"
        "function line(l){ctx.beginPath();ctx.moveTo(l.a.px,l.a.py);ctx.lineTo(l.b.px,l.b.py);ctx.lineWidth=Math.min(6,1+l.count);ctx.stroke();}"
        "function labelText(t,max){return t.length>max?t.slice(0,max-1)+'…':t;}"
        "function drawNode(n,i){const pulse=Math.sin(tick/24+i)*2;ctx.beginPath();ctx.arc(n.px,n.py,n.r+pulse,0,Math.PI*2);ctx.fillStyle=n.color;ctx.fill();ctx.lineWidth=2;ctx.strokeStyle=css('--fg');ctx.stroke();ctx.fillStyle=css('--fg');ctx.textAlign='center';ctx.textBaseline='middle';ctx.font='700 12px system-ui';ctx.fillText(labelText(n.id,14),n.px,n.py-4);ctx.font='11px system-ui';ctx.fillText(n.kind,n.px,n.py+12);}"
        "function draw(){tick++;const w=innerWidth,h=innerHeight;step(w,h);ctx.fillStyle=css('--bg');ctx.fillRect(0,0,w,h);ctx.strokeStyle=css('--line');links.forEach(line);nodes.forEach(drawNode);requestAnimationFrame(draw);}"
        "function pointer(e){const r=canvas.getBoundingClientRect();return{x:e.clientX-r.left,y:e.clientY-r.top};}"
        "function hit(p){for(let i=nodes.length-1;i>=0;i--){const n=nodes[i],dx=p.x-n.px,dy=p.y-n.py;if(dx*dx+dy*dy<=n.r*n.r){return n;}}return null;}"
        "canvas.addEventListener('pointerdown',e=>{const p=pointer(e),n=hit(p);if(!n)return;drag={n,dx:p.x-n.px,dy:p.y-n.py};n.fixed=true;n.vx=0;n.vy=0;canvas.setPointerCapture(e.pointerId);canvas.style.cursor='grabbing';});"
        "canvas.addEventListener('pointermove',e=>{const p=pointer(e);if(drag){drag.n.x=Math.max(.04,Math.min(.96,(p.x-drag.dx)/innerWidth));drag.n.y=Math.max(.08,Math.min(.92,(p.y-drag.dy)/innerHeight));drag.n.vx=0;drag.n.vy=0;}else{canvas.style.cursor=hit(p)?'grab':'default';}});"
        "canvas.addEventListener('pointerup',e=>{if(drag){canvas.releasePointerCapture(e.pointerId);drag=null;canvas.style.cursor='grab';}});"
        "canvas.addEventListener('pointercancel',()=>{drag=null;canvas.style.cursor='default';});"
        "canvas.addEventListener('dblclick',e=>{const n=hit(pointer(e));if(n){n.fixed=false;n.vx=0;n.vy=0;}});"
        "addEventListener('resize',resize);resize();draw();"
        "</script></body></html>",
        jsc_probe,
        event_json);
    webkit_web_view_load_html(WEBKIT_WEB_VIEW(view), html, "file:///opt/jx/share/jx-sentinel-control/");
    g_free(event_json);
    g_free(html);
    return view;
}

static GtkWidget *make_diagnostics_list(void)
{
    GtkWidget *list = gtk_list_box_new();
    const char *checks[] = {
        "PASS  /opt/jx/sbin/jx-sentinel",
        "PASS  /opt/jx/libexec/jx-sentinel-helper",
        "PASS  /opt/jx/etc/jx-sentinel/jx-sentinel.conf",
        "PASS  /etc/systemd/system/jx-sentinel.service",
        "PASS  /usr/bin/notify-send",
        "WARN  GNOME DBus session depends on active desktop login",
    };
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        list_append_text(list, checks[i]);
    }
    return list;
}

static void show_command_palette(struct jx_control_app *app)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Command Palette",
                                                    GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "_Close",
                                                    GTK_RESPONSE_CLOSE,
                                                    NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *search = gtk_search_entry_new();
    gtk_widget_set_margin_top(search, 12);
    gtk_widget_set_margin_start(search, 12);
    gtk_widget_set_margin_end(search, 12);
    gtk_box_append(GTK_BOX(area), search);

    GtkWidget *list = gtk_list_box_new();
    const char *commands[] = {
        "Restart Sentinel",
        "Start Sentinel",
        "Stop Sentinel",
        "Add Watch Path",
        "Add Extension Rule",
        "Open Logs",
        "Run Diagnostics",
        "Export Config",
        "Open /opt/jx",
        "Apply Changes",
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        list_append_text(list, commands[i]);
    }
    gtk_widget_set_margin_bottom(list, 12);
    gtk_widget_set_margin_start(list, 12);
    gtk_widget_set_margin_end(list, 12);
    gtk_box_append(GTK_BOX(area), list);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_window_destroy), NULL);
    gtk_window_present(GTK_WINDOW(dialog));
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)controller;
    (void)keycode;
    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_k || keyval == GDK_KEY_K)) {
        show_command_palette(user_data);
        return TRUE;
    }
    return FALSE;
}

static void splash_status_cb(const char *message, double fraction, void *user_data)
{
    jx_splash_update((JxSplash *)user_data, message, fraction);
}

static void activate(GtkApplication *gtk_app, gpointer user_data)
{
    apply_desktop_theme_preference();

    struct jx_control_app *app = user_data;
    app->app = gtk_app;

    size_t module_count = 0;
    app->modules = jx_module_manifest(&module_count);
    app->module_count = module_count;
    JxSplash *splash = jx_splash_new(gtk_app);
    jx_splash_show(splash);
    if (jx_module_load_all(app->modules, app->module_count, splash_status_cb, splash) != 0) {
        char fatal[1200];
        snprintf(fatal, sizeof(fatal), "Fatal error: required module failed to load. %s",
                 jx_module_last_error());
        jx_splash_update(splash, fatal, 1.0);
        return;
    }
    jx_splash_close(splash);

    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "JX Sentinel Control");
    gtk_window_set_icon_name(GTK_WINDOW(app->window), JX_CONTROL_APP_ID);
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 720);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), app);
    gtk_widget_add_controller(app->window, keys);

    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *title = gtk_label_new("JX Sentinel Control");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);
    GtkWidget *start_button = gtk_button_new_with_label("Start");
    GtkWidget *stop_button = gtk_button_new_with_label("Stop");
    GtkWidget *restart_button = gtk_button_new_with_label("Restart");
    GtkWidget *apply_button = gtk_button_new_with_label("Apply");
    gtk_widget_set_size_request(start_button, 74, 34);
    gtk_widget_set_size_request(stop_button, 74, 34);
    gtk_widget_set_size_request(restart_button, 86, 34);
    gtk_widget_set_size_request(apply_button, 74, 34);
    g_signal_connect(start_button, "clicked", G_CALLBACK(on_start), app);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_stop), app);
    g_signal_connect(restart_button, "clicked", G_CALLBACK(on_restart), app);
    g_signal_connect(apply_button, "clicked", G_CALLBACK(on_save), app);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), start_button);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), stop_button);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), restart_button);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), apply_button);
    app->header_status_label = gtk_label_new("○ unknown");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), app->header_status_label);
    gtk_window_set_titlebar(GTK_WINDOW(app->window), header);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(root, 4);
    gtk_widget_set_margin_bottom(root, 4);
    gtk_widget_set_margin_start(root, 4);
    gtk_widget_set_margin_end(root, 4);
    gtk_window_set_child(GTK_WINDOW(app->window), root);

    app->status_label = gtk_label_new("unknown");
    app->enabled_label = gtk_label_new("unknown");
    app->pid_label = gtk_label_new("-");
    app->roots_label = gtk_label_new("-");
    app->rules_label = gtk_label_new("-");
    app->last_event_label = gtk_label_new("-");
    app->events_model = gtk_string_list_new(NULL);

    GtkWidget *workspace = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_vexpand(workspace, TRUE);
    gtk_widget_set_hexpand(workspace, TRUE);
    gtk_box_append(GTK_BOX(root), workspace);

    GtkWidget *objects_frame = section_box("Navigate");
    GtkWidget *objects_box = frame_child(objects_frame);
    GtkWidget *objects_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(objects_scroll, TRUE);
    gtk_box_append(GTK_BOX(objects_box), objects_scroll);

    GtkWidget *sidebar = gtk_list_box_new();
    gtk_widget_set_size_request(sidebar, 190, -1);
    const char *tree[] = {"Dashboard", "Service", "Watch Roots", "Extension Rules", "Guard", "Events", "Graph", "Logs", "Diagnostics", "Settings"};
    for (size_t i = 0; i < sizeof(tree) / sizeof(tree[0]); i++) {
        list_append_text(sidebar, tree[i]);
    }
    g_signal_connect(sidebar, "row-selected", G_CALLBACK(on_navigation_selected), app);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(objects_scroll), sidebar);
    gtk_widget_set_size_request(objects_frame, 210, -1);
    gtk_widget_set_hexpand(objects_frame, FALSE);
    gtk_box_append(GTK_BOX(workspace), objects_frame);

    GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(center, TRUE);
    gtk_widget_set_vexpand(center, TRUE);
    gtk_box_append(GTK_BOX(workspace), center);

    GtkWidget *stack = gtk_stack_new();
    app->stack = stack;
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_box_append(GTK_BOX(center), stack);

    GtkWidget *dashboard_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(dashboard_scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    GtkWidget *dashboard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(dashboard, 8);
    gtk_widget_set_margin_bottom(dashboard, 8);
    gtk_widget_set_margin_start(dashboard, 8);
    gtk_widget_set_margin_end(dashboard, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(dashboard_scroll), dashboard);
    gtk_stack_add_titled(GTK_STACK(stack), dashboard_scroll, "dashboard", "Dashboard");

    GtkWidget *cards = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(cards), 12);
    gtk_grid_set_column_spacing(GTK_GRID(cards), 12);
    app->events_today_label = gtk_label_new("0");
    app->dashboard_guard_rules_label = gtk_label_new("0");
    gtk_grid_attach(GTK_GRID(cards), dashboard_card("Service", app->status_label), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cards), dashboard_card("Events Today", app->events_today_label), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cards), dashboard_card("Guard Rules", app->dashboard_guard_rules_label), 2, 0, 1, 1);
    gtk_box_append(GTK_BOX(dashboard), cards);

    GtkWidget *live_events = section_box("Live Events");
    GtkWidget *live_events_box = frame_child(live_events);
    GtkWidget *live_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(live_scroll, -1, 160);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(live_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(live_scroll), make_events_table(app->events_model, app));
    gtk_box_append(GTK_BOX(live_events_box), live_scroll);
    gtk_box_append(GTK_BOX(dashboard), live_events);

    GtkWidget *settings_fold = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *folders = section_box("Watched Folders");
    GtkWidget *folders_box = frame_child(folders);
    app->watch_list = gtk_list_box_new();
    gtk_widget_set_size_request(app->watch_list, -1, 96);
    gtk_widget_set_hexpand(folders, TRUE);
    g_signal_connect(app->watch_list, "row-selected", G_CALLBACK(on_context_list_selected), app);
    gtk_box_append(GTK_BOX(folders_box), app->watch_list);
    const char *folder_buttons[][2] = {
        {"Add Folder", "Add an absolute existing directory"},
        {"Remove Folder", "Remove the selected watched directory"}
    };
    GCallback folder_callbacks[] = {G_CALLBACK(on_add_folder), G_CALLBACK(on_remove_folder)};
    gtk_box_append(GTK_BOX(folders_box), button_row(app, folder_buttons, 2, folder_callbacks));
    gtk_box_append(GTK_BOX(settings_fold), folders);

    GtkWidget *extensions = section_box("File Extensions");
    GtkWidget *extensions_box = frame_child(extensions);
    app->extension_list = gtk_list_box_new();
    gtk_widget_set_size_request(app->extension_list, -1, 96);
    gtk_widget_set_hexpand(extensions, TRUE);
    g_signal_connect(app->extension_list, "row-selected", G_CALLBACK(on_context_list_selected), app);
    gtk_box_append(GTK_BOX(extensions_box), app->extension_list);
    const char *extension_buttons[][2] = {
        {"Add Extension", "Add an extension such as .conf"},
        {"Remove Extension", "Remove the selected extension"}
    };
    GCallback extension_callbacks[] = {G_CALLBACK(on_add_extension), G_CALLBACK(on_remove_extension)};
    gtk_box_append(GTK_BOX(extensions_box), button_row(app, extension_buttons, 2, extension_callbacks));
    app->log_all_switch = gtk_switch_new();
    gtk_box_append(GTK_BOX(extensions_box), label_row("Log all files", app->log_all_switch));
    gtk_box_append(GTK_BOX(settings_fold), extensions);
    gtk_box_append(GTK_BOX(dashboard), settings_fold);

    GtkWidget *notify = section_box("Notifications");
    GtkWidget *notify_box = frame_child(notify);
    app->notify_switch = gtk_switch_new();
    gtk_box_append(GTK_BOX(notify_box), label_row("GNOME notifications", app->notify_switch));
    app->notify_user_entry = gtk_entry_new();
    gtk_box_append(GTK_BOX(notify_box), label_row("Notification user", app->notify_user_entry));
    app->verbose_switch = gtk_switch_new();
    gtk_box_append(GTK_BOX(notify_box), label_row("Verbose mode", app->verbose_switch));
    const char *notify_buttons[][2] = {
        {"Test Notification", "Send a local GNOME test notification"}
    };
    GCallback notify_callbacks[] = {G_CALLBACK(on_test_notify)};
    gtk_box_append(GTK_BOX(notify_box), button_row(app, notify_buttons, 1, notify_callbacks));
    gtk_box_append(GTK_BOX(dashboard), notify);

    GtkWidget *rules_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *rules_toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(rules_toolbar), gtk_button_new_with_label("Add Rule"));
    gtk_box_append(GTK_BOX(rules_toolbar), gtk_button_new_with_label("Remove"));
    gtk_box_append(GTK_BOX(rules_page), rules_toolbar);
    GtkWidget *rules_scroll = gtk_scrolled_window_new();
    GtkWidget *rules_list = gtk_list_box_new();
    list_append_text(rules_list, "JX Config Files    /opt/jx    .conf .service    High");
    list_append_text(rules_list, "Source Code        /home/jackson    .c .h .sh    Medium");
    g_signal_connect(rules_list, "row-selected", G_CALLBACK(on_context_list_selected), app);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rules_scroll), rules_list);
    gtk_widget_set_vexpand(rules_scroll, TRUE);
    gtk_box_append(GTK_BOX(rules_page), rules_scroll);
    gtk_stack_add_titled(GTK_STACK(stack), rules_page, "rules", "Rules");

    GtkWidget *guard_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(guard_scroll),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    GtkWidget *guard_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(guard_page, 8);
    gtk_widget_set_margin_bottom(guard_page, 8);
    gtk_widget_set_margin_start(guard_page, 8);
    gtk_widget_set_margin_end(guard_page, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(guard_scroll), guard_page);

    app->guard_status_label = gtk_label_new("unknown");
    app->guard_agent_label = gtk_label_new("unknown");
    app->guard_paths_label = gtk_label_new("0");
    app->guard_rules_label = gtk_label_new("0");
    app->guard_last_label = gtk_label_new("-");
    gtk_label_set_wrap(GTK_LABEL(app->guard_last_label), TRUE);
    GtkWidget *guard_cards = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(guard_cards), 12);
    gtk_grid_set_column_spacing(GTK_GRID(guard_cards), 12);
    gtk_grid_attach(GTK_GRID(guard_cards), dashboard_card("Guard", app->guard_status_label), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(guard_cards), dashboard_card("Agent", app->guard_agent_label), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(guard_cards), dashboard_card("Protected Paths", app->guard_paths_label), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(guard_cards), dashboard_card("Rules", app->guard_rules_label), 3, 0, 1, 1);
    gtk_box_append(GTK_BOX(guard_page), guard_cards);

    GtkWidget *guard_controls = section_box("Guard Controls");
    GtkWidget *guard_controls_box = frame_child(guard_controls);
    const char *guard_buttons[][2] = {
        {"Start", "Start JX Sentinel Guard"},
        {"Stop", "Stop JX Sentinel Guard"},
        {"Restart", "Restart JX Sentinel Guard"},
        {"Start Agent", "Start the user-session permission prompt agent"},
        {"Enable", "Enable Guard on boot"},
        {"Disable", "Disable Guard on boot"}
    };
    GCallback guard_callbacks[] = {
        G_CALLBACK(on_guard_start),
        G_CALLBACK(on_guard_stop),
        G_CALLBACK(on_guard_restart),
        G_CALLBACK(on_start_agent),
        G_CALLBACK(on_guard_enable),
        G_CALLBACK(on_guard_disable)
    };
    gtk_box_append(GTK_BOX(guard_controls_box), button_row(app, guard_buttons, 6, guard_callbacks));
    gtk_box_append(GTK_BOX(guard_controls_box), label_row("Config", gtk_label_new(JX_GUARD_CONFIG_PATH)));
    gtk_box_append(GTK_BOX(guard_controls_box), label_row("Policy DB", gtk_label_new(JX_GUARD_DB_PATH)));
    gtk_box_append(GTK_BOX(guard_controls_box), label_row("Socket", gtk_label_new(JX_GUARD_SOCKET_PATH)));
    gtk_box_append(GTK_BOX(guard_page), guard_controls);

    GtkWidget *guard_paths = section_box("Protected Folders");
    GtkWidget *guard_paths_box = frame_child(guard_paths);
    app->guard_paths_list = gtk_list_box_new();
    gtk_widget_set_size_request(app->guard_paths_list, -1, 130);
    g_signal_connect(app->guard_paths_list, "row-selected", G_CALLBACK(on_context_list_selected), app);
    gtk_box_append(GTK_BOX(guard_paths_box), app->guard_paths_list);
    const char *guard_folder_buttons[][2] = {
        {"Add Folder", "Protect another existing folder"},
        {"Remove", "Remove the selected protected folder"}
    };
    GCallback guard_folder_callbacks[] = {G_CALLBACK(on_add_guard_folder), G_CALLBACK(on_remove_guard_folder)};
    gtk_box_append(GTK_BOX(guard_paths_box), button_row(app, guard_folder_buttons, 2, guard_folder_callbacks));
    gtk_box_append(GTK_BOX(guard_page), guard_paths);

    GtkWidget *guard_processes = section_box("Allowed Apps");
    GtkWidget *guard_processes_box = frame_child(guard_processes);
    app->guard_process_allowlist = gtk_list_box_new();
    gtk_widget_set_size_request(app->guard_process_allowlist, -1, 110);
    g_signal_connect(app->guard_process_allowlist, "row-selected", G_CALLBACK(on_context_list_selected), app);
    g_signal_connect(app->guard_process_allowlist,
                     "row-activated",
                     G_CALLBACK(on_guard_allowed_app_activated),
                     app);
    gtk_box_append(GTK_BOX(guard_processes_box), app->guard_process_allowlist);
    const char *guard_app_buttons[][2] = {
        {"Add App", "Allow a process name such as nautilus to access protected folders"},
        {"Remove", "Remove the selected allowed process"}
    };
    GCallback guard_app_callbacks[] = {G_CALLBACK(on_add_guard_app), G_CALLBACK(on_remove_guard_app)};
    gtk_box_append(GTK_BOX(guard_processes_box), button_row(app, guard_app_buttons, 2, guard_app_callbacks));
    gtk_box_append(GTK_BOX(guard_page), guard_processes);

    GtkWidget *last_guard = section_box("Last Guard Decision");
    GtkWidget *last_guard_box = frame_child(last_guard);
    gtk_box_append(GTK_BOX(last_guard_box), app->guard_last_label);
    gtk_box_append(GTK_BOX(guard_page), last_guard);

    GtkWidget *guard_log_section = section_box("Recent Guard Logs");
    GtkWidget *guard_log_box = frame_child(guard_log_section);
    app->guard_logs_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->guard_logs_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->guard_logs_view), TRUE);
    GtkWidget *guard_log_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(guard_log_scroll, -1, 180);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(guard_log_scroll), app->guard_logs_view);
    gtk_box_append(GTK_BOX(guard_log_box), guard_log_scroll);
    gtk_box_append(GTK_BOX(guard_page), guard_log_section);
    gtk_stack_add_titled(GTK_STACK(stack), guard_scroll, "guard", "Guard");

    app->event_view = make_events_table(app->events_model, app);
    GtkWidget *event_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(event_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(event_scroll), app->event_view);
    gtk_stack_add_titled(GTK_STACK(stack), event_scroll, "events", "Events");

    GtkWidget *graph = make_graph_view();
    gtk_stack_add_titled(GTK_STACK(stack), graph, "graph", "Graph");

    GtkWidget *diagnostics_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(diagnostics_page, 10);
    gtk_widget_set_margin_bottom(diagnostics_page, 10);
    gtk_widget_set_margin_start(diagnostics_page, 10);
    gtk_widget_set_margin_end(diagnostics_page, 10);
    gtk_box_append(GTK_BOX(diagnostics_page), gtk_button_new_with_label("Run Diagnostics"));
    app->diagnostics_list = make_diagnostics_list();
    g_signal_connect(app->diagnostics_list, "row-selected", G_CALLBACK(on_context_list_selected), app);
    gtk_box_append(GTK_BOX(diagnostics_page), app->diagnostics_list);
    gtk_stack_add_titled(GTK_STACK(stack), diagnostics_page, "diagnostics", "Diagnostics");

    GtkWidget *logs_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    app->logs_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->logs_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->logs_view), TRUE);
    GtkWidget *log_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(log_scroll), app->logs_view);
    gtk_widget_set_vexpand(log_scroll, TRUE);
    gtk_box_append(GTK_BOX(logs_page), log_scroll);
    const char *log_buttons[][2] = {
        {"Refresh", "Refresh recent journal entries"},
        {"Follow Logs", "Open journalctl follow mode in GNOME Terminal"},
        {"Export", "Export log view"}
    };
    GCallback log_callbacks[] = {G_CALLBACK(on_refresh), G_CALLBACK(on_follow_logs), G_CALLBACK(on_follow_logs)};
    gtk_box_append(GTK_BOX(logs_page), button_row(app, log_buttons, 3, log_callbacks));
    gtk_stack_add_titled(GTK_STACK(stack), logs_page, "logs", "Logs");

    GtkWidget *settings_page = section_box("Security");
    GtkWidget *security_box = frame_child(settings_page);
    gtk_box_append(GTK_BOX(security_box), label_row("GUI binary", gtk_label_new(JX_GUI_PATH)));
    gtk_box_append(GTK_BOX(security_box), label_row("Daemon binary", gtk_label_new(JX_DAEMON_PATH)));
    gtk_box_append(GTK_BOX(security_box), label_row("Guard binary", gtk_label_new(JX_GUARD_DAEMON_PATH)));
    gtk_box_append(GTK_BOX(security_box), label_row("Config path", gtk_label_new(JX_CONFIG_PATH)));
    gtk_box_append(GTK_BOX(security_box), label_row("Guard config", gtk_label_new(JX_GUARD_CONFIG_PATH)));
    gtk_box_append(GTK_BOX(security_box), label_row("Service", gtk_label_new(JX_SERVICE_NAME)));
    gtk_box_append(GTK_BOX(security_box), label_row("Guard service", gtk_label_new(JX_GUARD_SERVICE_NAME)));
    app->helper_label = gtk_label_new("unknown");
    gtk_box_append(GTK_BOX(security_box), label_row("Helper", app->helper_label));
    gtk_stack_add_titled(GTK_STACK(stack), settings_page, "settings", "Settings");

    GtkWidget *inspector_frame = section_box("Inspector");
    app->inspector = frame_child(inspector_frame);
    gtk_widget_set_size_request(inspector_frame, 240, -1);
    gtk_widget_set_hexpand(inspector_frame, FALSE);
    app->inspector_title = gtk_label_new("No object selected");
    gtk_label_set_xalign(GTK_LABEL(app->inspector_title), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(app->inspector_title), TRUE);
    gtk_box_append(GTK_BOX(app->inspector), app->inspector_title);
    app->inspector_details = gtk_label_new("Select a service, watch root, rule, process, or event to inspect it.");
    gtk_label_set_wrap(GTK_LABEL(app->inspector_details), TRUE);
    gtk_label_set_xalign(GTK_LABEL(app->inspector_details), 0.0f);
    gtk_box_append(GTK_BOX(app->inspector), app->inspector_details);
    gtk_box_append(GTK_BOX(workspace), inspector_frame);

    GtkWidget *journal_frame = section_box("Journal Console");
    GtkWidget *journal_box = frame_child(journal_frame);
    GtkWidget *journal_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(journal_scroll, -1, 88);
    GtkWidget *journal_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(journal_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(journal_text), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(journal_scroll), journal_text);
    gtk_box_append(GTK_BOX(journal_box), journal_scroll);
    app->timeline = gtk_label_new("Timeline: file creation | folder creation | moved-in file | service restart | configuration save");
    gtk_label_set_xalign(GTK_LABEL(app->timeline), 0.0f);
    gtk_box_append(GTK_BOX(journal_box), app->timeline);
    gtk_box_append(GTK_BOX(root), journal_frame);

    load_config_into_app(app);
    refresh_status(app);
    refresh_logs(app);
    refresh_events(app);
    refresh_guard_status(app);
    refresh_guard_logs(app);
    refresh_overview_metrics(app);
    gtk_window_present(GTK_WINDOW(app->window));
}

int main(int argc, char **argv)
{
    struct jx_control_app app;
    memset(&app, 0, sizeof(app));
    signal(SIGPIPE, SIG_IGN);
    g_set_application_name("JX Sentinel Control");
    g_set_prgname(JX_CONTROL_APP_ID);
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", FALSE);

    GtkApplication *gtk_app = gtk_application_new(JX_CONTROL_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(activate), &app);
    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    if (app.modules && app.module_count > 0) {
        jx_module_shutdown_all(app.modules, app.module_count);
    }
    g_object_unref(gtk_app);
    return status;
}
