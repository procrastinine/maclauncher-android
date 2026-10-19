#include "file-xattrs.h"
#include <sys/xattr.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

extern const char *kl_guest_path(const char *,char *,size_t);

/* One stateless macOS boundary serves both the ELF libc imports and libcore.
 * Path authority stays with the existing guest namespace/process sandbox;
 * the kernel owns attribute storage and descriptor/symlink lifetime. */
static int set_flags(int flags,int options) {
    if(flags&~3){errno=EINVAL;return -1;}
    return options|((flags&1)?XATTR_CREATE:0)|((flags&2)?XATTR_REPLACE:0);
}
ssize_t klj_getxattr(const char *file,const char *name,void *value,size_t size) {
    char mapped[PATH_MAX];return getxattr(kl_guest_path(file,mapped,sizeof(mapped)),name,value,size,0,0);
}
static ssize_t klj_lgetxattr(const char *file,const char *name,void *value,size_t size) {
    char mapped[PATH_MAX];return getxattr(kl_guest_path(file,mapped,sizeof(mapped)),name,value,size,0,XATTR_NOFOLLOW);
}
static ssize_t klj_fgetxattr(int fd,const char *name,void *value,size_t size) {
    return fgetxattr(fd,name,value,size,0,0);
}
ssize_t klj_listxattr(const char *file,char *value,size_t size) {
    char mapped[PATH_MAX];return listxattr(kl_guest_path(file,mapped,sizeof(mapped)),value,size,0);
}
static ssize_t klj_llistxattr(const char *file,char *value,size_t size) {
    char mapped[PATH_MAX];return listxattr(kl_guest_path(file,mapped,sizeof(mapped)),value,size,XATTR_NOFOLLOW);
}
static ssize_t klj_flistxattr(int fd,char *value,size_t size) {
    return flistxattr(fd,value,size,0);
}
int klj_setxattr(const char *file,const char *name,const void *value,size_t size,int flags) {
    int options=set_flags(flags,0);if(options<0)return -1;
    char mapped[PATH_MAX];return setxattr(kl_guest_path(file,mapped,sizeof(mapped)),name,value,size,0,options);
}
static int klj_lsetxattr(const char *file,const char *name,const void *value,size_t size,int flags) {
    int options=set_flags(flags,XATTR_NOFOLLOW);if(options<0)return -1;
    char mapped[PATH_MAX];return setxattr(kl_guest_path(file,mapped,sizeof(mapped)),name,value,size,0,options);
}
static int klj_fsetxattr(int fd,const char *name,const void *value,size_t size,int flags) {
    int options=set_flags(flags,0);return options<0?-1:fsetxattr(fd,name,value,size,0,options);
}
int klj_removexattr(const char *file,const char *name) {
    char mapped[PATH_MAX];return removexattr(kl_guest_path(file,mapped,sizeof(mapped)),name,0);
}
static int klj_lremovexattr(const char *file,const char *name) {
    char mapped[PATH_MAX];return removexattr(kl_guest_path(file,mapped,sizeof(mapped)),name,XATTR_NOFOLLOW);
}
static int klj_fremovexattr(int fd,const char *name) {return fremovexattr(fd,name,0);}
void *klj_xattr_lookup(const char *name) {
#define ATTRIBUTE(name_) if(!strcmp(name,#name_))return (void *)klj_##name_;
    ATTRIBUTE(getxattr) ATTRIBUTE(lgetxattr) ATTRIBUTE(fgetxattr)
    ATTRIBUTE(setxattr) ATTRIBUTE(lsetxattr) ATTRIBUTE(fsetxattr)
    ATTRIBUTE(listxattr) ATTRIBUTE(llistxattr) ATTRIBUTE(flistxattr)
    ATTRIBUTE(removexattr) ATTRIBUTE(lremovexattr) ATTRIBUTE(fremovexattr)
#undef ATTRIBUTE
    return NULL;
}
