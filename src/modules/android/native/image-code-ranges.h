#pragma once
#include <stdint.h>
#include <stddef.h>
#include "image-code-metadata.h"

/* ELF program headers own executable mappings. Section headers describe data
 * inside them, but may omit executable prefixes, suffixes, or whole functions.
 * One iterator supplies both veneer allocation and instruction adaptation. */
typedef struct { unsigned segment; uint64_t address; const KlImageCodeMetadata *metadata; } KlImageCodeCursor;
typedef struct { uint64_t address, offset; size_t size; } KlImageCodeRange;

static void klj_code_exclude(uint64_t start,uint64_t size,uint64_t cursor,
                             uint64_t *next,uint64_t *skip) {
    if(!size || start>UINT64_MAX-size)return;
    uint64_t end=start+size;
    if(start<=cursor && cursor<end) { if(end>*skip)*skip=end; }
    else if(cursor<start && start<*next)*next=start;
}

static void klj_code_exclude_file(const Elf64_Phdr *segment,uint64_t offset,
                                  uint64_t size,uint64_t cursor,
                                  uint64_t *next,uint64_t *skip) {
    if(!size || offset>UINT64_MAX-size)return;
    uint64_t end=offset+size,segmentEnd=segment->p_offset+segment->p_filesz;
    if(offset<segment->p_offset)offset=segment->p_offset;
    if(end>segmentEnd)end=segmentEnd;
    if(offset<end)klj_code_exclude(segment->p_vaddr+offset-segment->p_offset,
                                  end-offset,cursor,next,skip);
}

static int klj_image_code_next(const Elf64_Ehdr *header,const Elf64_Phdr *segments,
                               const Elf64_Shdr *sections,size_t fileSize,
                               KlImageCodeCursor *cursor,KlImageCodeRange *range) {
    while(cursor->segment<header->e_phnum) {
        const Elf64_Phdr *segment=&segments[cursor->segment];
        if(segment->p_type!=PT_LOAD || !(segment->p_flags&PF_X) ||
           segment->p_offset>fileSize || segment->p_filesz>fileSize-segment->p_offset ||
           segment->p_vaddr>UINT64_MAX-segment->p_filesz) {
            cursor->segment++;cursor->address=0;continue;
        }
        uint64_t end=segment->p_vaddr+segment->p_filesz;
        uint64_t start=cursor->address>segment->p_vaddr?cursor->address:segment->p_vaddr;
        if(start>UINT64_MAX-3) {cursor->segment++;cursor->address=0;continue;}
        start=(start+3)&~UINT64_C(3);
        if(start>=end) {cursor->segment++;cursor->address=0;continue;}
        uint64_t next=end,skip=start;
        /* Typed ELF unwind/object records remain data even when their section
         * names were removed. The sorted intervals are shared by all passes. */
        const KlImageCodeMetadata *metadata=cursor->metadata;
        if(metadata&&metadata->dataCount) {
            size_t lo=0,hi=metadata->dataCount;
            while(lo<hi){size_t mid=lo+(hi-lo)/2;if(metadata->data[mid].end<=start)lo=mid+1;else hi=mid;}
            if(lo<metadata->dataCount)klj_code_exclude(metadata->data[lo].start,
                metadata->data[lo].end-metadata->data[lo].start,start,&next,&skip);
        }
        /* Preserve declared non-code, even when it resides in an executable
         * segment. Non-allocated debug sections do not describe mapped bytes. */
        for(unsigned i=0;sections && i<header->e_shnum;i++) {
            const Elf64_Shdr *section=&sections[i];
            if((section->sh_flags&2) && !(section->sh_flags&SHF_EXECINSTR))
                klj_code_exclude(section->sh_addr,section->sh_size,start,&next,&skip);
        }
        /* ELF tables and typed metadata segments remain data when section
         * headers have been stripped or shortened by a packaging tool. */
        klj_code_exclude_file(segment,0,header->e_ehsize,start,&next,&skip);
        klj_code_exclude_file(segment,header->e_phoff,
            (uint64_t)header->e_phnum*header->e_phentsize,start,&next,&skip);
        klj_code_exclude_file(segment,header->e_shoff,
            (uint64_t)header->e_shnum*header->e_shentsize,start,&next,&skip);
        for(unsigned i=0;i<header->e_phnum;i++) {
            const Elf64_Phdr *metadata=&segments[i];
            if(metadata->p_type==PT_DYNAMIC || metadata->p_type==4 ||
               metadata->p_type==6 || metadata->p_type==0x6474e550 ||
               metadata->p_type==PT_GNU_RELRO)
                klj_code_exclude(metadata->p_vaddr,metadata->p_filesz,start,&next,&skip);
        }
        if(skip>start) {cursor->address=skip;continue;}
        cursor->address=next;
        size_t size=(size_t)((next-start)&~UINT64_C(3));
        if(!size)continue;
        *range=(KlImageCodeRange){start,segment->p_offset+start-segment->p_vaddr,size};
        return 1;
    }
    return 0;
}
