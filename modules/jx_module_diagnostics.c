#include "../jx_module.h"

#include <stdio.h>
#include <unistd.h>

const char *jx_module_name(void) { return "Diagnostics Engine"; }
const char *jx_module_version(void) { return "1.0.0"; }
int jx_module_init(void) { return 0; }
int jx_module_self_test(char *error, size_t error_len)
{
    const char *required_paths[] = {
        "/opt/jx/bin/jx-sentinel-control",
        "/opt/jx/libexec/jx-sentinel-helper",
        "/opt/jx/etc/jx-sentinel/jx-sentinel.conf",
        "/opt/jx/etc/jx-sentinel/guard.conf",
    };

    for (size_t i = 0; i < sizeof(required_paths) / sizeof(required_paths[0]); i++) {
        if (access(required_paths[i], F_OK) != 0) {
            snprintf(error, error_len, "missing %s", required_paths[i]);
            return -1;
        }
    }
    return 0;
}
void jx_module_shutdown(void) {}
