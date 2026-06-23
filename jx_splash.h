#ifndef JX_SPLASH_H
#define JX_SPLASH_H

#include <gtk/gtk.h>

#define JX_BANNER_PATH "/opt/jx/share/jx-sentinel-control/banner.png"

typedef struct JxSplash {
    GtkWidget *window;
    GtkWidget *progress;
    GtkWidget *status;
    GtkWidget *strip;
} JxSplash;

JxSplash *jx_splash_new(GtkApplication *app);
void jx_splash_show(JxSplash *splash);
void jx_splash_update(JxSplash *splash, const char *message, double fraction);
void jx_splash_close(JxSplash *splash);

#endif
