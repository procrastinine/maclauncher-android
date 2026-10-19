/* Real directory descriptors retain the supplier's existing description
 * owner. Its storage FD is shared only by authenticated descriptor transport. */
static errno_t native_description_read(FileDescription *desc,int fd,void *bytes,size_t size,size_t *count){
    (void)desc;ssize_t value=real_read(fd,bytes,size);if(value<0)return errno;*count=value;return 0;
}
static errno_t native_description_write(FileDescription *desc,int fd,const void *bytes,size_t size,size_t *count){
    (void)desc;ssize_t value=real_write(fd,bytes,size);if(value<0)return errno;*count=value;return 0;
}
static errno_t directory_close(FileDescription *desc){klj_directory_cursor_destroy(desc->ctx.native_context);return 0;}
static size_t directory_metadata(FileDescription *desc,int files[4]){files[0]=klj_directory_cursor_metadata(desc->ctx.native_context);return 1;}
static const struct file_description_vtable directory_vtable={.kind=1,.native_vectors=true,
    .metadata_fun=directory_metadata,.read_fun=native_description_read,.write_fun=native_description_write,.close_fun=directory_close};
static int directory_description_locked(EpollShimCtx *owner,int fd,FileDescription **out){
    FileDescription *desc=epoll_shim_ctx_find_desc_impl(owner,fd);
    if(desc){*out=desc;return 0;}
    struct stat status;if(fstat(fd,&status))return errno;
    if(!S_ISDIR(status.st_mode)){*out=NULL;return 0;}
    int error=klj_pollable_reserve(owner,(unsigned)fd);if(error)return error;
    if((error=file_description_create(&desc)))return error;
    desc->ctx.native_context=klj_directory_cursor_create(fd);
    if(!desc->ctx.native_context){error=errno;file_description_unref(&desc);return error;}
    desc->vtable=&directory_vtable;owner->open_files[fd]=desc;*out=desc;return 0;
}
int klj_directory_descriptor_adopt(int fd){
    EpollShimCtx *owner;int error=epoll_shim_ctx_global(&owner);if(error){errno=error;return -1;}
    rwlock_lock_write(&owner->rwlock);FileDescription *desc;error=directory_description_locked(owner,fd,&desc);
    rwlock_unlock_write(&owner->rwlock);if(error){errno=error;return -1;}return 0;
}
static FileDescription *directory_retain(int fd,int *retained,EpollShimCtx **locked){
    EpollShimCtx *owner;int error=epoll_shim_ctx_global(&owner);if(error){errno=error;return NULL;}
    rwlock_lock_write(&owner->rwlock);FileDescription *desc=NULL;
    error=directory_description_locked(owner,fd,&desc);
    if(!error&&(!desc||desc->vtable->kind!=1))error=ENOTDIR;
    if(!error){*retained=real_fcntl(fd,F_DUPFD_CLOEXEC,0);if(*retained<0)error=errno;else file_description_ref(desc);}
    if(error){rwlock_unlock_write(&owner->rwlock);errno=error;return NULL;}
    /* Holding the existing owner read lock keeps any received metadata alias
     * from closing during a POSIX-lock transaction (Darwin's any-close rule).
     * It also prevents close/reuse of the public FD until its operation ends. */
    rwlock_downgrade(&owner->rwlock);*locked=owner;return desc;
}
ssize_t klj_descriptor_getdents64(int fd,void *buffer,size_t size){
    int retained=-1,cancellation;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancellation);
    EpollShimCtx *owner=NULL;FileDescription *desc=directory_retain(fd,&retained,&owner);
    ssize_t result=-1;
    if(desc){pthread_mutex_lock(&desc->mutex);result=klj_directory_cursor_read(desc->ctx.native_context,retained,buffer,size);
        int error=errno;pthread_mutex_unlock(&desc->mutex);real_close(retained);file_description_unref(&desc);rwlock_unlock_read(&owner->rwlock);errno=error;}
    int error=errno;pthread_setcancelstate(cancellation,NULL);errno=error;return result;
}
off_t klj_descriptor_lseek(int fd,off_t position,int whence){
    int retained=-1,cancellation;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancellation);
    EpollShimCtx *owner=NULL;FileDescription *desc=directory_retain(fd,&retained,&owner);off_t result=-1;
    if(!desc){if(errno==ENOTDIR)result=lseek(fd,position,whence);}
    else{pthread_mutex_lock(&desc->mutex);result=klj_directory_cursor_seek(desc->ctx.native_context,retained,position,whence);
        int error=errno;pthread_mutex_unlock(&desc->mutex);real_close(retained);file_description_unref(&desc);rwlock_unlock_read(&owner->rwlock);errno=error;}
    int error=errno;pthread_setcancelstate(cancellation,NULL);errno=error;return result;
}
int klj_descriptor_export(int fd,KljDescriptorTransfer *record){
    if(!record){errno=EINVAL;return -1;}*record=(KljDescriptorTransfer){.metadata=-1};
    EpollShimCtx *owner;int error=epoll_shim_ctx_global(&owner);if(error){errno=error;return -1;}
    rwlock_lock_write(&owner->rwlock);FileDescription *desc=NULL;error=directory_description_locked(owner,fd,&desc);
    if(!error&&desc&&desc->vtable->metadata_fun){int files[4]={-1,-1,-1,-1};file_description_ref(desc);record->kind=desc->vtable->kind;record->lease=desc;record->count=(uint32_t)desc->vtable->metadata_fun(desc,files);record->metadata=files[0];record->extra[0]=files[1];record->extra[1]=files[2];record->extra[2]=files[3];}
    rwlock_unlock_write(&owner->rwlock);if(error){errno=error;return -1;}return 0;
}
void klj_descriptor_export_release(KljDescriptorTransfer *record){
    if(!record)return;FileDescription *desc=record->lease;if(desc)file_description_unref(&desc);*record=(KljDescriptorTransfer){.metadata=-1};
}
static int directory_description_import(int fd,int metadata){
    EpollShimCtx *owner;int error=epoll_shim_ctx_global(&owner);if(error){real_close(metadata);errno=error;return -1;}
    rwlock_lock_write(&owner->rwlock);FileDescription *desc=NULL;
    if(fd<0||real_fcntl(fd,F_GETFD,0)<0){error=EBADF;goto failed;}
    if(epoll_shim_ctx_find_desc_impl(owner,fd)){error=EEXIST;goto failed;}
    if((error=klj_pollable_reserve(owner,(unsigned)fd)))goto failed;
    /* A second transfer into this process joins its original description.
     * Close the incoming metadata alias under that description's mutex so
     * POSIX any-close lock semantics cannot release an active transaction. */
    for(unsigned i=0;i<owner->open_files_length;i++){
        FileDescription *candidate=owner->open_files[i];
        if(candidate&&candidate->vtable->kind==1&&klj_directory_cursor_matches(candidate->ctx.native_context,metadata)){
            struct stat directory;CursorStorage *state=((KljDirectoryCursor*)candidate->ctx.native_context)->storage;
            if(fstat(fd,&directory)||state->device!=(uint64_t)directory.st_dev||state->inode!=directory.st_ino){error=EINVAL;goto failed;}
            pthread_mutex_lock(&candidate->mutex);real_close(metadata);pthread_mutex_unlock(&candidate->mutex);
            file_description_ref(candidate);owner->open_files[fd]=candidate;rwlock_unlock_write(&owner->rwlock);return 0;
        }
    }
    if((error=file_description_create(&desc)))goto failed;
    desc->ctx.native_context=klj_directory_cursor_import(fd,metadata);metadata=-1;
    if(!desc->ctx.native_context){error=errno;file_description_unref(&desc);goto failed;}
    desc->vtable=&directory_vtable;owner->open_files[fd]=desc;rwlock_unlock_write(&owner->rwlock);return 0;
failed:
    if(metadata>=0)real_close(metadata);rwlock_unlock_write(&owner->rwlock);errno=error;return -1;
}
int klj_descriptor_import(int fd,uint32_t kind,int metadata){
    if(kind==1)return directory_description_import(fd,metadata);
    if(metadata>=0)klj_descriptor_discard(metadata);
    errno=EINVAL;return -1;
}
int klj_descriptor_importv(int fd,uint32_t kind,const int *files,size_t count){
    if(kind==1&&count==1&&files)return directory_description_import(fd,files[0]);
    if(files)for(size_t i=0;i<count;i++)klj_descriptor_discard(files[i]);
    errno=EINVAL;return -1;
}
int klj_descriptor_discard(int fd){
    EpollShimCtx *owner;int error=epoll_shim_ctx_global(&owner);if(error){errno=error;return -1;}
    rwlock_lock_write(&owner->rwlock);int result=real_close(fd);error=errno;
    rwlock_unlock_write(&owner->rwlock);errno=error;return result;
}

static void directory_descriptions_before_fork(void){
    EpollShimCtx *owner=&epoll_shim_ctx_global_;rwlock_lock_write(&owner->rwlock);
    for(unsigned i=0;i<owner->open_files_length;i++){FileDescription *desc=owner->open_files[i];if(!desc||desc->vtable->kind!=1)continue;
        bool seen=false;for(unsigned j=0;j<i;j++)if(owner->open_files[j]==desc){seen=true;break;}
        if(!seen)pthread_mutex_lock(&desc->mutex);}
}
static void directory_descriptions_after_fork(void){
    EpollShimCtx *owner=&epoll_shim_ctx_global_;
    for(unsigned i=0;i<owner->open_files_length;i++){FileDescription *desc=owner->open_files[i];if(!desc||desc->vtable->kind!=1)continue;
        bool seen=false;for(unsigned j=0;j<i;j++)if(owner->open_files[j]==desc){seen=true;break;}
        if(!seen)pthread_mutex_unlock(&desc->mutex);}
    rwlock_unlock_write(&owner->rwlock);
}
static __attribute__((constructor)) void directory_descriptions_initialize(void){pthread_atfork(directory_descriptions_before_fork,directory_descriptions_after_fork,directory_descriptions_after_fork);}
