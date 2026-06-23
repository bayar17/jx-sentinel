#include "../jx_module.h"

#include <stdio.h>
#include <unistd.h>

const char *jx_module_name(void) { return "Service Controller"; }
const char *jx_module_version(void) { return "1.0.0"; }
int jx_module_init(void) { return 0; }
int jx_module_self_test(char *error, size_t error_len)
{
    if (access("/usr/bin/systemctl", X_OK) != 0) {
        snprintf(error, error_len, "cannot execute /usr/bin/systemctl");
        return -1;
    }
    if (access("/opt/jx/libexec/jx-sentinel-helper", X_OK) != 0) {
        snprintf(error, error_len, "cannot execute /opt/jx/libexec/jx-sentinel-helper");
        return -1;
    }
    return 0;
}
void jx_module_shutdown(void) {}
