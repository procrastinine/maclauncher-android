/* Auxiliary readonly handles follow the original open-file description across
 * dup and close. These are physical files, with no extra transport metadata. */
extern void klj_ashmem_capability_release(int64_t);
typedef struct {int readonly;int64_t access;} KljReadonlyDescription;
static errno_t readonly_description_close(FileDescription *desc){
    KljReadonlyDescription *value=desc->ctx.native_context;int error=0;
    if(value->readonly>=0&&real_close(value->readonly))error=errno;
    if(value->access>=0)klj_ashmem_capability_release(value->access);
    free(value);return error;
}
static const struct file_description_vtable readonly_description_vtable={.native_vectors=true,
    .read_fun=native_description_read,.write_fun=native_description_write,.close_fun=readonly_description_close};
int klj_descriptor_readonly_adopt(int fd,int readonly){
    EpollShimCtx *owner;int error=epoll_shim_ctx_global(&owner);if(error){errno=error;return -1;}
    rwlock_lock_write(&owner->rwlock);
    if(fd<0||readonly<0||real_fcntl(fd,F_GETFD,0)<0||real_fcntl(readonly,F_GETFD,0)<0){error=EBADF;goto done;}
    if(epoll_shim_ctx_find_desc_impl(owner,fd)){error=EEXIST;goto done;}
    if((error=klj_pollable_reserve(owner,(unsigned)fd)))goto done;
    KljReadonlyDescription *value=malloc(sizeof(*value));if(!value){error=ENOMEM;goto done;}
    FileDescription *desc;if((error=file_description_create(&desc))){free(value);goto done;}
    value->readonly=readonly;value->access=-1;desc->ctx.native_context=value;desc->vtable=&readonly_description_vtable;owner->open_files[fd]=desc;
done:
    rwlock_unlock_write(&owner->rwlock);if(error){errno=error;return -1;}return 0;
}
int klj_descriptor_access_adopt(int fd,int64_t access){
    EpollShimCtx *owner;int error=epoll_shim_ctx_global(&owner);if(error){errno=error;return -1;}
    rwlock_lock_write(&owner->rwlock);
    if(fd<0||access<0||real_fcntl(fd,F_GETFD,0)<0){error=EBADF;goto done;}
    FileDescription *desc=epoll_shim_ctx_find_desc_impl(owner,fd);
    if(desc&&desc->vtable!=&readonly_description_vtable){error=EEXIST;goto done;}
    if(!desc){
        if((error=klj_pollable_reserve(owner,(unsigned)fd)))goto done;
        KljReadonlyDescription *value=malloc(sizeof(*value));if(!value){error=ENOMEM;goto done;}
        if((error=file_description_create(&desc))){free(value);goto done;}
        value->readonly=-1;value->access=-1;desc->ctx.native_context=value;desc->vtable=&readonly_description_vtable;owner->open_files[fd]=desc;
    }
    KljReadonlyDescription *value=desc->ctx.native_context;
    if(value->access>=0)klj_ashmem_capability_release(access);else value->access=access;
done:
    rwlock_unlock_write(&owner->rwlock);if(error){errno=error;return -1;}return 0;
}
int klj_descriptor_readonly_seal(int fd){
    EpollShimCtx *owner;int error=epoll_shim_ctx_global(&owner);if(error){errno=error;return -1;}
    rwlock_lock_write(&owner->rwlock);FileDescription *desc=epoll_shim_ctx_find_desc_impl(owner,fd);
    if(desc&&desc->vtable==&readonly_description_vtable){
        KljReadonlyDescription *value=desc->ctx.native_context;
        if(value->readonly>=0){if(dup2(value->readonly,fd)<0||real_fcntl(fd,F_SETFD,FD_CLOEXEC)<0)error=errno;}
        else{int flags=real_fcntl(fd,F_GETFL,0);if(flags<0)error=errno;else if((flags&O_ACCMODE)!=O_RDONLY)error=EPERM;}
    }else{int flags=real_fcntl(fd,F_GETFL,0);if(flags<0)error=errno;else if((flags&O_ACCMODE)!=O_RDONLY)error=EPERM;}
    rwlock_unlock_write(&owner->rwlock);if(error){errno=error;return -1;}return 0;
}
