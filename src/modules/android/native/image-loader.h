#pragma once
#include "runtime-host.h"
/* Explicit, process-scoped installation after original Bionic initialization.
 * No application filename, ELF class or library name changes this selection. */
int64_t klj_bionic_images_install(KljBionicRuntime *,JNIEnv *);
