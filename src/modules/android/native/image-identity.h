#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* Read immutable identity from the original file, before constructors may
 * replace its mapped dynamic tables. Both execution formats keep the name
 * under their existing kl_image owner; no alias image is registered. */
static uint64_t klj_identity_word(const unsigned char *bytes,unsigned width) {
    uint64_t value=0;
    for(unsigned i=0;i<width;i++)value|=(uint64_t)bytes[i]<<(i*8);
    return value;
}
static int klj_identity_range(uint64_t offset,uint64_t length,size_t size) {
    return offset<=size && length<=size-offset;
}
static char *klj_elf_soname_bytes(const unsigned char *bytes,size_t size) {
    if(size<52 || memcmp(bytes,"\177ELF",4) || bytes[5]!=1 ||
       (bytes[4]!=1 && bytes[4]!=2))return NULL;
    unsigned width=bytes[4]==2?8:4;
    size_t header=width==8?64:52,phsize=width==8?56:32;
    if(size<header)return NULL;
    uint64_t phoff=klj_identity_word(bytes+(width==8?32:28),width);
    uint64_t stride=klj_identity_word(bytes+(width==8?54:42),2);
    uint64_t count=klj_identity_word(bytes+(width==8?56:44),2);
    if(stride<phsize || !klj_identity_range(phoff,count*stride,size))return NULL;
    uint64_t strings=0,string_size=0,name=UINT64_MAX;
    for(uint64_t i=0;i<count;i++) {
        const unsigned char *ph=bytes+phoff+i*stride;
        if(klj_identity_word(ph,4)!=2)continue; /* PT_DYNAMIC */
        uint64_t offset=klj_identity_word(ph+(width==8?8:4),width);
        uint64_t length=klj_identity_word(ph+(width==8?32:16),width);
        if(!klj_identity_range(offset,length,size))return NULL;
        for(uint64_t at=0;at+2*width<=length;at+=2*width) {
            uint64_t tag=klj_identity_word(bytes+offset+at,width);
            uint64_t value=klj_identity_word(bytes+offset+at+width,width);
            if(!tag)break;
            if(tag==5)strings=value;      /* DT_STRTAB */
            if(tag==10)string_size=value; /* DT_STRSZ */
            if(tag==14)name=value;        /* DT_SONAME */
        }
    }
    if(name>=string_size)return NULL;
    for(uint64_t i=0;i<count;i++) {
        const unsigned char *ph=bytes+phoff+i*stride;
        if(klj_identity_word(ph,4)!=1)continue; /* PT_LOAD */
        uint64_t offset=klj_identity_word(ph+(width==8?8:4),width);
        uint64_t address=klj_identity_word(ph+(width==8?16:8),width);
        uint64_t length=klj_identity_word(ph+(width==8?32:16),width);
        if(strings<address || strings-address>length || string_size>length-(strings-address) ||
           !klj_identity_range(offset,length,size))continue;
        const char *start=(const char *)bytes+offset+(strings-address)+name;
        const char *end=memchr(start,0,(size_t)(string_size-name));
        if(!end || end==start)return NULL;
        size_t size=(size_t)(end-start)+1;
        char *copy=malloc(size);
        if(copy)memcpy(copy,start,size);
        return copy;
    }
    return NULL;
}
static char *klj_elf_soname(const char *path) {
    int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return NULL;
    struct stat st;
    if(fstat(fd,&st) || st.st_size<=0 || (uint64_t)st.st_size>SIZE_MAX){close(fd);return NULL;}
    size_t size=(size_t)st.st_size;
    void *bytes=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
    if(bytes==MAP_FAILED)return NULL;
    char *name=klj_elf_soname_bytes(bytes,size);munmap(bytes,size);return name;
}
