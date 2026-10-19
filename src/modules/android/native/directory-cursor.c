#include "directory-cursor.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern ssize_t __getdirentries64(int,void *,size_t,off_t *);
enum {NATIVE_BATCH=65536,COOKIE_START=131072};
typedef struct {off_t base,end,cookie,next;size_t offset,size;int valid;} CursorState;
typedef struct {
    char magic[8];uint32_t version,dirty,active;uint64_t device,inode;
    CursorState state[2];
    unsigned char entries[NATIVE_BATCH];
} CursorStorage;
typedef struct {off_t base;uint64_t offset;} CursorCookie;
struct KljDirectoryCursor {int descriptor;CursorStorage *storage;};
_Static_assert(sizeof(CursorStorage)<COOKIE_START,"cursor storage overlaps cookies");

static int storage_lock(int fd,short kind){
    struct flock lock={.l_type=kind,.l_whence=SEEK_SET,.l_start=0,.l_len=1};
    int result;do{result=fcntl(fd,kind==F_UNLCK?F_SETLK:F_SETLKW,&lock);}while(result<0&&errno==EINTR);
    return result;
}
static int valid_state(const CursorStorage *value){
    const CursorState *state=&value->state[__atomic_load_n(&value->active,__ATOMIC_ACQUIRE)&1];
    return !memcmp(value->magic,"ADIRFD1",8)&&value->version==1&&
        state->offset<=state->size&&state->size<=NATIVE_BATCH&&
        state->next>=COOKIE_START&&!(state->next%sizeof(CursorCookie));
}
static KljDirectoryCursor *adopt(int directory,int metadata,int initialize){
    struct stat target,shared;
    if(fstat(directory,&target)||fcntl(metadata,F_SETFD,FD_CLOEXEC))goto failed;
    if(!S_ISDIR(target.st_mode)){errno=ENOTDIR;goto failed;}
    if(storage_lock(metadata,F_WRLCK))goto failed;
    if(fstat(metadata,&shared))goto failed_locked;
    if(!S_ISREG(shared.st_mode)||shared.st_uid!=getuid()||(shared.st_mode&077)||shared.st_size<COOKIE_START){errno=EINVAL;goto failed_locked;}
    CursorStorage *storage=mmap(NULL,COOKIE_START,PROT_READ|PROT_WRITE,MAP_SHARED,metadata,0);
    if(storage==MAP_FAILED)goto failed_locked;
    if(initialize){
        off_t position=lseek(directory,0,SEEK_CUR);if(position<0){munmap(storage,COOKIE_START);goto failed_locked;}
        *storage=(CursorStorage){.magic="ADIRFD1",.version=1,.device=(uint64_t)target.st_dev,.inode=target.st_ino,
            .state={{.base=position,.end=position,.next=COOKIE_START}}};
    }
    if(!valid_state(storage)||storage->device!=(uint64_t)target.st_dev||storage->inode!=target.st_ino||storage->state[storage->active&1].next>shared.st_size){
        munmap(storage,COOKIE_START);errno=EINVAL;goto failed_locked;
    }
    KljDirectoryCursor *cursor=malloc(sizeof *cursor);
    if(!cursor){munmap(storage,COOKIE_START);goto failed_locked;}
    *cursor=(KljDirectoryCursor){metadata,storage};storage_lock(metadata,F_UNLCK);return cursor;
failed_locked:;
    int locked_error=errno;storage_lock(metadata,F_UNLCK);errno=locked_error;
failed:;
    int error=errno;close(metadata);errno=error;return NULL;
}
static int descriptor_storage_create(size_t size){
    const char *temporary=getenv("TMPDIR");if(!temporary||!*temporary)temporary="/tmp";
    char name[PATH_MAX];int length=snprintf(name,sizeof name,"%s/maclauncher-descriptor-XXXXXX",temporary);
    if(length<0||(size_t)length>=sizeof name){errno=ENAMETOOLONG;return -1;}
    int descriptor=mkstemp(name);if(descriptor<0)return -1;
    int result=unlink(name);if(!result)result=fcntl(descriptor,F_SETFD,FD_CLOEXEC);
    if(!result)result=ftruncate(descriptor,(off_t)size);
    if(result){int error=errno;close(descriptor);errno=error;return -1;}return descriptor;
}
KljDirectoryCursor *klj_directory_cursor_create(int directory){
    int descriptor=descriptor_storage_create(COOKIE_START);if(descriptor<0)return NULL;
    return adopt(directory,descriptor,1);
}
KljDirectoryCursor *klj_directory_cursor_import(int directory,int metadata){return adopt(directory,metadata,0);}
int klj_directory_cursor_matches(KljDirectoryCursor *cursor,int descriptor){
    struct stat left,right;return !fstat(cursor->descriptor,&left)&&!fstat(descriptor,&right)&&left.st_dev==right.st_dev&&left.st_ino==right.st_ino;
}
int klj_directory_cursor_metadata(KljDirectoryCursor *cursor){return cursor->descriptor;}
void klj_directory_cursor_destroy(KljDirectoryCursor *cursor){if(!cursor)return;int error=errno;munmap(cursor->storage,COOKIE_START);close(cursor->descriptor);free(cursor);errno=error;}

