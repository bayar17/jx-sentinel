#include "../jx_module.h"

#include <stdio.h>
#include <unistd.h>

const char *jx_module_name(void) { return "Notification Probe"; }
const char *jx_module_version(void) { return "1.0.0"; }
int jx_module_init(void) { return access("/usr/bin/notify-send", X_OK) == 0 ? 0 : -1; }
int jx_module_self_test(char *error, size_t error_len)
{
    if (access("/usr/bin/notify-send", X_OK) != 0) {
        snprintf(error, error_len, "cannot execute /usr/bin/notify-send");
        return -1;
    }
    return 0;
}
void jx_module_shutdown(void) {}
