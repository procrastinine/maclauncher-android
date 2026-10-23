#include "process-identity.h"
#include "process-ids.h"
#include "thread-identity.h"
#include <jni.h>
#include <stdatomic.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include "proc-maps.h"
extern const char *kl_guest_path(const char *,char *,size_t);

// One immutable Android principal, configured by the inherited supervisor
// channel before any guest code. Darwin credentials are never changed.
static _Atomic int logical_uid = -1;
// Bionic exposes a writable __progname pointer. Keep that data export and
// getprogname on the Android identity, independent of the JVM's host argv.
static char *program_name="",*owned_program_name;
static const char *get_program_name(void){return program_name;}
int klj_process_name(const char *name){
    char *copy=strdup(name);if(!copy)return -1;
    // Diagnostic command discovery must not turn a successful name change into
    // a launch failure when the metadata filesystem cannot persist a name.
    if(klj_process_publish_name(name))fprintf(stderr,"[android-host] Process command metadata: %s\n",strerror(errno));
    char *before=owned_program_name;owned_program_name=copy;
    char *last=strrchr(copy,'/');program_name=last?last+1:copy;free(before);
    return 0;
}
/* Linux cmdline belongs to the Android process name, never the host JVM's
 * executable or launch arguments. Atomic replacement keeps open readers on
 * their existing snapshot and bounds the retained publication to one inode. */
