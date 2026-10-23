/* Private to the original ELF image projection. Pools occupy separately
 * reserved outer space or genuine PT_LOAD gaps; original segment bytes and
 * relative virtual addresses are never moved or repurposed. */
static int klj_image_inline_pools(kl_image *image,const Elf64_Phdr *headers,int count,uint64_t low){
    image->inline_pools=calloc((size_t)count+2,sizeof *image->inline_pools);
    if(!image->inline_pools)return -1;
    uint8_t *mapping=image->base-image->map_off;
    image->inline_pools[0]=(KljInlinePool){mapping+image->map_size,image->x18_arena_size,0,0};
    image->inline_pools[1]=(KljInlinePool){mapping-image->x18_pre_arena_size,image->x18_pre_arena_size,0,0};
    image->inline_pool_count=2;
    if(!image->inline_prefix_size)return 0;
    uintptr_t page=(uintptr_t)getpagesize();
    for(int n=0;n<count;n++)if(headers[n].p_type==PT_LOAD){
        uint64_t start=headers[n].p_vaddr+headers[n].p_memsz,end=UINT64_MAX;
        if(start<headers[n].p_vaddr)continue;
        int contained=0;
        for(int k=0;k<count;k++)if(headers[k].p_type==PT_LOAD){
            uint64_t first=headers[k].p_vaddr,last=first+headers[k].p_memsz;
            if(last<first){contained=1;break;}
            if(first<start&&last>start){contained=1;break;}
            if(first>=start&&first<end)end=first;
        }
        if(contained||end==UINT64_MAX||end<=start||start<low||end-low>image->span)continue;
        uintptr_t first=(uintptr_t)image->base+(uintptr_t)(start-low),last=(uintptr_t)image->base+(uintptr_t)(end-low);
        first=(first+3)&~(uintptr_t)3;last&=~(uintptr_t)3;
        // Packed pools can share a host page with original read-only bytes,
        // as normal ELF RX/R segments do. Never grant X to a writable segment.
        for(int k=0;k<count;k++)if(headers[k].p_type==PT_LOAD&&(headers[k].p_flags&PF_W)){
            uintptr_t writable=(uintptr_t)image->base+(uintptr_t)(headers[k].p_vaddr-low);
            uintptr_t stop=writable+(uintptr_t)headers[k].p_memsz;
            writable&=~(page-1);stop=(stop+page-1)&~(page-1);
            if(writable<=first&&stop>first)first=stop;
            else if(writable<last&&stop>first)last=writable;
        }
        if(first>=last||last-first<32)continue;
        int duplicate=0;
        for(size_t k=2;k<image->inline_pool_count;k++)if(image->inline_pools[k].base==(void *)first)duplicate=1;
        if(!duplicate)image->inline_pools[image->inline_pool_count++]=(KljInlinePool){(void *)first,last-first,0,1};
    }
    return 0;
}
