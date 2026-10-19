#include "directory-bridge.h"
#include "guest-paths.h"
#include "pollable-fds/bridge.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(klj_dirent)==280,"Linux directory record size");

extern int kl_obbmap_readdir(void *, char *, size_t, int *);
extern int kl_obbmap_closedir(void *);
extern int klj_obb_directory_rewind(void *);
extern int klj_directory_fd(void *);
extern int kl_errno_to_linux(int);

typedef struct {
    void *stream;
    int virtual_stream;
    pthread_mutex_t lock;
    long position;
    klj_dirent entry;
} Directory;

static Directory *allocate(void) {
    Directory *value=calloc(1,sizeof(*value));
    if(!value)return NULL;
    int error=pthread_mutex_init(&value->lock,NULL);
    if(error){free(value);errno=error;return NULL;}
    return value;
}
static int close_directory(void *stream){return closedir(stream);}
static int release_directory(void *stream){return klj_descriptor_release(dirfd(stream),close_directory,stream);}
void *klj_directory_create(void *stream,int virtual_stream) {
    if(!stream)return NULL;
    Directory *value=allocate();
    if(!value){int error=errno;if(virtual_stream)kl_obbmap_closedir(stream);else release_directory(stream);errno=error;return NULL;}
    value->stream=stream;value->virtual_stream=virtual_stream;return value;
}
void *klj_directory_fdopen(int fd) {
    Directory *value=allocate();if(!value)return NULL;
    value->stream=fdopendir(fd);
    if(!value->stream){int error=errno;pthread_mutex_destroy(&value->lock);free(value);errno=error;return NULL;}
    return value;
}
int klj_directory_close(void *handle) {
    Directory *value=handle;
    int result=value->virtual_stream?kl_obbmap_closedir(value->stream):release_directory(value->stream),error=errno;
    pthread_mutex_destroy(&value->lock);free(value);errno=error;return result;
}
static klj_dirent *read_locked(Directory *value) {
    klj_dirent *entry=&value->entry;
    if(value->virtual_stream){
        char name[sizeof(entry->d_name)];int is_dir;
        if(kl_obbmap_readdir(value->stream,name,sizeof(name),&is_dir)<=0)return NULL;
        memset(entry,0,sizeof(*entry));entry->d_ino=1;entry->d_type=is_dir?DT_DIR:DT_REG;
        memcpy(entry->d_name,name,strlen(name)+1);entry->d_off=++value->position;
    }else{
        struct dirent *native=readdir(value->stream);if(!native)return NULL;
        memset(entry,0,sizeof(*entry));entry->d_ino=native->d_ino;entry->d_type=native->d_type;
        size_t length=strlen(native->d_name);
        if(length>=sizeof(entry->d_name)){errno=ENAMETOOLONG;return NULL;}
        memcpy(entry->d_name,native->d_name,length+1);
        entry->d_off=value->position=telldir(value->stream);
    }
    entry->d_reclen=sizeof(*entry);return entry;
}
klj_dirent *klj_directory_read(void *handle) {
    Directory *value=handle;pthread_mutex_lock(&value->lock);
    klj_dirent *entry=read_locked(value);int error=errno;
    pthread_mutex_unlock(&value->lock);errno=error;return entry;
}
int klj_directory_read_r(void *handle,klj_dirent *output,klj_dirent **result) {
    Directory *value=handle;pthread_mutex_lock(&value->lock);int previous=errno;errno=0;
    klj_dirent *entry=read_locked(value);int error=entry?0:errno;
    if(entry)*output=*entry;*result=entry?output:NULL;
    pthread_mutex_unlock(&value->lock);errno=previous;return error?kl_errno_to_linux(error):0;
}
int klj_directory_descriptor(void *handle) {
    Directory *value=handle;pthread_mutex_lock(&value->lock);
    int result=klj_directory_fd(value->stream),error=errno;
    pthread_mutex_unlock(&value->lock);errno=error;return result;
}
static void rewind_locked(Directory *value) {
    if(value->virtual_stream)klj_obb_directory_rewind(value->stream);else rewinddir(value->stream);
    value->position=0;
}
void klj_directory_rewind(void *handle) {
    Directory *value=handle;pthread_mutex_lock(&value->lock);rewind_locked(value);
    int error=errno;pthread_mutex_unlock(&value->lock);errno=error;
}
long klj_directory_tell(void *handle) {
    Directory *value=handle;pthread_mutex_lock(&value->lock);
    long result=value->virtual_stream?value->position:telldir(value->stream);int error=errno;
    pthread_mutex_unlock(&value->lock);errno=error;return result;
}
void klj_directory_seek(void *handle,long position) {
    Directory *value=handle;pthread_mutex_lock(&value->lock);
    if(value->virtual_stream){
        /* A merged OBB stream has no single native directory cookie. Its
         * ordinal is an allocation-free position; only backwards seeking
         * replays the merge, while ordinary iteration stays direct. */
        if(position<0)errno=EINVAL;
        else{
            if(position<value->position)rewind_locked(value);
            while(value->position<position&&read_locked(value)){}
        }
    }else{seekdir(value->stream,position);value->position=position;}
    int error=errno;pthread_mutex_unlock(&value->lock);errno=error;
}
void *klj_directory_lookup(const char *name) {
#define DIRECTORY_API(symbol, function) if(!strcmp(name,symbol))return (void *)function
    DIRECTORY_API("fdopendir",klj_directory_fdopen);
    DIRECTORY_API("dirfd",klj_directory_descriptor);
    DIRECTORY_API("rewinddir",klj_directory_rewind);
    DIRECTORY_API("telldir",klj_directory_tell);
    DIRECTORY_API("seekdir",klj_directory_seek);
    DIRECTORY_API("readdir64",klj_directory_read);
    DIRECTORY_API("readdir_r",klj_directory_read_r);
    DIRECTORY_API("readdir64_r",klj_directory_read_r);
#undef DIRECTORY_API
    return NULL;
}

