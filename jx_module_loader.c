#include "jx_module_loader.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static JxModule g_modules[] = {
    {"Configuration Manager", JX_MODULE_DIR "/libjx_config.so", 1, NULL, NULL, NULL, NULL, NULL, NULL, 0, ""},
    {"Service Controller", JX_MODULE_DIR "/libjx_service.so", 1, NULL, NULL, NULL, NULL, NULL, NULL, 0, ""},
    {"Journal Reader", JX_MODULE_DIR "/libjx_logs.so", 1, NULL, NULL, NULL, NULL, NULL, NULL, 0, ""},
    {"Notification Probe", JX_MODULE_DIR "/libjx_notify.so", 0, NULL, NULL, NULL, NULL, NULL, NULL, 0, ""},
    {"Event Graph Renderer", JX_MODULE_DIR "/libjx_graph.so", 0, NULL, NULL, NULL, NULL, NULL, NULL, 0, ""},
    {"Diagnostics Engine", JX_MODULE_DIR "/libjx_diagnostics.so", 0, NULL, NULL, NULL, NULL, NULL, NULL, 0, ""},
};

typedef struct JxRuntimeLibrary {
    const char *display_name;
    const char *soname;
    int required;
} JxRuntimeLibrary;

static const JxRuntimeLibrary g_runtime_libraries[] = {
    {"GTK 4", "libgtk-4.so.1", 1},
    {"Pango", "libpango-1.0.so.0", 1},
    {"GIO", "libgio-2.0.so.0", 1},
    {"GObject", "libgobject-2.0.so.0", 1},
    {"GLib", "libglib-2.0.so.0", 1},
    {"WebKitGTK", "libwebkitgtk-6.0.so.4", 1},
    {"JavaScriptCoreGTK", "libjavascriptcoregtk-6.0.so.1", 1},
    {"C Runtime", "libc.so.6", 1},
};

static char g_last_error[1024];

static const char *module_filename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

JxModule *jx_module_manifest(size_t *count)
{
    *count = sizeof(g_modules) / sizeof(g_modules[0]);
    return g_modules;
}

static int module_file_is_safe(const char *path, char *error, size_t error_len)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(error, error_len, "missing: %s", strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(error, error_len, "not a regular file");
        return -1;
    }
    if (st.st_uid != 0) {
        snprintf(error, error_len, "owner UID is %lu, expected root", (unsigned long)st.st_uid);
        return -1;
    }
    if (st.st_mode & S_IWGRP) {
        snprintf(error, error_len, "group-writable module rejected");
        return -1;
    }
    if (st.st_mode & S_IWOTH) {
        snprintf(error, error_len, "world-writable module rejected");
        return -1;
    }
    return 0;
}

int jx_module_load_next(JxModule *module, char *message, size_t message_len)
{
    module->loaded = 0;
    module->error[0] = '\0';

    if (module_file_is_safe(module->path, module->error, sizeof(module->error)) != 0) {
        snprintf(message, message_len, "%s module failed: %s", module->required ? "Required" : "Optional",
                 module->display_name);
        return module->required ? -1 : 1;
    }

    dlerror();
    module->handle = dlopen(module->path, RTLD_NOW | RTLD_LOCAL);
    if (!module->handle) {
        snprintf(module->error, sizeof(module->error), "%s", dlerror());
        snprintf(message, message_len, "%s module failed: %s", module->required ? "Required" : "Optional",
                 module->display_name);
        return module->required ? -1 : 1;
    }

    dlerror();
    *(void **)(&module->name) = dlsym(module->handle, "jx_module_name");
    *(void **)(&module->version) = dlsym(module->handle, "jx_module_version");
    *(void **)(&module->init) = dlsym(module->handle, "jx_module_init");
    *(void **)(&module->self_test) = dlsym(module->handle, "jx_module_self_test");
    *(void **)(&module->shutdown) = dlsym(module->handle, "jx_module_shutdown");
    const char *sym_error = dlerror();
    if (sym_error || !module->name || !module->version || !module->init || !module->self_test || !module->shutdown) {
        snprintf(module->error, sizeof(module->error), "missing module ABI symbol");
        dlclose(module->handle);
        module->handle = NULL;
        snprintf(message, message_len, "%s module failed: %s", module->required ? "Required" : "Optional",
                 module->display_name);
        return module->required ? -1 : 1;
    }

    if (module->init() != 0) {
        snprintf(module->error, sizeof(module->error), "module init failed");
        dlclose(module->handle);
        module->handle = NULL;
        snprintf(message, message_len, "%s module failed: %s", module->required ? "Required" : "Optional",
                 module->display_name);
        return module->required ? -1 : 1;
    }

    if (module->self_test(module->error, sizeof(module->error)) != 0) {
        if (!module->error[0]) {
            snprintf(module->error, sizeof(module->error), "module self-test failed");
        }
        dlclose(module->handle);
        module->handle = NULL;
        snprintf(message, message_len, "%s module test failed: %s", module->required ? "Required" : "Optional",
                 module->display_name);
        return module->required ? -1 : 1;
    }

    module->loaded = 1;
    snprintf(message, message_len, "Tested %s %s", module->name(), module->version());
    return 0;
}

