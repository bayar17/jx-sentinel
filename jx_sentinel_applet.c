#include "jx_sentinel_control.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct Applet {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *guard_status;
} Applet;

static int run_child(char *const argv[])
{
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execve(argv[0], argv, environ);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static char *capture_child(char *const argv[])
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return NULL;
    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    if (child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execve(argv[0], argv, environ);
        _exit(127);
    }
    close(pipefd[1]);
    GString *out = g_string_new(NULL);
    char buf[1024];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        g_string_append_len(out, buf, (gssize)n);
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    (void)status;
    return g_string_free(out, FALSE);
}

static void refresh_status(Applet *applet)
{
    char *argv[] = {"/usr/bin/systemctl", "is-active", JX_GUARD_SERVICE_NAME, NULL};
    char *out = capture_child(argv);
    char *status = out ? g_strstrip(out) : NULL;
    gtk_label_set_text(GTK_LABEL(applet->guard_status), status && *status ? status : "unknown");
    g_free(out);
}

static void on_open_control(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    pid_t child = fork();
    if (child == 0) {
        char *const argv[] = {JX_GUI_PATH, NULL};
        execve(JX_GUI_PATH, argv, environ);
        _exit(127);
    }
}

static void on_restart_guard(GtkButton *button, gpointer user_data)
{
    (void)button;
    Applet *applet = user_data;
    char *const argv[] = {"/usr/bin/pkexec", JX_HELPER_PATH, "--guard-restart", NULL};
    run_child(argv);
    refresh_status(applet);
}

static void on_prompt_mode(GtkButton *button, gpointer user_data)
{
    (void)button;
    Applet *applet = user_data;
    char *const argv[] = {"/usr/bin/pkexec", JX_HELPER_PATH, "--guard-mode-prompt", NULL};
    run_child(argv);
    refresh_status(applet);
}

static void on_log_mode(GtkButton *button, gpointer user_data)
{
    (void)button;
    Applet *applet = user_data;
    char *const argv[] = {"/usr/bin/pkexec", JX_HELPER_PATH, "--guard-mode-log", NULL};
    run_child(argv);
    refresh_status(applet);
}

static void on_open_logs(GtkButton *button, gpointer user_data)
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
            JX_GUARD_SERVICE_NAME,
            "-f",
            NULL
        };
        execve("/usr/bin/gnome-terminal", argv, environ);
        _exit(127);
    }
}

static void on_quit(GtkButton *button, gpointer user_data)
{
    (void)button;
    Applet *applet = user_data;
    g_application_quit(G_APPLICATION(applet->app));
}

static GtkWidget *applet_button(const char *label, GCallback callback, Applet *applet)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_hexpand(button, TRUE);
    g_signal_connect(button, "clicked", callback, applet);
    return button;
}

static void activate(GtkApplication *app, gpointer user_data)
{
    Applet *applet = user_data;
    applet->app = app;
    applet->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(applet->window), "JX Sentinel Applet");
    gtk_window_set_default_size(GTK_WINDOW(applet->window), 300, 320);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_window_set_child(GTK_WINDOW(applet->window), box);

    GtkWidget *title = gtk_label_new("JX Sentinel Guard");
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.16));
    gtk_label_set_attributes(GTK_LABEL(title), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_append(GTK_BOX(box), title);

    applet->guard_status = gtk_label_new("unknown");
    gtk_box_append(GTK_BOX(box), applet->guard_status);
    gtk_box_append(GTK_BOX(box), applet_button("Open JX Sentinel Control", G_CALLBACK(on_open_control), applet));
    gtk_box_append(GTK_BOX(box), applet_button("Enable Prompt Mode", G_CALLBACK(on_prompt_mode), applet));
    gtk_box_append(GTK_BOX(box), applet_button("Enable Log Mode", G_CALLBACK(on_log_mode), applet));
    gtk_box_append(GTK_BOX(box), applet_button("Restart Guard", G_CALLBACK(on_restart_guard), applet));
    gtk_box_append(GTK_BOX(box), applet_button("Open Guard Logs", G_CALLBACK(on_open_logs), applet));
    gtk_box_append(GTK_BOX(box), applet_button("Quit Applet", G_CALLBACK(on_quit), applet));

    refresh_status(applet);
    gtk_window_present(GTK_WINDOW(applet->window));
}

int main(int argc, char **argv)
{
    Applet applet;
    memset(&applet, 0, sizeof(applet));
    signal(SIGPIPE, SIG_IGN);
    GtkApplication *app = gtk_application_new("org.jx.sentinel.applet", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &applet);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
