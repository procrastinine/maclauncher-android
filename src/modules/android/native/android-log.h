#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Physical log output has its own process lifetime. Guest stdio can be closed
 * or redirected without redirecting Android's logger back into its reader. */
void klj_android_log_initialize(void);
int klj_android_log_write(int priority,const char *tag,const char *message);
#ifdef __cplusplus
}
#endif
