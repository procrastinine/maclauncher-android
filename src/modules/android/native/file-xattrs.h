#ifndef MACLAUNCHER_FILE_XATTRS_H
#define MACLAUNCHER_FILE_XATTRS_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Android syscall signatures; errors remain native until the common guest
 * errno boundary (or the original libcore JNI exception boundary). */
ssize_t klj_getxattr(const char *,const char *,void *,size_t);
ssize_t klj_listxattr(const char *,char *,size_t);
int klj_setxattr(const char *,const char *,const void *,size_t,int);
int klj_removexattr(const char *,const char *);
void *klj_xattr_lookup(const char *);
#ifdef __cplusplus
}
#endif
#endif
