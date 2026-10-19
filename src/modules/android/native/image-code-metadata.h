#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "instruction-context.h"

/* Borrowed ELF metadata, indexed once while the file mapping is alive. Marks
 * bound instruction neighborhoods; they never limit executable coverage. */
typedef klx_instruction_range KlImageDataRange;
typedef struct {
    uint64_t *marks; size_t count,capacity;
    uint64_t *roots; size_t rootCount,rootCapacity;
    KlImageDataRange *data; size_t dataCount,dataCapacity;
    KlImageDataRange *reachable; size_t reachableCount,reachableCapacity;
} KlImageCodeMetadata;
static inline void klj_code_metadata_release(KlImageCodeMetadata *m) {
    free(m->marks);free(m->roots);free(m->data);free(m->reachable);memset(m,0,sizeof *m);
}
static inline int klj_code_grow(void **items,size_t *capacity,size_t count,size_t width) {
    if(count<*capacity)return 1;
    size_t next=*capacity?*capacity*2:64;
    if(next<*capacity||next>SIZE_MAX/width)return 0;
    void *p=realloc(*items,next*width);if(!p)return 0;
    *items=p;*capacity=next;return 1;
}
static inline int klj_code_mark(KlImageCodeMetadata *m,uint64_t address) {
    if(address&3)return 1;
    if(!klj_code_grow((void **)&m->marks,&m->capacity,m->count,sizeof *m->marks))return 0;
    m->marks[m->count++]=address;return 1;
}
static inline int klj_code_root(KlImageCodeMetadata *m,uint64_t address) {
    if(address&3)return 1;
    if(!klj_code_grow((void **)&m->roots,&m->rootCapacity,m->rootCount,sizeof *m->roots))return 0;
    m->roots[m->rootCount++]=address;return klj_code_mark(m,address);
}
static inline int klj_code_data(KlImageCodeMetadata *m,uint64_t address,uint64_t size) {
    if(!size||address>UINT64_MAX-size)return 1;
    if(!klj_code_grow((void **)&m->data,&m->dataCapacity,m->dataCount,sizeof *m->data))return 0;
    m->data[m->dataCount++]=(KlImageDataRange){address,address+size};return 1;
}
static inline int klj_code_mark_cmp(const void *a,const void *b) {
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return x<y?-1:x>y;
}
static inline int klj_code_data_cmp(const void *a,const void *b) {
    return klj_code_mark_cmp(&((const KlImageDataRange *)a)->start,&((const KlImageDataRange *)b)->start);
}
static inline void klj_code_metadata_sort(KlImageCodeMetadata *m) {
    qsort(m->marks,m->count,sizeof *m->marks,klj_code_mark_cmp);
    size_t n=0;for(size_t i=0;i<m->count;i++)if(!n||m->marks[i]!=m->marks[n-1])m->marks[n++]=m->marks[i];m->count=n;
    qsort(m->data,m->dataCount,sizeof *m->data,klj_code_data_cmp);
    n=0;for(size_t i=0;i<m->dataCount;i++) {
        if(n&&m->data[i].start<=m->data[n-1].end) {if(m->data[i].end>m->data[n-1].end)m->data[n-1].end=m->data[i].end;}
        else m->data[n++]=m->data[i];
    }m->dataCount=n;
}
static inline const uint8_t *klj_code_address(const uint8_t *file,size_t size,const Elf64_Ehdr *eh,
                                      const Elf64_Phdr *ph,uint64_t address,size_t bytes) {
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD&&address>=ph[i].p_vaddr) {
        uint64_t delta=address-ph[i].p_vaddr;
        if(delta<=ph[i].p_filesz&&bytes<=ph[i].p_filesz-delta&&ph[i].p_offset<=size&&
           delta<=size-ph[i].p_offset&&bytes<=size-ph[i].p_offset-delta)return file+ph[i].p_offset+delta;
    }
    return NULL;
}
/* DW_EH_PE scalar encodings. Indirect pointers refer to runtime relocation
 * results, so those optional file records are left to the normal coverage. */
