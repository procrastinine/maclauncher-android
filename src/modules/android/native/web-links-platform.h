#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int klj_web_links_configure(int allowed);
int klj_web_links_open(const void* bytes,size_t length);
void klj_web_links_close(void);
#ifdef __cplusplus
}
#endif
