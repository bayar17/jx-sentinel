#include "../jx_module.h"

#include <stdio.h>
#include <unistd.h>

const char *jx_module_name(void) { return "Configuration Manager"; }
const char *jx_module_version(void) { return "1.0.0"; }
int jx_module_init(void) { return 0; }
int jx_module_self_test(char *error, size_t error_len)
{
    if (access("/opt/jx/etc/jx-sentinel/jx-sentinel.conf", R_OK) != 0) {
        snprintf(error, error_len, "cannot read /opt/jx/etc/jx-sentinel/jx-sentinel.conf");
        return -1;
    }
    return 0;
}
void jx_module_shutdown(void) {}