static inline int klj_code_encoded(const uint8_t **cursor,const uint8_t *end,unsigned encoding,
                            uint64_t pc,uint64_t dataBase,uint64_t textBase,uint64_t functionBase,uint64_t *value) {
    if(encoding==0xff||(encoding&0x80))return 0;
    const uint8_t *p=*cursor;uint64_t v=0;unsigned form=encoding&15,width=0;
    if((encoding&0x70)==0x50) {size_t padding=(size_t)(-pc)&7;if((size_t)(end-p)<padding)return 0;p+=padding;pc+=padding;}
    switch(form){case 0:case 4:case 12:width=8;break;case 2:case 10:width=2;break;case 3:case 11:width=4;break;
    case 1:case 9:{unsigned shift=0;uint8_t byte;do{if(p==end||shift>=64)return 0;byte=*p++;v|=(uint64_t)(byte&127)<<shift;shift+=7;}while(byte&128);if(form==9&&shift<64&&(byte&64))v|=UINT64_MAX<<shift;break;}
    default:return 0;}
    if(width){if((size_t)(end-p)<width)return 0;memcpy(&v,p,width);p+=width;if(form&8){if(width==2)v=(uint64_t)(int64_t)(int16_t)v;if(width==4)v=(uint64_t)(int64_t)(int32_t)v;}}
    switch(encoding&0x70){case 0:case 0x50:break;case 0x10:v+=pc;break;case 0x20:v+=textBase;break;case 0x30:v+=dataBase;break;case 0x40:v+=functionBase;break;default:return 0;}
    *cursor=p;*value=v;return 1;
}
static inline int klj_code_record(KlImageCodeMetadata *m,const uint8_t *file,size_t size,
                           const Elf64_Ehdr *eh,const Elf64_Phdr *ph,uint64_t address,int fde) {
    const uint8_t *p=klj_code_address(file,size,eh,ph,address,4);if(!p)return 1;
    uint32_t shortSize;memcpy(&shortSize,p,4);uint64_t length=shortSize;size_t prefix=4,idWidth=4;
    if(shortSize==UINT32_MAX){p=klj_code_address(file,size,eh,ph,address,12);if(!p)return 1;memcpy(&length,p+4,8);prefix=12;idWidth=8;}
    if(length<idWidth||length>SIZE_MAX-prefix)return 1;
    p=klj_code_address(file,size,eh,ph,address,(size_t)length+prefix);if(!p)return 1;
    if(!klj_code_data(m,address,length+prefix))return 0;
    if(fde){uint64_t offset=0;memcpy(&offset,p+prefix,idWidth);if(offset&&offset<=address+prefix)
        return klj_code_record(m,file,size,eh,ph,address+prefix-offset,0);}
    return 1;
}
typedef struct { uint64_t address; unsigned section,data; } KlImageMapping;
static inline int klj_code_mapping_cmp(const void *a,const void *b) {
    const KlImageMapping *x=a,*y=b;
    if(x->section!=y->section)return x->section<y->section?-1:1;
    return x->address<y->address?-1:x->address>y->address;
}
static inline int klj_code_symbols(KlImageCodeMetadata *m,const uint8_t *file,size_t size,
                                   const Elf64_Ehdr *eh,const Elf64_Shdr *sh) {
    KlImageMapping *mapping=NULL;size_t count=0,capacity=0;int result=0;
    for(unsigned i=0;sh&&i<eh->e_shnum;i++) {
        if(sh[i].sh_type!=2&&sh[i].sh_type!=SHT_DYNSYM)continue;
        if(sh[i].sh_entsize<sizeof(Elf64_Sym)||sh[i].sh_offset>size||sh[i].sh_size>size-sh[i].sh_offset)continue;
        const Elf64_Shdr *strings=sh[i].sh_link<eh->e_shnum?&sh[sh[i].sh_link]:NULL;
        if(strings&&(strings->sh_offset>size||strings->sh_size>size-strings->sh_offset))strings=NULL;
        for(uint64_t at=0;at+sizeof(Elf64_Sym)<=sh[i].sh_size;at+=sh[i].sh_entsize) {
            Elf64_Sym sym;memcpy(&sym,file+sh[i].sh_offset+at,sizeof sym);
            if(!sym.st_shndx||sym.st_shndx>=eh->e_shnum||!(sh[sym.st_shndx].sh_flags&SHF_EXECINSTR))continue;
            const Elf64_Shdr *section=&sh[sym.st_shndx];
            if(sym.st_value<section->sh_addr||sym.st_value-section->sh_addr>section->sh_size||
               sym.st_size>section->sh_size-(sym.st_value-section->sh_addr))continue;
            if(sh[i].sh_type==2&&strings&&sym.st_name<strings->sh_size) {
                const char *name=(const char *)file+strings->sh_offset+sym.st_name;
                size_t left=(size_t)(strings->sh_size-sym.st_name);
                if(left>=3&&name[0]=='$'&&(name[1]=='d'||name[1]=='x')&&(name[2]==0||name[2]=='.')) {
                    if(!klj_code_grow((void**)&mapping,&capacity,count,sizeof *mapping))goto done;
                    mapping[count++]=(KlImageMapping){sym.st_value,sym.st_shndx,name[1]=='d'};
                    continue;
                }
            }
            if((sym.st_info&15)==2||(sym.st_info&15)==10) {
                if(!klj_code_root(m,sym.st_value))goto done;
                if(sym.st_size&&sym.st_value<=UINT64_MAX-sym.st_size&&!klj_code_mark(m,sym.st_value+sym.st_size))goto done;
            }else if((sym.st_info&15)==1&&!klj_code_data(m,sym.st_value,sym.st_size))goto done;
        }
    }
    qsort(mapping,count,sizeof *mapping,klj_code_mapping_cmp);
    for(size_t i=0;i<count;i++) {
        const KlImageMapping *item=&mapping[i];const Elf64_Shdr *section=&sh[item->section];
        uint64_t end=i+1<count&&mapping[i+1].section==item->section?mapping[i+1].address:section->sh_addr+section->sh_size;
        if(item->data){if(!klj_code_data(m,item->address,end-item->address))goto done;}
        else if(!klj_code_root(m,item->address)||!klj_code_mark(m,end))goto done;
    }
    result=1;
done:free(mapping);return result;
}
static inline int klj_code_metadata_read(KlImageCodeMetadata *m,const uint8_t *file,size_t size,
                                  const Elf64_Ehdr *eh,const Elf64_Phdr *ph,const Elf64_Shdr *sh) {
    uint64_t textBase=0;for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD&&(ph[i].p_flags&PF_X)){textBase=ph[i].p_vaddr;break;}
    if(eh->e_entry&&!klj_code_root(m,eh->e_entry))return 0;
    if(!klj_code_symbols(m,file,size,eh,sh))return 0;
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==0x6474e550&&ph[i].p_filesz>=4&&
        ph[i].p_offset<=size&&ph[i].p_filesz<=size-ph[i].p_offset) {
        const uint8_t *begin=file+ph[i].p_offset,*p=begin+4,*end=begin+ph[i].p_filesz;
        if(begin[0]!=1)continue;
        uint64_t frame,count;unsigned enc=begin[3];
        if(!klj_code_encoded(&p,end,begin[1],ph[i].p_vaddr+4,ph[i].p_vaddr,textBase,0,&frame)||
           !klj_code_encoded(&p,end,begin[2],ph[i].p_vaddr+(p-begin),ph[i].p_vaddr,textBase,0,&count))continue;
        if(!klj_code_record(m,file,size,eh,ph,frame,0))return 0;
        for(uint64_t j=0;j<count;j++) {
            uint64_t start,record;
            if(!klj_code_encoded(&p,end,enc,ph[i].p_vaddr+(p-begin),ph[i].p_vaddr,textBase,0,&start)||
               !klj_code_encoded(&p,end,enc,ph[i].p_vaddr+(p-begin),ph[i].p_vaddr,textBase,start,&record))break;
            if(!klj_code_root(m,start)||!klj_code_record(m,file,size,eh,ph,record,1))return 0;
        }
    }
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD&&(ph[i].p_flags&PF_X)&&
        ph[i].p_offset<=size&&ph[i].p_filesz<=size-ph[i].p_offset) {
        uint64_t offset=(-ph[i].p_vaddr)&3u;
        for(;offset<=ph[i].p_filesz&&ph[i].p_filesz-offset>=8;offset+=4) {
            uint32_t save,frame;memcpy(&save,file+ph[i].p_offset+offset,4);memcpy(&frame,file+ph[i].p_offset+offset+4,4);
            if(klj_code_frame_record(save,frame)&&!klj_code_root(m,ph[i].p_vaddr+offset))return 0;
        }
    }
    klj_code_metadata_sort(m);
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD&&(ph[i].p_flags&PF_X)&&
        ph[i].p_offset<=size&&ph[i].p_filesz<=size-ph[i].p_offset) {
        uint64_t skip=(-ph[i].p_vaddr)&3u;if(skip>ph[i].p_filesz)continue;
        size_t n=(size_t)((ph[i].p_filesz-skip)/4);uint64_t base=ph[i].p_vaddr+skip;
        uint8_t *state=calloc(n?n:1,1);if(!state)return 0;
        for(size_t k=0;k<m->dataCount;k++) {
            uint64_t lo=m->data[k].start>base?m->data[k].start:base;
            uint64_t hi=m->data[k].end<base+n*4?m->data[k].end:base+n*4;
            for(uint64_t a=(lo+3)&~3ULL;a<hi;a+=4)state[(a-base)/4]=2;
        }
        int result=klx_instruction_evidence((const uint32_t *)(file+ph[i].p_offset+skip),n,base,m->roots,m->rootCount,state);
        if(result){free(state);return 0;}
        for(size_t k=0;k<n;) {
            if(!(state[k]&2)){k++;continue;}size_t begin=k;while(k<n&&(state[k]&2))k++;
            if(!klj_code_data(m,base+begin*4,(k-begin)*4)){free(state);return 0;}
        }
        for(size_t k=0;k<n;) {
            if(!(state[k]&1)){k++;continue;}size_t begin=k;while(k<n&&(state[k]&1))k++;
            if(!klj_code_grow((void **)&m->reachable,&m->reachableCapacity,m->reachableCount,sizeof *m->reachable)){free(state);return 0;}
            m->reachable[m->reachableCount++]=(KlImageDataRange){base+begin*4,base+k*4};
        }
        free(state);
    }
    qsort(m->reachable,m->reachableCount,sizeof *m->reachable,klj_code_data_cmp);
    klj_code_metadata_sort(m);return 1;
}