int jx_module_load_all(JxModule *modules, size_t count, jx_module_status_cb cb, void *user_data)
{
    g_last_error[0] = '\0';
    size_t runtime_count = sizeof(g_runtime_libraries) / sizeof(g_runtime_libraries[0]);
    size_t total_count = runtime_count + count;
    size_t completed = 0;

    for (size_t i = 0; i < runtime_count; i++) {
        const JxRuntimeLibrary *library = &g_runtime_libraries[i];
        char status[768];
        snprintf(status, sizeof(status), "Loading %s", library->soname);
        if (cb) {
            cb(status, total_count ? (double)completed / (double)total_count : 1.0, user_data);
        }

        dlerror();
        void *handle = dlopen(library->soname, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            const char *error = dlerror();
            snprintf(status, sizeof(status), "%s library failed: %s", library->required ? "Required" : "Optional",
                     library->display_name);
            if (cb) {
                cb(status, total_count ? (double)(completed + 1) / (double)total_count : 1.0, user_data);
            }
            if (library->required) {
                snprintf(g_last_error, sizeof(g_last_error), "%s: %s (%s)",
                         library->display_name,
                         error && *error ? error : "unknown dlopen error",
                         library->soname);
                return -1;
            }
        } else {
            dlclose(handle);
            snprintf(status, sizeof(status), "Loaded %s", library->display_name);
            if (cb) {
                cb(status, total_count ? (double)(completed + 1) / (double)total_count : 1.0, user_data);
            }
        }
        completed++;
    }

    for (size_t i = 0; i < count; i++) {
        char status[768];
        snprintf(status, sizeof(status), "Loading %s", module_filename(modules[i].path));
        if (cb) {
            cb(status, total_count ? (double)completed / (double)total_count : 1.0, user_data);
        }

        int rc = jx_module_load_next(&modules[i], status, sizeof(status));
        if (rc > 0) {
            snprintf(status, sizeof(status), "Optional module failed: %s", modules[i].display_name);
        }
        if (cb) {
            cb(status, total_count ? (double)(completed + 1) / (double)total_count : 1.0, user_data);
        }
        if (rc < 0) {
            snprintf(g_last_error, sizeof(g_last_error), "%s: %s (%s)",
                     modules[i].display_name,
                     modules[i].error[0] ? modules[i].error : "unknown error",
                     modules[i].path);
            return -1;
        }
        completed++;
    }

    if (cb) {
        cb("Opening control panel...", 1.0, user_data);
    }
    return 0;
}

const char *jx_module_last_error(void)
{
    return g_last_error[0] ? g_last_error : "unknown module loader error";
}

void jx_module_shutdown_all(JxModule *modules, size_t count)
{
    for (size_t i = count; i > 0; i--) {
        JxModule *module = &modules[i - 1];
        if (module->loaded && module->shutdown) {
            module->shutdown();
        }
        if (module->handle) {
            dlclose(module->handle);
            module->handle = NULL;
        }
        module->loaded = 0;
    }
}
