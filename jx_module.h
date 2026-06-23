#ifndef JX_MODULE_H
#define JX_MODULE_H

#include <stddef.h>

#define JX_MODULE_ABI_VERSION "1.0.0"

typedef const char *(*jx_module_name_fn)(void);
typedef const char *(*jx_module_version_fn)(void);
typedef int (*jx_module_init_fn)(void);
typedef int (*jx_module_self_test_fn)(char *error, size_t error_len);
typedef void (*jx_module_shutdown_fn)(void);

typedef struct JxModule {
    const char *display_name;
    const char *path;
    int required;
    void *handle;
    jx_module_name_fn name;
    jx_module_version_fn version;
    jx_module_init_fn init;
    jx_module_self_test_fn self_test;
    jx_module_shutdown_fn shutdown;
    int loaded;
    char error[512];
} JxModule;

#endif
