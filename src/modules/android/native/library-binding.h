#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The configured application target is immutable before image publication. */
int klj_library_set_target_sdk(unsigned);
unsigned klj_library_target_sdk(void);
uint64_t klj_image_dt_flags_1(void *);
unsigned klj_image_rtld_flags(void *);
unsigned klj_image_target_sdk(void *);
void klj_image_set_rtld_flags(void *,unsigned);
void *klj_image_local_group(void *);
void klj_image_set_local_group(void *,void *);
void *klj_guest_sym_for_image(void *,const char *);
#ifdef __cplusplus
}
#endif
