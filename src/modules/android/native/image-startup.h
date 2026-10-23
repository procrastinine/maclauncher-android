#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The declared virtual process invocation and admitted environment are copied
 * before images load. Constructors share writable, process-lifetime vectors;
 * repeat configuration compares the retained original inputs. */
typedef struct {int argc;char **argv;char **envp;} KljImageStartup;
int klj_library_set_startup(int,const char *const *,size_t,const char *const *);
const KljImageStartup *klj_library_startup(void);
#ifdef __cplusplus
}
#endif
