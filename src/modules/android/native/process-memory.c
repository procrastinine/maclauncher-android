#include "process-memory.h"
#include "process-ids.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>

enum { VECTOR_LIMIT=1024 };
static int vectors(const struct iovec *source,unsigned long count,struct iovec *result,size_t *length){
    *length=0;if(!count)return 0;
    mach_vm_size_t copied=0;
    if(!source||mach_vm_read_overwrite(mach_task_self(),(mach_vm_address_t)(uintptr_t)source,count*sizeof(*source),(mach_vm_address_t)(uintptr_t)result,&copied)!=KERN_SUCCESS||copied!=count*sizeof(*source)){errno=EFAULT;return -1;}
    for(unsigned long index=0;index<count;index++){
        if(result[index].iov_len>(size_t)SSIZE_MAX-*length){errno=EINVAL;return -1;}
        *length+=result[index].iov_len;
    }return 0;
}
static ssize_t transfer(int writing,pid_t id,const struct iovec *local,unsigned long local_count,const struct iovec *remote,unsigned long remote_count,unsigned long flags){
    if(flags||local_count>VECTOR_LIMIT||remote_count>VECTOR_LIMIT){errno=EINVAL;return -1;}
    struct iovec local_vectors[VECTOR_LIMIT],remote_vectors[VECTOR_LIMIT];size_t local_length,remote_length;
    if(vectors(local,local_count,local_vectors,&local_length)||vectors(remote,remote_count,remote_vectors,&remote_length))return -1;
    if(!local_length||!remote_length)return 0;
    pid_t physical;int kind,process;uid_t uid;
    if(id<=0||klj_process_signal_owner(id,&physical,&kind,&process,&uid)){errno=ESRCH;return -1;}
    pid_t caller_host;int caller_kind,caller_process;uid_t caller_uid;
    if(klj_process_signal_owner(klj_process_id(),&caller_host,&caller_kind,&caller_process,&caller_uid)){errno=ESRCH;return -1;}
    if(uid!=caller_uid){errno=EPERM;return -1;}
    task_t task=mach_task_self();int borrowed=physical==getpid();
    if(!borrowed){
        if(task_for_pid(mach_task_self(),physical,&task)!=KERN_SUCCESS){errno=EPERM;return -1;}
        // A task port must still name the same live enrolled principal after
        // acquisition. Never interpret an arbitrary physical Mac PID as an ID.
        pid_t current;int current_kind,current_process;uid_t current_uid;
        if(klj_process_signal_owner(id,&current,&current_kind,&current_process,&current_uid)||current!=physical||current_uid!=uid){mach_port_deallocate(mach_task_self(),task);errno=ESRCH;return -1;}
    }
    size_t li=0,ri=0,lo=0,ro=0,done=0;int failure=0;
    // The same ordered-vector/fault-safe Mach copy used by AIM's original
    // process_vm implementation; no direct dereference of caller buffers.
    while(li<local_count&&ri<remote_count){
        if(lo==local_vectors[li].iov_len){li++;lo=0;continue;}
        if(ro==remote_vectors[ri].iov_len){ri++;ro=0;continue;}
        size_t length=local_vectors[li].iov_len-lo,other=remote_vectors[ri].iov_len-ro;if(length>other)length=other;
        uintptr_t local_base=(uintptr_t)local_vectors[li].iov_base,remote_base=(uintptr_t)remote_vectors[ri].iov_base;
        if(local_base>UINTPTR_MAX-lo||remote_base>UINTPTR_MAX-ro||length>UINTPTR_MAX-(local_base+lo)||length>UINTPTR_MAX-(remote_base+ro)){failure=EFAULT;break;}
        kern_return_t result;mach_vm_size_t copied=0;
        if(writing&&!borrowed){
            if(length>UINT32_MAX)length=UINT32_MAX;
            // Validate local readability through the kernel as well. A remote
            // write consumes local bytes; mach_vm_write reads its OOL source.
            result=mach_vm_write(task,remote_base+ro,(vm_offset_t)(local_base+lo),(mach_msg_type_number_t)length);copied=result==KERN_SUCCESS?length:0;
        }else{
            mach_vm_address_t source=writing?local_base+lo:remote_base+ro,destination=writing?remote_base+ro:local_base+lo;
            result=mach_vm_read_overwrite(task,source,length,destination,&copied);
        }
        if(result!=KERN_SUCCESS||copied!=length){failure=EFAULT;break;}
        done+=length;lo+=length;ro+=length;
    }
    if(!borrowed)mach_port_deallocate(mach_task_self(),task);
    if(!done&&failure){errno=failure;return -1;}
    return (ssize_t)done;
}
ssize_t klj_process_vm_readv(pid_t id,const struct iovec *local,unsigned long local_count,const struct iovec *remote,unsigned long remote_count,unsigned long flags){return transfer(0,id,local,local_count,remote,remote_count,flags);}
ssize_t klj_process_vm_writev(pid_t id,const struct iovec *local,unsigned long local_count,const struct iovec *remote,unsigned long remote_count,unsigned long flags){return transfer(1,id,local,local_count,remote,remote_count,flags);}
