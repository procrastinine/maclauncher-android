/* Linux maps formatting follows AIM's sys/procfs.rs; the native region walk
 * follows its sys/vmmap.rs and queries only the current physical process. */
#include "proc-maps.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

int klj_proc_maps_self(const char *path,uint64_t process){
    if(!path)return 0;
    if(!strcmp(path,"/proc/self/maps")||!strcmp(path,"/proc/thread-self/maps"))return 1;
    if(strncmp(path,"/proc/",6)||path[6]<'0'||path[6]>'9')return 0;
    uint64_t number=0;const char *at=path+6;
    while(*at>='0'&&*at<='9'){
        unsigned digit=(unsigned)(*at++-'0');
        if(number>(UINT64_MAX-digit)/10)return 0;
        number=number*10+digit;
    }
    return process&&number==process&&!strcmp(at,"/maps");
}
static int map_line(FILE *file,const KljProcMapRegion *region,const char *path){
    int length=fprintf(file,"%08" PRIx64 "-%08" PRIx64 " %c%c%c%c %08" PRIx64 " %02" PRIx64 ":%02" PRIx64 " %" PRIu64,
        region->start,region->end,region->protection&1?'r':'-',
        region->protection&2?'w':'-',region->protection&4?'x':'-',
        region->shared?'s':'p',region->offset,(region->device>>24)&0xff,
        region->device&0xffffff,region->inode);
    if(length<0)return 0;
    if(path&&*path){
        int spaces=length<73?73-length:1;
        while(spaces--)if(fputc(' ',file)==EOF)return 0;
        for(const unsigned char *at=(const unsigned char *)path;*at;at++){
            if(*at=='\n'){if(fputs("\\012",file)==EOF)return 0;}
            else if(fputc(*at,file)==EOF)return 0;
        }
    }
    return fputc('\n',file)!=EOF;
}
int klj_proc_maps_format(FILE *file,const KljProcMapRegion *regions,size_t count,KljProcMapLookup lookup){
    for(size_t index=0;index<count;index++){
        const KljProcMapRegion *source=&regions[index];
        if(source->start>=source->end){errno=EINVAL;return 0;}
        uint64_t cursor=source->start;
        while(cursor<source->end){
            KljProcMapRegion part=*source;part.start=cursor;
            if(part.path&&*part.path)part.offset+=cursor-source->start;
            KljProcMapFile image={0};
            if(lookup&&lookup(cursor,&image)&&image.end>cursor&&image.start<source->end){
                if(image.start>cursor)part.end=image.start;
                else{
                    if(image.end<part.end)part.end=image.end;
                    part.offset=image.offset+cursor-image.start;
                    part.device=image.device;part.inode=image.inode;part.path=image.path;
                }
            }
            if(!map_line(file,&part,part.path))return 0;
            cursor=part.end;
        }
    }
    return !ferror(file);
}
/* AIM's existing Linux user-xattr namespace has the same physical spelling
 * when this unit executes inside Bionic. This inode metadata survives native
 * dup/fork/exec without retaining another descriptor table. */