int klj_process_cmdline(const char *path,char *buffer,size_t capacity){
    if(!path)return 0;
    int process;
    if(!strcmp(path,"/proc/self/cmdline"))process=klj_process_id();
    else{
        if(strncmp(path,"/proc/",6)||path[6]<'0'||path[6]>'9')return 0;
        uint64_t id=0;const char *at=path+6;
        while(*at>='0'&&*at<='9'){
            unsigned digit=(unsigned)(*at++-'0');
            if(id>(UINT64_MAX-digit)/10)return 0;
            id=id*10+digit;
        }
        if(strcmp(at,"/cmdline"))return 0;
        process=klj_process_id();if(id!=(uint64_t)process)return 0;
    }
    if(process<0)return -1;
    if(!buffer||!capacity){errno=EINVAL;return -1;}
    const char *root=klj_proc_maps_root();if(!root){errno=ENOENT;return -1;}
    char directory[PATH_MAX];int n=snprintf(directory,sizeof directory,"%s/proc/%d",root,process);
    if(n<0||(size_t)n>=sizeof directory){errno=ENAMETOOLONG;return -1;}
    n=snprintf(buffer,capacity,"%s/cmdline",directory);
    if(n<0||(size_t)n>=capacity){errno=ENAMETOOLONG;return -1;}
    if(mkdir(directory,0700)&&errno!=EEXIST)return -1;
    size_t length=0;char *snapshot=klj_process_commandline(&length);
    if(!snapshot)return -1;
    char temporary[PATH_MAX];
    n=snprintf(temporary,sizeof temporary,"%s/.cmdline.XXXXXX",directory);
    if(n<0||(size_t)n>=sizeof temporary){free(snapshot);errno=ENAMETOOLONG;return -1;}
    int fd=mkstemp(temporary);if(fd<0){free(snapshot);return -1;}
    int ok=fcntl(fd,F_SETFD,FD_CLOEXEC)==0,error=errno;
    for(size_t at=0;ok&&at<length;){
        ssize_t written=write(fd,snapshot+at,length-at);
        if(written>0)at+=(size_t)written;
        else if(written<0&&errno==EINTR)continue;
        else{ok=0;error=written<0?errno:EIO;}
    }
    free(snapshot);
    if(ok&&fchmod(fd,0444)){ok=0;error=errno;}
    if(close(fd)&&ok){ok=0;error=errno;}
    if(ok&&rename(temporary,buffer)){ok=0;error=errno;}
    if(!ok){unlink(temporary);errno=error;return -1;}
    return 1;
}
uid_t klj_process_uid(void) { int uid=atomic_load_explicit(&logical_uid,memory_order_acquire);return uid<0?getuid():(uid_t)uid; }
gid_t klj_process_gid(void) { int uid=atomic_load_explicit(&logical_uid,memory_order_acquire);return uid<0?getgid():(gid_t)uid; }
uid_t klj_process_uid_to_host(uid_t uid) { return uid==klj_process_uid()?getuid():uid; }
gid_t klj_process_gid_to_host(gid_t gid) { return gid==klj_process_gid()?getgid():gid; }
uid_t klj_process_uid_from_host(uid_t uid) { return uid==getuid()?klj_process_uid():uid; }
gid_t klj_process_gid_from_host(gid_t gid) { return gid==getgid()?klj_process_gid():gid; }
int klj_process_setuid(uid_t uid) { if(uid==klj_process_uid())return 0;errno=EPERM;return -1; }
int klj_process_setgid(gid_t gid) { if(gid==klj_process_gid())return 0;errno=EPERM;return -1; }
// Supplementary credentials belong to the same immutable Android principal.
// Never expose or alter the Mac user's host group memberships.
int klj_process_getgroups(int size,gid_t *groups) {
    if(size<0){errno=EINVAL;return -1;}if(!size)return 1;
    if(!groups){errno=EFAULT;return -1;}groups[0]=klj_process_gid();return 1;
}
int klj_process_setgroups(size_t size,const gid_t *groups) {
    if(size&&!groups){errno=EFAULT;return -1;}
    if(size==1&&groups[0]==klj_process_gid())return 0;
    errno=EPERM;return -1;
}
static int set_reuid(uid_t real,uid_t effective) { uid_t uid=klj_process_uid();if((real==(uid_t)-1||real==uid)&&(effective==(uid_t)-1||effective==uid))return 0;errno=EPERM;return -1; }
static int set_regid(gid_t real,gid_t effective) { gid_t gid=klj_process_gid();if((real==(gid_t)-1||real==gid)&&(effective==(gid_t)-1||effective==gid))return 0;errno=EPERM;return -1; }
static int set_resuid(uid_t real,uid_t effective,uid_t saved) { if(saved!=(uid_t)-1&&saved!=klj_process_uid()){errno=EPERM;return -1;}return set_reuid(real,effective); }
static int set_resgid(gid_t real,gid_t effective,gid_t saved) { if(saved!=(gid_t)-1&&saved!=klj_process_gid()){errno=EPERM;return -1;}return set_regid(real,effective); }
static int get_resuid(uid_t *real,uid_t *effective,uid_t *saved) { if(!real||!effective||!saved){errno=EFAULT;return -1;}*real=*effective=*saved=klj_process_uid();return 0; }
static int get_resgid(gid_t *real,gid_t *effective,gid_t *saved) { if(!real||!effective||!saved){errno=EFAULT;return -1;}*real=*effective=*saved=klj_process_gid();return 0; }
static int guest_chown(const char *path,uid_t uid,gid_t gid) { char resolved[PATH_MAX];return chown(kl_guest_path(path,resolved,sizeof(resolved)),klj_process_uid_to_host(uid),klj_process_gid_to_host(gid)); }
static int guest_lchown(const char *path,uid_t uid,gid_t gid) { char resolved[PATH_MAX];return lchown(kl_guest_path(path,resolved,sizeof(resolved)),klj_process_uid_to_host(uid),klj_process_gid_to_host(gid)); }
static int guest_fchown(int fd,uid_t uid,gid_t gid) { return fchown(fd,klj_process_uid_to_host(uid),klj_process_gid_to_host(gid)); }
static int guest_fchownat(int dirfd,const char *path,uid_t uid,gid_t gid,int flags) {
    if(flags&~(0x100|0x1000)){errno=EINVAL;return -1;}
    if(path&&!*path&&(flags&0x1000))return guest_fchown(dirfd,uid,gid);
    char resolved[PATH_MAX];return fchownat(dirfd==-100?AT_FDCWD:dirfd,kl_guest_path(path,resolved,sizeof(resolved)),
        klj_process_uid_to_host(uid),klj_process_gid_to_host(gid),(flags&0x100)?AT_SYMLINK_NOFOLLOW:0);
}
static pid_t klj_process_group(void){return klj_process_getpgid(0);}

