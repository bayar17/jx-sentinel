#include "jx_guard_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct AgentApp {
    GtkApplication *app;
    GtkWidget *window;
    int fd;
    GIOChannel *channel;
    guint watch_id;
    guint reconnect_id;
    char request_id[64];
} AgentApp;

typedef struct PromptRequest {
    AgentApp *agent;
    char request_id[64];
    char pid[32];
    char uid[32];
    char exe[PATH_MAX];
    char cmd[1024];
    char path[PATH_MAX];
    char action[32];
    char protected_root[PATH_MAX];
} PromptRequest;

static void log_agent(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "jx-permission-agent: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void unescape_value(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '\\' && r[1]) {
            r++;
            if (*r == 't') *w++ = '\t';
            else if (*r == 'n') *w++ = '\n';
            else if (*r == 'r') *w++ = '\r';
            else *w++ = *r;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static void parse_request_line(const char *line, PromptRequest *req)
{
    char copy[JX_GUARD_MAX_LINE];
    snprintf(copy, sizeof(copy), "%s", line);
    for (char *tok = strtok(copy, "\t\n"); tok; tok = strtok(NULL, "\t\n")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = tok;
        char *value = eq + 1;
        unescape_value(value);
        if (strcmp(key, "request_id") == 0) snprintf(req->request_id, sizeof(req->request_id), "%s", value);
        else if (strcmp(key, "pid") == 0) snprintf(req->pid, sizeof(req->pid), "%s", value);
        else if (strcmp(key, "uid") == 0) snprintf(req->uid, sizeof(req->uid), "%s", value);
        else if (strcmp(key, "exe") == 0) snprintf(req->exe, sizeof(req->exe), "%s", value);
        else if (strcmp(key, "cmd") == 0) snprintf(req->cmd, sizeof(req->cmd), "%s", value);
        else if (strcmp(key, "path") == 0) snprintf(req->path, sizeof(req->path), "%s", value);
        else if (strcmp(key, "action") == 0) snprintf(req->action, sizeof(req->action), "%s", value);
        else if (strcmp(key, "protected_root") == 0) snprintf(req->protected_root, sizeof(req->protected_root), "%s", value);
    }
}

static const char *basename_for_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void escape_response(char *dst, size_t dst_len, const char *src)
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

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void send_decision(AgentApp *agent, const char *request_id, const char *decision)
{
    if (agent->fd < 0) return;
    char line[512] = "request_id=";
    escape_response(line, sizeof(line), request_id);
    strncat(line, "\tdecision=", sizeof(line) - strlen(line) - 1);
    escape_response(line, sizeof(line), decision);
    strncat(line, "\n", sizeof(line) - strlen(line) - 1);
    if (write_all(agent->fd, line, strlen(line)) != 0) {
        log_agent("failed to send response: %s", strerror(errno));
    } else {
        log_agent("sent decision request_id=%s decision=%s", request_id, decision);
    }
}

static void show_agent_notification(AgentApp *agent, const char *title, const char *body)
{
    if (!agent || !agent->app) {
        return;
    }
    GNotification *notification = g_notification_new(title);
    g_notification_set_body(notification, body);
    g_notification_set_priority(notification, G_NOTIFICATION_PRIORITY_URGENT);
    g_application_send_notification(G_APPLICATION(agent->app), "jx-sentinel-guard-prompt", notification);
    g_object_unref(notification);
}

static const char *decision_from_zenity_result(int status, const char *output)
{
    if (output && strcmp(output, "Always Allow") == 0) {
        return "allow_always";
    }
    if (output && strcmp(output, "Always Deny") == 0) {
        return "deny_always";
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return "allow_once";
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 5) {
        return "allow_once";
    }
    return "deny_once";
}

static int zenity_display_failed(int status, const char *output)
{
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        return 1;
    }
    if (!output || !*output) {
        return 0;
    }
    return strstr(output, "cannot open display") ||
           strstr(output, "Gtk-WARNING") ||
           strstr(output, "Unable to init server") ||
           strstr(output, "Failed to open display");
}

static int read_child_output(int fd, char *out, size_t out_len)
{
    size_t len = 0;
    while (len + 1 < out_len) {
        ssize_t n = read(fd, out + len, out_len - len - 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
    }
    out[len] = '\0';
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) {
        out[--len] = '\0';
    }
    return len > 0 ? 0 : -1;
}

