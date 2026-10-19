#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include "image-format.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Region bounds and permissions come from the executing memory owner. File
 * identity is a view of that owner's existing ELF/file metadata. */
typedef struct {
    uint64_t start,end,offset,device,inode;
    unsigned protection,shared;
    const char *path;
} KljProcMapRegion;
typedef struct {
    uint64_t start,end,offset,device,inode;
    char path[4096];
} KljProcMapFile;
typedef int (*KljProcMapLookup)(uint64_t,KljProcMapFile *);
typedef int (*KljProcMapWrite)(FILE *,void *);
int klj_proc_maps_self(const char *path,uint64_t process);
int klj_proc_maps_format(FILE *,const KljProcMapRegion *,size_t,KljProcMapLookup);
/* Replace one published inode atomically. Already-open descriptors retain
 * their immutable snapshot; no descriptor registry or retained old files. */
int klj_proc_maps_publish(const char *root,char *path,size_t,KljProcMapWrite,void *,uint64_t process);
/* -2: another descriptor/path; otherwise readlink's byte count or -1/errno. */
ssize_t klj_proc_maps_readlink(const char *,char *,size_t,uint64_t process);
int klj_proc_maps_elf_file(uint64_t base,const KlProgramHeader *,size_t,
                         uint64_t page,const char *file,const char *guest,
                         uint64_t address,KljProcMapFile *);
#if defined(__APPLE__)
int klj_proc_maps_native(const char *root,char *path,size_t,uint64_t process);
const char *klj_proc_maps_root(void);
int klj_proc_maps_image(uint64_t,KljProcMapFile *);
#endif
#ifdef __cplusplus
}
#endif
