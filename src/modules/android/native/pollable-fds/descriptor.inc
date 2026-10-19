/* Native duplication keeps the original supplier's FileDescription, including
 * counter/timer state and nonblocking flags. There is no parallel fd registry. */
static int directory_description_locked(EpollShimCtx *,int,FileDescription **);
int klj_descriptor_release(int fd,int (*release)(void *),void *context){
    EpollShimCtx *owner;int error=epoll_shim_ctx_global(&owner);if(error){errno=error;return -1;}
    if(fd<0||!release){errno=EINVAL;return -1;}
    error=epoll_shim_ctx_remove_desc(owner,fd,release,context);if(error){errno=error;return -1;}return 0;
}
int klj_pollable_owned(int fd) {
    EpollShimCtx *owner;if(epoll_shim_ctx_global(&owner))return 0;
    rwlock_lock_read(&owner->rwlock);
    int found=epoll_shim_ctx_find_desc_impl(owner,fd)!=NULL;
    rwlock_unlock_read(&owner->rwlock);return found;
}
int klj_pollable_physical(int fd){
    EpollShimCtx *owner;if(epoll_shim_ctx_global(&owner))return 0;
    rwlock_lock_read(&owner->rwlock);FileDescription *desc=epoll_shim_ctx_find_desc_impl(owner,fd);
    int physical=desc&&desc->vtable->native_vectors;rwlock_unlock_read(&owner->rwlock);return physical;
}
size_t klj_pollable_vector_limit(int fd,int writing) {
    EpollShimCtx *owner;if(epoll_shim_ctx_global(&owner))return 0;
    FileDescription *desc=epoll_shim_ctx_find_desc(owner,fd);
    if(!desc)return 0;
    size_t limit=writing?desc->vtable->vector_write_size:desc->vtable->vector_read_limit;
    file_description_unref(&desc);return limit;
}
static errno_t klj_pollable_reserve(EpollShimCtx *owner,unsigned fd) {
    if(fd<owner->open_files_length)return 0;
    size_t length=owner->open_files_length?owner->open_files_length:32;
    while(length<=fd){if(length>SIZE_MAX/2)return ENOMEM;length*=2;}
    if(length>SIZE_MAX/sizeof(FileDescription*))return ENOMEM;
    FileDescription **files=realloc(owner->open_files,length*sizeof(*files));
    if(!files)return ENOMEM;
    memset(files+owner->open_files_length,0,(length-owner->open_files_length)*sizeof(*files));
    owner->open_files=files;owner->open_files_length=(unsigned)length;return 0;
}
int klj_pollable_duplicate(int fd,int target,int mode) {
    if(target<0){errno=mode==2?EBADF:EINVAL;return -1;}
    struct rlimit limit;
    if(!(mode==2&&fd==target)&&getrlimit(RLIMIT_NOFILE,&limit)==0&&(rlim_t)target>=limit.rlim_cur){errno=mode==2?EBADF:EINVAL;return -1;}
    EpollShimCtx *owner;errno_t error=epoll_shim_ctx_global(&owner);
    if(error){errno=error;return -1;}
    rwlock_lock_write(&owner->rwlock);
    FileDescription *source=NULL;
    error=directory_description_locked(owner,fd,&source);
    if(error){rwlock_unlock_write(&owner->rwlock);errno=error;return -1;}
    if(mode==2&&fd==target){int result=real_fcntl(fd,F_GETFD,0);rwlock_unlock_write(&owner->rwlock);return result<0?-1:fd;}
    if(mode==2&&source&&(error=klj_pollable_reserve(owner,(unsigned)target))){rwlock_unlock_write(&owner->rwlock);errno=error;return -1;}
    int result=mode==2?dup2(fd,target):real_fcntl(fd,mode==1?F_DUPFD_CLOEXEC:F_DUPFD,target);
    if(result<0){error=errno;rwlock_unlock_write(&owner->rwlock);errno=error;return -1;}
    if(source&&(error=klj_pollable_reserve(owner,(unsigned)result))){real_close(result);rwlock_unlock_write(&owner->rwlock);errno=error;return -1;}
    FileDescription *old=epoll_shim_ctx_find_desc_impl(owner,result);
    if(old||mode==2){
        /* Every epoll owner appears once even when its descriptor is duplicated. */
        epoll_shim_ctx_for_each_unlocked(owner,remove_desc_lock_epollfd,NULL);
        epoll_shim_ctx_for_each_unlocked(owner,remove_desc_remove_fd_from_epollfd,&result);
        epoll_shim_ctx_for_each_unlocked(owner,remove_desc_unlock_epollfd,NULL);
    }
    if(source)file_description_ref(source);
    if((unsigned)result<owner->open_files_length)owner->open_files[result]=source;
    if(old)file_description_unref(&old);
    rwlock_unlock_write(&owner->rwlock);return result;
}
