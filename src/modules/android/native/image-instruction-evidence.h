#pragma once
#include "image-code-ranges.h"

/* File-offset evidence for the offline and load-time bindings. The same ELF
 * metadata, segment iterator and decoder context used by the native loader
 * make the decisions; this entry point only serializes their result. */
int klx_elf_instruction_evidence(const void *source,size_t size,uint8_t *state,size_t count) {
    const uint8_t *file=source;
    if(!source||!state||count!=size/4+(size%4!=0)||size<sizeof(Elf64_Ehdr))return -1;
    Elf64_Ehdr header;memcpy(&header,file,sizeof header);
    if(memcmp(header.e_ident,"\177ELF\2\1",6)||header.e_machine!=183||
       header.e_phentsize!=sizeof(Elf64_Phdr)||header.e_phoff>size||
       (size_t)header.e_phnum>(size-header.e_phoff)/sizeof(Elf64_Phdr)||
       (header.e_shnum&&(header.e_shentsize!=sizeof(Elf64_Shdr)||header.e_shoff>size||
        (size_t)header.e_shnum>(size-header.e_shoff)/sizeof(Elf64_Shdr))))return -1;
    /* Valid ELF64 tables are aligned. Copies also support safe inspection of
     * unaligned caller buffers without borrowing typed pointers from them. */
    Elf64_Phdr *segments=malloc((header.e_phnum?header.e_phnum:1)*sizeof *segments);
    Elf64_Shdr *sections=header.e_shnum?malloc(header.e_shnum*sizeof *sections):NULL;
    if(!segments||(header.e_shnum&&!sections)){free(segments);free(sections);return -1;}
    memcpy(segments,file+header.e_phoff,header.e_phnum*sizeof *segments);
    if(sections)memcpy(sections,file+header.e_shoff,header.e_shnum*sizeof *sections);
    KlImageCodeMetadata metadata={0};int result=-1;
    memset(state,0,count);
    if(!klj_code_metadata_read(&metadata,file,size,&header,segments,sections))goto done;
    KlImageCodeCursor cursor={.metadata=&metadata};KlImageCodeRange range;
    while(klj_image_code_next(&header,segments,sections,size,&cursor,&range)) {
        const uint32_t *words=(const uint32_t *)(file+range.offset);
        klx_code_context context={metadata.marks,metadata.count,range.address,
                                  metadata.reachable,metadata.reachableCount};
        size_t reachable=0;
        for(size_t i=0;i<range.size/4;i++) {
            uint64_t address=range.address+i*4;
            while(reachable<metadata.reachableCount&&metadata.reachable[reachable].end<=address)reachable++;
            int code=reachable<metadata.reachableCount&&metadata.reachable[reachable].start<=address;
            state[(range.offset+i*4)/4]=(uint8_t)(klj_x18_data_context(words,range.size/4,i,&context)?2:1)|(code?4:0);
        }
    }
    result=0;
done:
    klj_code_metadata_release(&metadata);free(segments);free(sections);return result;
}