static const char *run_zenity_prompt(const PromptRequest *req)
{
    const char *app_name = strcmp(req->exe, "unknown") == 0 ? req->cmd : basename_for_path(req->exe);
    const char *root_name = basename_for_path(req->protected_root);
    const char *helper_path = getenv("JX_GUARD_PROMPT_HELPER");
    if (!helper_path || !*helper_path) {
        helper_path = JX_GUARD_PROMPT_HELPER_PATH;
    }
    char *prompt = g_strdup_printf("%s wants to access %s.",
                                   app_name && *app_name ? app_name : "An application",
                                   root_name);
    char *text = g_strdup_printf("%s\n\nExecutable: %s\nCommand: %s\nPath: %s\nAction: %s\nPID: %s\nUID: %s",
                                 prompt,
                                    req->exe, req->cmd, req->path, req->action, req->pid, req->uid);
    show_agent_notification(req->agent, "JX Sentinel Guard", prompt);
    log_agent("launching prompt helper request_id=%s helper=%s", req->request_id, helper_path);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        g_free(prompt);
        g_free(text);
        return "deny_once";
    }

    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        g_free(prompt);
        g_free(text);
        return "deny_once";
    }

    if (child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        setenv("JX_GUARD_PROMPT_TITLE", "JX Sentinel Guard", 1);
        setenv("JX_GUARD_PROMPT_TEXT", text, 1);
        setenv("JX_GUARD_PROMPT_TIMEOUT", "35", 1);
        char *const argv[] = {
            (char *)helper_path,
            NULL
        };
        execve(helper_path, argv, environ);
        _exit(127);
    }

    close(pipefd[1]);
    char selected[128];
    int have_selection = read_child_output(pipefd[0], selected, sizeof(selected)) == 0;
    close(pipefd[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }

    if (WIFEXITED(status)) {
        log_agent("prompt helper exited status=%d output=%s",
                  WEXITSTATUS(status), have_selection ? selected : "");
    } else {
        log_agent("prompt helper exited abnormally output=%s", have_selection ? selected : "");
    }

    g_free(prompt);
    g_free(text);

    if (!WIFEXITED(status)) {
        log_agent("zenity exited abnormally");
        return "deny_once";
    }
    if (zenity_display_failed(status, have_selection ? selected : "")) {
        log_agent("zenity could not display prompt; allowing once to avoid silent suppression: %s",
                  have_selection ? selected : "exec failed");
        return "allow_once";
    }
    if (WEXITSTATUS(status) == 5 && !have_selection) {
        log_agent("zenity timed out; allowing once to avoid invisible prompt suppression");
    } else if (WEXITSTATUS(status) != 0 && !have_selection) {
        log_agent("zenity denied or was cancelled: status=%d", WEXITSTATUS(status));
    }
    return decision_from_zenity_result(status, have_selection ? selected : "");
}

static int run_test_prompt(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    AgentApp agent;
    PromptRequest req;
    memset(&agent, 0, sizeof(agent));
    memset(&req, 0, sizeof(req));
    req.agent = &agent;
    snprintf(req.request_id, sizeof(req.request_id), "test");
    snprintf(req.pid, sizeof(req.pid), "%ld", (long)getpid());
    snprintf(req.uid, sizeof(req.uid), "%lu", (unsigned long)getuid());
    snprintf(req.exe, sizeof(req.exe), "/usr/bin/touch");
    snprintf(req.cmd, sizeof(req.cmd), "touch /home/jackson/Desktop/guard-test.txt");
    snprintf(req.path, sizeof(req.path), "/home/jackson/Desktop/guard-test.txt");
    snprintf(req.action, sizeof(req.action), "open");
    snprintf(req.protected_root, sizeof(req.protected_root), "/home/jackson/Desktop");
    const char *decision = run_zenity_prompt(&req);
    printf("%s\n", decision);
    return strcmp(decision, "deny_once") == 0 ? 1 : 0;
}

static void show_permission_prompt(PromptRequest *req)
{
    const char *decision = run_zenity_prompt(req);
    send_decision(req->agent, req->request_id, decision);
    g_free(req);
}