static int refill(KljDirectoryCursor *cursor,int directory,CursorState *state){
    off_t base=state->end;
    if(lseek(directory,base,SEEK_SET)<0)return -1;
    ssize_t size=__getdirentries64(directory,cursor->storage->entries,NATIVE_BATCH,&base);
    if(size<0)return -1;
    off_t end=lseek(directory,0,SEEK_CUR);if(end<0)return -1;
    state->base=base;state->end=end;state->size=(size_t)size;state->offset=0;state->valid=1;return 0;
}
static int recover(KljDirectoryCursor *cursor,int directory,CursorState *state){
    if(!cursor->storage->dirty)return 0;
    if(!state->valid)return lseek(directory,state->end,SEEK_SET)<0?-1:0;
    size_t offset=state->offset;state->end=state->base;
    if(refill(cursor,directory,state))return -1;
    if(offset>state->size){errno=EINVAL;return -1;}
    state->offset=offset;return 0;
}
static int begin(KljDirectoryCursor *cursor,int directory,CursorState *state){
    if(storage_lock(cursor->descriptor,F_WRLCK))return -1;
    if(!valid_state(cursor->storage)){errno=EIO;storage_lock(cursor->descriptor,F_UNLCK);return -1;}
    *state=cursor->storage->state[__atomic_load_n(&cursor->storage->active,__ATOMIC_ACQUIRE)&1];
    if(recover(cursor,directory,state)){int error=errno;storage_lock(cursor->descriptor,F_UNLCK);errno=error;return -1;}
    /* POSIX locks release on process death. A dirty transaction is reloaded
     * from the last published native batch/cursor by the next real FD owner. */
    cursor->storage->dirty=1;return 0;
}
static void finish(KljDirectoryCursor *cursor,const CursorState *state,int commit){
    int error=errno;
    if(commit){unsigned next=(cursor->storage->active&1)^1;cursor->storage->state[next]=*state;
        __atomic_store_n(&cursor->storage->active,next,__ATOMIC_RELEASE);__atomic_store_n(&cursor->storage->dirty,0,__ATOMIC_RELEASE);}
    storage_lock(cursor->descriptor,F_UNLCK);errno=error;
}
static int entry_at(const unsigned char *buffer,size_t size,size_t offset,const struct dirent **result){
    if(offset>size||size-offset<offsetof(struct dirent,d_name)+1){errno=EIO;return -1;}
    const struct dirent *entry=(const void *)(buffer+offset);
    if(entry->d_reclen<offsetof(struct dirent,d_name)+1||entry->d_reclen>size-offset||
        entry->d_namlen>entry->d_reclen- offsetof(struct dirent,d_name)-1||entry->d_name[entry->d_namlen]){errno=EIO;return -1;}
    *result=entry;return 0;
}
ssize_t klj_directory_cursor_read(KljDirectoryCursor *cursor,int directory,void *buffer,size_t capacity){
    if(!buffer&&capacity){errno=EFAULT;return -1;}
    if(capacity<24){errno=EINVAL;return -1;}
    if(capacity>SSIZE_MAX)capacity=SSIZE_MAX;
    CursorState state;if(begin(cursor,directory,&state))return -1;
    size_t produced=0;
    for(;;){
        if(!state.valid||state.offset==state.size){if(refill(cursor,directory,&state))break;if(!state.size){finish(cursor,&state,1);return produced;}}
        const struct dirent *entry;if(entry_at(cursor->storage->entries,state.size,state.offset,&entry))break;
        if(entry->d_namlen>255){errno=ENAMETOOLONG;break;}
        if(!entry->d_ino){state.offset+=entry->d_reclen;continue;}
        size_t length=(19u+entry->d_namlen+1u+7u)&~7u;
        if(length>capacity-produced){if(!produced)errno=EINVAL;break;}
        if(state.next>INT64_MAX-(off_t)sizeof(CursorCookie)){errno=EOVERFLOW;break;}
        CursorCookie cookie={state.base,state.offset+entry->d_reclen};
        if(pwrite(cursor->descriptor,&cookie,sizeof cookie,state.next)!=sizeof cookie){if(!errno)errno=EIO;break;}
        uint64_t inode=entry->d_ino;unsigned char type=entry->d_type;
        unsigned char *out=(unsigned char*)buffer+produced;uint16_t record=(uint16_t)length;
        memset(out,0,length);memcpy(out,&inode,8);memcpy(out+8,&state.next,8);memcpy(out+16,&record,2);out[18]=type;memcpy(out+19,entry->d_name,entry->d_namlen+1);
        state.cookie=state.next;state.next+=sizeof cookie;state.offset+=entry->d_reclen;produced+=length;
        if(capacity-produced<24){finish(cursor,&state,1);return produced;}
    }
    finish(cursor,&state,produced!=0);return produced?(ssize_t)produced:-1;
}
off_t klj_directory_cursor_seek(KljDirectoryCursor *cursor,int directory,off_t offset,int whence){
    CursorState state;if(begin(cursor,directory,&state))return -1;
    if(whence==SEEK_CUR&&offset==0){finish(cursor,&state,1);return state.cookie;}
    if(whence!=SEEK_SET||offset<0){errno=EINVAL;finish(cursor,&state,0);return -1;}
    if(!offset){state.base=state.end=state.cookie=0;state.offset=state.size=0;state.valid=0;
        if(lseek(directory,0,SEEK_SET)<0){finish(cursor,&state,0);return -1;}
        finish(cursor,&state,1);return 0;}
    CursorCookie cookie;
    if(offset<COOKIE_START||offset%sizeof cookie||offset>=state.next||pread(cursor->descriptor,&cookie,sizeof cookie,offset)!=sizeof cookie){errno=EINVAL;finish(cursor,&state,0);return -1;}
    state.end=cookie.base;
    if(refill(cursor,directory,&state)){finish(cursor,&state,0);return -1;}
    size_t at=0;
    while(at<cookie.offset&&at<state.size){const struct dirent *entry;if(entry_at(cursor->storage->entries,state.size,at,&entry))break;at+=entry->d_reclen;}
    if(at!=cookie.offset){errno=EINVAL;finish(cursor,&state,0);return -1;}
    state.offset=at;state.cookie=offset;finish(cursor,&state,1);return offset;
}