void *klj_process_identity_symbol(const char *name) {
    if(!strcmp(name,"getpid"))return (void*)klj_process_id;
    if(!strcmp(name,"getppid"))return (void*)klj_process_parent_id;
    if(!strcmp(name,"gettid"))return (void*)klj_thread_id;
    if(!strcmp(name,"fork"))return (void*)klj_process_fork;
    if(!strcmp(name,"getpgid"))return (void*)klj_process_getpgid;
    if(!strcmp(name,"getpgrp"))return (void*)klj_process_group;
    if(!strcmp(name,"getsid"))return (void*)klj_process_getsid;
    if(!strcmp(name,"setpgid"))return (void*)klj_process_setpgid;
    if(!strcmp(name,"setsid"))return (void*)klj_process_setsid;
    if(!strcmp(name,"__progname"))return &program_name;
    if(!strcmp(name,"getprogname"))return (void*)get_program_name;
    if(!strcmp(name,"getuid")||!strcmp(name,"geteuid"))return (void*)klj_process_uid;
    if(!strcmp(name,"getgid")||!strcmp(name,"getegid"))return (void*)klj_process_gid;
    if(!strcmp(name,"setuid")||!strcmp(name,"seteuid"))return (void*)klj_process_setuid;
    if(!strcmp(name,"setgid")||!strcmp(name,"setegid"))return (void*)klj_process_setgid;
    if(!strcmp(name,"getgroups"))return (void*)klj_process_getgroups;
    if(!strcmp(name,"setgroups"))return (void*)klj_process_setgroups;
    if(!strcmp(name,"setreuid"))return (void*)set_reuid;
    if(!strcmp(name,"setregid"))return (void*)set_regid;
    if(!strcmp(name,"setresuid"))return (void*)set_resuid;
    if(!strcmp(name,"setresgid"))return (void*)set_resgid;
    if(!strcmp(name,"getresuid"))return (void*)get_resuid;
    if(!strcmp(name,"getresgid"))return (void*)get_resgid;
    if(!strcmp(name,"chown"))return (void*)guest_chown;
    if(!strcmp(name,"lchown"))return (void*)guest_lchown;
    if(!strcmp(name,"fchown"))return (void*)guest_fchown;
    if(!strcmp(name,"fchownat"))return (void*)guest_fchownat;
    return NULL;
}

JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostProcessIdentity_configureNative(JNIEnv *env,jclass owner,jint uid,jint host_uid) {
    (void)owner;
    if(uid<0||host_uid<0||(uid_t)host_uid!=getuid()) {
        jclass type=(*env)->FindClass(env,"java/lang/SecurityException");
        if(type){(*env)->ThrowNew(env,type,"Android process authority does not match this host process");(*env)->DeleteLocalRef(env,type);}return;
    }
    if(klj_process_publish_principal((uid_t)uid)) {
        jclass type=(*env)->FindClass(env,"java/lang/SecurityException");
        if(type){(*env)->ThrowNew(env,type,"Android process lease principal could not be configured");(*env)->DeleteLocalRef(env,type);}return;
    }
    int before=-1;
    if(!atomic_compare_exchange_strong_explicit(&logical_uid,&before,uid,memory_order_release,memory_order_acquire)&&before!=uid) {
        jclass type=(*env)->FindClass(env,"java/lang/SecurityException");
        if(type){(*env)->ThrowNew(env,type,"Android process identity is already configured");(*env)->DeleteLocalRef(env,type);}
    }
}
JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostProcessIdentity_physicalPidNative(JNIEnv *env,jclass owner){
    (void)env;(void)owner;return (jint)getpid();
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostProcessIdentity_initializeIdsNative(JNIEnv *env,jclass owner){
    (void)owner;const char *directory=getenv("KL_ANDROID_ID_DIRECTORY");
    if(directory&&*directory&&klj_process_ids_configure(directory)>0)return;
    jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");
    if(type){(*env)->ThrowNew(env,type,"Android process identity namespace is unavailable");(*env)->DeleteLocalRef(env,type);}
}
