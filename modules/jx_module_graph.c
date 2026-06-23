#include "../jx_module.h"

#include <dlfcn.h>
#include <stdio.h>

const char *jx_module_name(void) { return "Event Graph Renderer"; }
const char *jx_module_version(void) { return "1.0.0"; }
int jx_module_init(void) { return 0; }
int jx_module_self_test(char *error, size_t error_len)
{
    void *webkit = dlopen("libwebkitgtk-6.0.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!webkit) {
        snprintf(error, error_len, "cannot load libwebkitgtk-6.0.so.4");
        return -1;
    }
    dlclose(webkit);

    void *jsc = dlopen("libjavascriptcoregtk-6.0.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!jsc) {
        snprintf(error, error_len, "cannot load libjavascriptcoregtk-6.0.so.1");
        return -1;
    }
    dlclose(jsc);
    return 0;
}
void jx_module_shutdown(void) {}
