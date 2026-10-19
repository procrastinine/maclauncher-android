#include <cutils/ashmem.h>
#include <linux/ashmem.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "../pollable-fds/bridge.h"
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

namespace {
extern "C" int klj_ashmem_initialize(int,const char*);
extern "C" int klj_ashmem_validate(int);
extern "C" int klj_ashmem_mapped(int,unsigned);
extern "C" int klj_ashmem_restrict(int,unsigned);
extern "C" int64_t klj_ashmem_capability_issue(int,const char*);
extern "C" int64_t klj_ashmem_capability_consume(int,const uint8_t*,size_t);
extern "C" void klj_ashmem_capability_release(int64_t);
extern "C" int klj_descriptor_access_adopt(int,int64_t);
bool checked(int result){if(result<0){errno=-result;return false;}return true;}
bool valid(int fd,struct stat& state) {
    if(fstat(fd,&state)<0)return false;
    return checked(klj_ashmem_validate(fd));
}
}

// Original AIM validates every requested protection bit and records the map
// before entering native mmap. The host JNI shares that same inode state.
extern "C" void* klj_ashmem_mmap(void* address,size_t size,int protection,int flags,int fd,off_t offset) {
    if(!checked(klj_ashmem_mapped(fd,(unsigned)protection)))return MAP_FAILED;
    return mmap(address,size,protection,flags,fd,offset);
}

extern "C" int ashmem_create_region(const char* regionName,size_t size) {
    if(size>INT_MAX){errno=EOVERFLOW;return -1;}
    // The runtime sets TMPDIR to its private process directory before launch.
    const char* directory=getenv("TMPDIR");
    if(!directory||!*directory){errno=ENOENT;return -1;}
    std::string pattern=std::string(directory)+"/binder-memory-XXXXXX";
    std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    int fd=mkstemp(name.data());
    if(fd<0)return -1;
    int readOnly=open(name.data(),O_RDONLY|O_CLOEXEC),openError=errno;
    bool authorized=checked(klj_ashmem_capability_issue(fd,name.data()));int accessError=errno;
    int result=unlink(name.data());
    if(readOnly<0||!authorized||result<0||fcntl(fd,F_SETFD,FD_CLOEXEC)<0||ftruncate(fd,size)<0||!checked(klj_ashmem_initialize(fd,regionName))){
        int error=readOnly<0?openError:!authorized?accessError:errno;if(readOnly>=0)close(readOnly);close(fd);errno=error;return -1;
    }
    if(klj_descriptor_readonly_adopt(fd,readOnly)<0){int error=errno;close(readOnly);close(fd);errno=error;return -1;}
    return fd;
}

extern "C" int klj_ashmem_capability_import(int fd,const uint8_t* token,size_t size){
    int64_t lease=klj_ashmem_capability_consume(fd,token,size);if(lease<0)return (int)lease;
    if(klj_descriptor_access_adopt(fd,lease)<0){int error=errno;klj_ashmem_capability_release(lease);return -error;}return 0;
}

extern "C" int ashmem_valid(int fd){struct stat state;return valid(fd,state);}
extern "C" int ashmem_get_size_region(int fd){struct stat state;if(!valid(fd,state))return -1;if(state.st_size>INT_MAX){errno=EOVERFLOW;return -1;}return state.st_size;}
// Darwin file-backed mappings retain their contents until unmapped. They have
// no Android low-memory purge state; validation and shared atomics remain owned
// by the original MemoryIntArray implementation.
extern "C" int ashmem_pin_region(int fd,size_t,size_t){struct stat state;return valid(fd,state)?ASHMEM_NOT_PURGED:-1;}
extern "C" int ashmem_unpin_region(int fd,size_t,size_t){struct stat state;return valid(fd,state)?ASHMEM_IS_UNPINNED:-1;}
extern "C" int ashmem_set_prot_region(int fd,int protection) {
    if((unsigned)protection&~(PROT_READ|PROT_WRITE|PROT_EXEC)){errno=EINVAL;return -1;}
    if(!checked(klj_ashmem_validate(fd)))return -1;
    if(protection&PROT_WRITE){
        return checked(klj_ashmem_restrict(fd,protection))?0:-1;
    }
    // Existing writable mappings remain valid for the original Parcel fill;
    // subsequent mappings and transferred descriptors are genuinely read-only.
    return checked(klj_ashmem_restrict(fd,protection))?klj_descriptor_readonly_seal(fd):-1;
}
extern "C" int klj_parcel_close(int fd) {
    return close(fd);
}
