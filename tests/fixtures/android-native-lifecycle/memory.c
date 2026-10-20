#include "native-memory-imports.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
void *kl_shim_lookup(const char *name) { return !strcmp(name,"mremap") ? (void*)klj_mremap : NULL; }
void kl_thread_init(void) {}
void klj_enter_guest_thread(void) {}
void klb__exit(int status) { _exit(status); }
