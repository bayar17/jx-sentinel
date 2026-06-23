#include "jx_splash.h"

#include <stdlib.h>

#define JX_SPLASH_WIDTH 760
#define JX_SPLASH_HEIGHT 240
#define JX_SPLASH_STRIP_HEIGHT 42
#define JX_SPLASH_BANNER_HEIGHT (JX_SPLASH_HEIGHT - JX_SPLASH_STRIP_HEIGHT)

static void install_splash_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
                                      ".jx-splash-root { background: #10295c; }"
                                      ".jx-splash-strip { background: #132f6b; }"
                                      ".jx-splash-status { color: #f2f6ff; font-size: 16px; }"
                                      ".jx-splash-progress trough { min-height: 18px; background: #0d3ea9; }"
                                      ".jx-splash-progress progress { min-height: 18px; background: #1262ff; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

JxSplash *jx_splash_new(GtkApplication *app)
{
    install_splash_css();

    JxSplash *splash = g_new0(JxSplash, 1);
    splash->window = gtk_application_window_new(app);
    gtk_window_set_decorated(GTK_WINDOW(splash->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(splash->window), JX_SPLASH_WIDTH, JX_SPLASH_HEIGHT);
    gtk_window_set_resizable(GTK_WINDOW(splash->window), FALSE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root, "jx-splash-root");
    gtk_window_set_child(GTK_WINDOW(splash->window), root);

    GtkWidget *banner = gtk_picture_new_for_filename(JX_BANNER_PATH);
    gtk_picture_set_content_fit(GTK_PICTURE(banner), GTK_CONTENT_FIT_FILL);
    gtk_widget_set_size_request(banner, JX_SPLASH_WIDTH, JX_SPLASH_BANNER_HEIGHT);
    gtk_box_append(GTK_BOX(root), banner);

    splash->strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(splash->strip, "jx-splash-strip");
    gtk_widget_set_size_request(splash->strip, JX_SPLASH_WIDTH, JX_SPLASH_STRIP_HEIGHT);
    gtk_widget_set_halign(splash->strip, GTK_ALIGN_FILL);
    gtk_widget_set_margin_top(splash->strip, 0);
    gtk_widget_set_margin_bottom(splash->strip, 0);
    gtk_widget_set_margin_start(splash->strip, 0);
    gtk_widget_set_margin_end(splash->strip, 0);
    gtk_box_append(GTK_BOX(root), splash->strip);

    splash->status = gtk_label_new("Loading jx-sentinel-control");
    gtk_widget_add_css_class(splash->status, "jx-splash-status");
    gtk_label_set_xalign(GTK_LABEL(splash->status), 0.0f);
    gtk_widget_set_margin_start(splash->status, 18);
    gtk_widget_set_hexpand(splash->status, TRUE);
    gtk_box_append(GTK_BOX(splash->strip), splash->status);

    splash->progress = gtk_progress_bar_new();
    gtk_widget_add_css_class(splash->progress, "jx-splash-progress");
    gtk_widget_set_size_request(splash->progress, 240, 18);
    gtk_widget_set_margin_end(splash->progress, 20);
    gtk_widget_set_valign(splash->progress, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(splash->strip), splash->progress);
    return splash;
}

void jx_splash_show(JxSplash *splash)
{
    gtk_window_present(GTK_WINDOW(splash->window));
    for (int i = 0; i < 8; i++) {
        while (g_main_context_pending(NULL)) {
            g_main_context_iteration(NULL, FALSE);
        }
        g_usleep(25000);
    }
}

void jx_splash_update(JxSplash *splash, const char *message, double fraction)
{
    gtk_label_set_text(GTK_LABEL(splash->status), message);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(splash->progress), fraction);
    for (int i = 0; i < 6; i++) {
        while (g_main_context_pending(NULL)) {
            g_main_context_iteration(NULL, FALSE);
        }
        g_usleep(30000);
    }
}

void jx_splash_close(JxSplash *splash)
{
    if (!splash) {
        return;
    }
    gtk_window_destroy(GTK_WINDOW(splash->window));
    g_free(splash);
}