#if defined(__APPLE__)
#define PROC_ORIGIN "dev.aim.xattr.user.maclauncher.proc-origin"
#else
#define PROC_ORIGIN "user.maclauncher.proc-origin"
#endif
ssize_t klj_proc_maps_readlink(const char *name,char *buffer,size_t capacity,uint64_t process){
    if(!name||strncmp(name,"/proc/",6))return -2;
    const char *tail=name+6;
    if(!strncmp(tail,"self/",5))tail+=5;
    else if(!strncmp(tail,"thread-self/",12))tail+=12;
    else{
        uint64_t id=0;const char *start=tail;
        while(*tail>='0'&&*tail<='9'){unsigned digit=(unsigned)(*tail++-'0');if(id>(UINT64_MAX-digit)/10)return -2;id=id*10+digit;}
        if(tail==start||*tail++!='/'||id!=(process?process:(uint64_t)getpid()))return -2;
    }
    if(strncmp(tail,"fd/",3))return -2;tail+=3;
    unsigned descriptor=0;const char *start=tail;
    while(*tail>='0'&&*tail<='9'){unsigned digit=(unsigned)(*tail++-'0');if(descriptor>((unsigned)INT_MAX-digit)/10)return -2;descriptor=descriptor*10+digit;}
    if(tail==start||*tail)return -2;
    char origin[96];
#if defined(__APPLE__)
    ssize_t count=fgetxattr((int)descriptor,PROC_ORIGIN,origin,sizeof origin-1,0,0);
#else
    ssize_t count=fgetxattr((int)descriptor,PROC_ORIGIN,origin,sizeof origin-1);
#endif
    if(count<0){
#if defined(__APPLE__)
        if(errno==ENOATTR||errno==ENOTSUP)return -2;
#else
        if(errno==ENODATA||errno==ENOTSUP)return -2;
#endif
        return -1;
    }
    origin[count]=0;
    if(count<12||strncmp(origin,"/proc/",6)||memchr(origin,0,(size_t)count)){errno=EIO;return -1;}
    if(!capacity){errno=EINVAL;return -1;}
    if(!buffer){errno=EFAULT;return -1;}
    size_t length=(size_t)count<capacity?(size_t)count:capacity;
    memcpy(buffer,origin,length);return (ssize_t)length;
}
int klj_proc_maps_publish(const char *root,char *path,size_t capacity,KljProcMapWrite write,void *context,uint64_t process){
    if(!root||!*root||!path||!capacity||!write){errno=EINVAL;return 0;}
    char directory[PATH_MAX],temporary[PATH_MAX];
    int n=snprintf(directory,sizeof directory,"%s/proc",root);
    if(n<0||(size_t)n>=sizeof directory){errno=ENAMETOOLONG;return 0;}
    if(mkdir(directory,0700)&&errno!=EEXIST)return 0;
    n=snprintf(directory,sizeof directory,"%s/proc/self",root);
    if(n<0||(size_t)n>=sizeof directory){errno=ENAMETOOLONG;return 0;}
    if(mkdir(directory,0700)&&errno!=EEXIST)return 0;
    n=snprintf(path,capacity,"%s/maps",directory);
    if(n<0||(size_t)n>=capacity){errno=ENAMETOOLONG;return 0;}
    n=snprintf(temporary,sizeof temporary,"%s/.maps.XXXXXX",directory);
    if(n<0||(size_t)n>=sizeof temporary){errno=ENAMETOOLONG;return 0;}
    int descriptor=mkstemp(temporary);if(descriptor<0)return 0;
    if(fcntl(descriptor,F_SETFD,FD_CLOEXEC)){int error=errno;close(descriptor);unlink(temporary);errno=error;return 0;}
    FILE *file=fdopen(descriptor,"w");
    if(!file){int error=errno;close(descriptor);unlink(temporary);errno=error;return 0;}
    errno=0;int ok=write(file,context),error=errno;
    if(ok&&fflush(file)){ok=0;error=errno;}
    if(ok){
        char origin[96];int size=snprintf(origin,sizeof origin,"/proc/%" PRIu64 "/maps",process?process:(uint64_t)getpid());
#if defined(__APPLE__)
        int tagged=fsetxattr(descriptor,PROC_ORIGIN,origin,(size_t)size,0,0);
#else
        int tagged=fsetxattr(descriptor,PROC_ORIGIN,origin,(size_t)size,0);
#endif
        if(tagged){ok=0;error=errno;}
    }
    if(ok&&fchmod(descriptor,0444)){ok=0;error=errno;}
    if(fclose(file)&&ok){ok=0;error=errno;}
    if(ok&&rename(temporary,path)){ok=0;error=errno;}
    if(!ok){unlink(temporary);errno=error?error:EIO;}
    return ok;
}
int klj_proc_maps_elf_file(uint64_t base,const KlProgramHeader *headers,size_t count,uint64_t page,
                          const char *file,const char *guest,uint64_t address,KljProcMapFile *result){
    if(!page||(page&(page-1))||!file||!guest||!*guest)return 0;
    int found=0;uint64_t best=UINT64_MAX;
    for(size_t index=0;index<count;index++){
        const KlProgramHeader *header=&headers[index];
        if(header->type!=1||!header->filesz||header->vaddr>UINT64_MAX-header->filesz)continue;
        uint64_t start=header->vaddr&~(page-1),end=header->vaddr+header->filesz;
        if(end>UINT64_MAX-(page-1)||base>UINT64_MAX-start)continue;
        end=(end+page-1)&~(page-1);
        if(base>UINT64_MAX-end)continue;
        start+=base;end+=base;
        if(end<=address)continue;
        uint64_t candidate=start>address?start:address;
        if(candidate>=best)continue;
        size_t length=strlen(guest);if(length>=sizeof result->path)continue;
        struct stat status={0};if(address>=start&&address<end)(void)stat(file,&status);
        *result=(KljProcMapFile){.start=start,.end=end,.offset=header->offset&~(page-1),
            .device=(uint64_t)(uint32_t)status.st_dev,.inode=(uint64_t)status.st_ino};
        memcpy(result->path,guest,length+1);best=candidate;found=1;
    }
    return found;
}

