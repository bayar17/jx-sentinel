#ifndef JX_MODULE_LOADER_H
#define JX_MODULE_LOADER_H

#include "jx_module.h"

#include <stddef.h>

#define JX_MODULE_DIR "/opt/jx/lib/jx-sentinel-control"

typedef void (*jx_module_status_cb)(const char *message, double fraction, void *user_data);

JxModule *jx_module_manifest(size_t *count);
int jx_module_load_next(JxModule *module, char *message, size_t message_len);
int jx_module_load_all(JxModule *modules, size_t count, jx_module_status_cb cb, void *user_data);
const char *jx_module_last_error(void);
void jx_module_shutdown_all(JxModule *modules, size_t count);

#endif