static void disconnect_agent(AgentApp *agent)
{
    if (agent->watch_id) {
        g_source_remove(agent->watch_id);
        agent->watch_id = 0;
    }
    if (agent->channel) {
        g_io_channel_unref(agent->channel);
        agent->channel = NULL;
    }
    if (agent->fd >= 0) {
        close(agent->fd);
        agent->fd = -1;
    }
}

static gboolean hide_agent_window(gpointer user_data)
{
    AgentApp *agent = user_data;
    if (agent->window) {
        gtk_widget_set_visible(agent->window, FALSE);
    }
    return G_SOURCE_REMOVE;
}

static gboolean connect_agent(gpointer user_data);

static gboolean on_socket_ready(GIOChannel *channel, GIOCondition condition, gpointer user_data)
{
    AgentApp *agent = user_data;
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        disconnect_agent(agent);
        if (!agent->reconnect_id) agent->reconnect_id = g_timeout_add_seconds(2, connect_agent, agent);
        return G_SOURCE_REMOVE;
    }

    gchar *line = NULL;
    gsize len = 0;
    GError *error = NULL;
    GIOStatus status = g_io_channel_read_line(channel, &line, &len, NULL, &error);
    if (status == G_IO_STATUS_NORMAL && line) {
        PromptRequest *req = g_new0(PromptRequest, 1);
        req->agent = agent;
        parse_request_line(line, req);
        if (req->request_id[0] && req->path[0]) {
            log_agent("received request request_id=%s exe=%s path=%s", req->request_id, req->exe, req->path);
            show_permission_prompt(req);
        } else {
            g_free(req);
        }
    } else if (status == G_IO_STATUS_EOF || status == G_IO_STATUS_ERROR) {
        if (error) {
            log_agent("socket read failed: %s", error->message);
            g_error_free(error);
        }
        g_free(line);
        disconnect_agent(agent);
        if (!agent->reconnect_id) agent->reconnect_id = g_timeout_add_seconds(2, connect_agent, agent);
        return G_SOURCE_REMOVE;
    }
    g_free(line);
    return G_SOURCE_CONTINUE;
}

static gboolean connect_agent(gpointer user_data)
{
    AgentApp *agent = user_data;
    agent->reconnect_id = 0;
    if (agent->fd >= 0) return G_SOURCE_REMOVE;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        agent->reconnect_id = g_timeout_add_seconds(2, connect_agent, agent);
        return G_SOURCE_REMOVE;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", JX_GUARD_SOCKET_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        agent->reconnect_id = g_timeout_add_seconds(2, connect_agent, agent);
        return G_SOURCE_REMOVE;
    }

    agent->fd = fd;
    agent->channel = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(agent->channel, NULL, NULL);
    g_io_channel_set_buffered(agent->channel, TRUE);
    agent->watch_id = g_io_add_watch(agent->channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                     on_socket_ready, agent);
    if (write_all(agent->fd, "type=hello\n", strlen("type=hello\n")) != 0) {
        log_agent("failed to send hello: %s", strerror(errno));
    }
    log_agent("connected to %s", JX_GUARD_SOCKET_PATH);
    return G_SOURCE_REMOVE;
}

static void activate(GtkApplication *app, gpointer user_data)
{
    AgentApp *agent = user_data;
    agent->app = app;
    g_application_hold(G_APPLICATION(app));
    log_agent("started prompt backend=shell helper=%s", JX_GUARD_PROMPT_HELPER_PATH);

    if (!agent->window) {
        agent->window = gtk_application_window_new(app);
        gtk_window_set_title(GTK_WINDOW(agent->window), "JX Permission Agent");
        gtk_window_set_default_size(GTK_WINDOW(agent->window), 1, 1);
        gtk_window_set_deletable(GTK_WINDOW(agent->window), FALSE);
        gtk_widget_set_opacity(agent->window, 0.0);
        gtk_window_present(GTK_WINDOW(agent->window));
        g_idle_add(hide_agent_window, agent);
    }

    connect_agent(agent);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--test-prompt") == 0) {
        return run_test_prompt(argc, argv);
    }

    AgentApp agent;
    memset(&agent, 0, sizeof(agent));
    agent.fd = -1;

    GtkApplication *app = gtk_application_new("org.jx.sentinel.permission-agent", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &agent);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    disconnect_agent(&agent);
    g_application_release(G_APPLICATION(app));
    g_object_unref(app);
    return rc;
}