#if defined(__APPLE__) && !defined(KLJ_PROC_MAPS_FORMAT_ONLY)
#include "guest-paths.h"
#include <libproc.h>
#include <mach/vm_inherit.h>
#include <mach/vm_statistics.h>
typedef struct {KljProcMapRegion *regions;size_t count,capacity;KljGuestPathView *paths;} NativeSnapshot;
static void native_release(NativeSnapshot *snapshot){
    for(size_t i=0;i<snapshot->count;i++)free((void *)snapshot->regions[i].path);
    free(snapshot->regions);
    klj_guest_path_view_release(snapshot->paths);
}
static int native_snapshot(NativeSnapshot *snapshot){
    snapshot->paths=klj_guest_path_view();if(!snapshot->paths)return 0;
    uint64_t cursor=0;
    for(;;){
        struct proc_regionwithpathinfo entry={0};
        int size=proc_pidinfo(getpid(),PROC_PIDREGIONPATHINFO,cursor,&entry,sizeof entry);
        if(size<(int)sizeof entry){
            if(!snapshot->count)return 0;
            break;
        }
        const struct proc_regioninfo *info=&entry.prp_prinfo;
        if(!info->pri_size||info->pri_size>UINT64_MAX-info->pri_address){errno=EOVERFLOW;return 0;}
        uint64_t end=info->pri_address+info->pri_size;
        if(end<=cursor){errno=EIO;return 0;}
        if(snapshot->count==snapshot->capacity){
            size_t capacity=snapshot->capacity?snapshot->capacity*2:128;
            if(capacity<snapshot->capacity||capacity>SIZE_MAX/sizeof(KljProcMapRegion)){errno=ENOMEM;return 0;}
            void *next=realloc(snapshot->regions,capacity*sizeof(KljProcMapRegion));if(!next)return 0;
            snapshot->regions=(KljProcMapRegion *)next;snapshot->capacity=capacity;
        }
        KljProcMapRegion region={.start=info->pri_address,.end=end,.protection=info->pri_protection,
            .shared=info->pri_inheritance==VM_INHERIT_SHARE};
        char guest[PATH_MAX];
        if(entry.prp_vip.vip_path[0]&&klj_guest_path_view_name(snapshot->paths,entry.prp_vip.vip_path,guest,sizeof guest)){
            region.path=strdup(guest);if(!region.path)return 0;
            region.offset=info->pri_offset;
            region.device=(uint64_t)(uint32_t)entry.prp_vip.vip_vi.vi_stat.vst_dev;
            region.inode=entry.prp_vip.vip_vi.vi_stat.vst_ino;
        }
        snapshot->regions[snapshot->count++]=region;cursor=end;
    }
    return 1;
}
static int native_write(FILE *file,void *context){
    NativeSnapshot *snapshot=(NativeSnapshot *)context;
    return klj_proc_maps_format(file,snapshot->regions,snapshot->count,klj_proc_maps_image);
}
int klj_proc_maps_native(const char *root,char *path,size_t capacity,uint64_t process){
    NativeSnapshot snapshot={0};int ok=native_snapshot(&snapshot),error=errno;
    if(ok){ok=klj_proc_maps_publish(root,path,capacity,native_write,&snapshot,process);error=errno;}
    native_release(&snapshot);errno=error;return ok;
}
#endif
