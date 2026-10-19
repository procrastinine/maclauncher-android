// NDK callback ownership behind the original guest media API. AVFoundation
// requests ranges through these callbacks; no complete-file copy is required.
#include "media-data-source.h"
#include "klepton.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

struct klj_media_source {
    _Atomic unsigned references;
    _Atomic int closed;
    pthread_mutex_t reads;
    void *userdata;
    klj_media_read read;
    klj_media_size size;
    klj_media_close close;
    klj_media_close dispose;
    klj_media_available available;
};
klj_media_source *klj_media_source_new(void) {
    klj_media_source *source=calloc(1,sizeof *source);
    if(!source)return NULL;
    atomic_init(&source->references,1);atomic_init(&source->closed,0);
    if(pthread_mutex_init(&source->reads,NULL)){free(source);return NULL;}
    return source;
}
void klj_media_source_retain(klj_media_source *source){if(source)atomic_fetch_add(&source->references,1);}
void klj_media_source_delete(klj_media_source *source){
    if(source&&atomic_fetch_sub(&source->references,1)==1){
        if(source->dispose)source->dispose(source->userdata);
        pthread_mutex_destroy(&source->reads);free(source);
    }
}
void klj_media_source_userdata(klj_media_source *source,void *userdata){if(source)source->userdata=userdata;}
void klj_media_source_set_read(klj_media_source *source,klj_media_read callback){if(source)source->read=callback;}
void klj_media_source_set_size(klj_media_source *source,klj_media_size callback){if(source)source->size=callback;}
void klj_media_source_set_close(klj_media_source *source,klj_media_close callback){if(source)source->close=callback;}
void klj_media_source_set_dispose(klj_media_source *source,klj_media_close callback){if(source)source->dispose=callback;}
void klj_media_source_set_available(klj_media_source *source,klj_media_available callback){if(source)source->available=callback;}
int klj_media_source_closed(klj_media_source *source){return !source||atomic_load(&source->closed);}
void klj_media_source_close(klj_media_source *source){
    // Close must interrupt blocked guest reads, so it cannot take their lock.
    // Retaining the source during a request keeps the callback owner alive.
    if(source&&!atomic_exchange(&source->closed,1)&&source->close){kl_thread_init();source->close(source->userdata);}
}
ssize_t klj_media_source_read(klj_media_source *source,int64_t offset,void *buffer,size_t size){
    if(!source||offset<0||(!buffer&&size))return -1;
    pthread_mutex_lock(&source->reads);
    ssize_t result=-1;
    if(!klj_media_source_closed(source)&&source->read){
        kl_thread_init();result=source->read(source->userdata,offset,buffer,size);
        if(result>0&&(size_t)result>size)result=-1;
    }
    pthread_mutex_unlock(&source->reads);return result;
}
ssize_t klj_media_source_size(klj_media_source *source){
    if(!source)return -1;
    pthread_mutex_lock(&source->reads);ssize_t result=-1;
    if(!klj_media_source_closed(source)&&source->size){kl_thread_init();result=source->size(source->userdata);}
    pthread_mutex_unlock(&source->reads);return result;
}

struct fd_source { int fd; int64_t offset,size; };
static ssize_t fd_read(void *opaque,int64_t offset,void *buffer,size_t size){
    struct fd_source *source=opaque;
    if(offset<0){errno=EINVAL;return -1;}
    if(!size)return 0;
    if(offset>=source->size)return -1;
    uint64_t remaining=(uint64_t)(source->size-offset);
    if(size>remaining)size=(size_t)remaining;
    if(size>SSIZE_MAX)size=SSIZE_MAX;
    ssize_t result;
    do {result=pread(source->fd,buffer,size,(off_t)(source->offset+offset));} while(result<0&&errno==EINTR);
    return result<=0?(result==0?-1:-2):result;
}
static ssize_t fd_size(void *opaque){return (ssize_t)((struct fd_source *)opaque)->size;}
static ssize_t fd_available(void *opaque,int64_t offset){
    struct fd_source *source=opaque;
    return offset<0?-1:offset>=source->size?0:(ssize_t)(source->size-offset);
}
static void fd_dispose(void *opaque){struct fd_source *source=opaque;close(source->fd);free(source);}
klj_media_source *klj_media_source_from_fd(int fd,int64_t offset,int64_t size){
    if(fd<0||offset<0||size<0){errno=EINVAL;return NULL;}
    int owned=fcntl(fd,F_DUPFD_CLOEXEC,0);
    if(owned<0)return NULL;
    struct stat metadata;
    if(fstat(owned,&metadata)<0){int error=errno;close(owned);errno=error;return NULL;}
    // pread needs a seekable source; preserve range ownership without copying it.
    if(!S_ISREG(metadata.st_mode)&&lseek(owned,0,SEEK_CUR)<0){int error=errno;close(owned);errno=error;return NULL;}
    if(S_ISREG(metadata.st_mode)){
        int64_t available=offset>=metadata.st_size?0:(int64_t)metadata.st_size-offset;
        if(size>available)size=available;
    }
    if(size>INT64_MAX-offset){close(owned);errno=EOVERFLOW;return NULL;}
    struct fd_source *data=malloc(sizeof *data);
    klj_media_source *source=data?klj_media_source_new():NULL;
    if(!source){free(data);close(owned);errno=ENOMEM;return NULL;}
    *data=(struct fd_source){owned,offset,size};
    klj_media_source_userdata(source,data);
    klj_media_source_set_read(source,fd_read);
    klj_media_source_set_size(source,fd_size);
    klj_media_source_set_available(source,fd_available);
    klj_media_source_set_dispose(source,fd_dispose);
    return source;
}
void *klj_media_source_lookup(const char *name){
#define SOURCE(symbol,function) if(!strcmp(name,symbol))return (void *)function
    SOURCE("AMediaDataSource_new",klj_media_source_new);
    SOURCE("AMediaDataSource_delete",klj_media_source_delete);
    SOURCE("AMediaDataSource_close",klj_media_source_close);
    SOURCE("AMediaDataSource_setUserdata",klj_media_source_userdata);
    SOURCE("AMediaDataSource_setReadAt",klj_media_source_set_read);
    SOURCE("AMediaDataSource_setGetSize",klj_media_source_set_size);
    SOURCE("AMediaDataSource_setClose",klj_media_source_set_close);
    SOURCE("AMediaDataSource_setGetAvailableSize",klj_media_source_set_available);
#undef SOURCE
    return NULL;
}
