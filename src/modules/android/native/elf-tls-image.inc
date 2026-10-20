/* Included only in the owned ELF loader projection, after its private image
 * and symbol definitions. ELF symbol values for STT_TLS are block offsets;
 * adding the image bias would instead expose unrelated mapped bytes. */
struct klj_elf_tls_descriptor_node {
    KljElfTlsIndex index;
    struct klj_elf_tls_descriptor_node *next;
};
static int klj_image_tls_register(kl_image *image,const Elf64_Phdr *headers,int count,uint64_t low){
    for(int n=0;n<count;n++)if(headers[n].p_type==7 /* PT_TLS */){
        const Elf64_Phdr *header=&headers[n];
        uint64_t alignment=header->p_align?header->p_align:1;
        if(image->tls_module||header->p_filesz>header->p_memsz||
           header->p_vaddr<low||header->p_vaddr-low>image->span||
           header->p_filesz>image->span-(header->p_vaddr-low)||
           (alignment&(alignment-1))||header->p_memsz>SIZE_MAX){
            err("Invalid ELF TLS segment");return -1;
        }
        image->tls_module=klj_elf_tls_register(image->base+header->p_vaddr-low,
            header->p_filesz,header->p_memsz,alignment,header->p_vaddr&(alignment-1));
        if(!image->tls_module){err("ELF TLS registration: %s",strerror(errno));return -1;}
    }
    return 0;
}
static void klj_image_tls_unregister(kl_image *image){
    struct klj_elf_tls_descriptor_node *node=image->tls_descriptors;
    while(node){struct klj_elf_tls_descriptor_node *next=node->next;free(node);node=next;}
    klj_elf_tls_unregister(image->tls_module);
}
int klj_image_tls_symbol(kl_image *image,const char *name,KljElfTlsIndex *result){
    if(!image->tls_module)return 0;
    size_t count=image->nsyms;
    if(!count)count=((const char *)image->strtab-(const char *)image->symtab)/sizeof(Elf64_Sym);
    const char *end=(const char *)image->base+image->span;
    for(size_t n=0;n<count;n++){
        const Elf64_Sym *entry=&image->symtab[n];
        if((const char *)(entry+1)>end)break;
        if(!entry->st_shndx||!entry->st_name||(entry->st_info&15)!=6)continue;
        const char *candidate=image->strtab+entry->st_name;
        if(candidate<(const char *)image->base||candidate>=end)continue;
        if(!strcmp(candidate,name)){*result=(KljElfTlsIndex){image->tls_module,entry->st_value};return 1;}
    }
    return 0;
}
extern int klj_guest_tls_symbol(const char *,KljElfTlsIndex *);
static int klj_image_tls_index(kl_image *image,uint32_t symbol,KljElfTlsIndex *result){
    *result=(KljElfTlsIndex){image->tls_module,0};
    if(!symbol){if(image->tls_module)return 0;err("TLS relocation requires an ELF TLS segment");return -1;}
    if(image->nsyms&&symbol>=image->nsyms){err("Invalid ELF TLS symbol index");return -1;}
    const Elf64_Sym *entry=&image->symtab[symbol];
    const char *name=image->strtab+entry->st_name;
    if((entry->st_info&15)!=6 /* STT_TLS */){err("TLS relocation names non-TLS symbol %s",name);return -1;}
    if(entry->st_shndx){
        if(!image->tls_module){err("ELF TLS symbol has no TLS segment");return -1;}
        result->offset=entry->st_value;return 0;
    }
    if(klj_guest_tls_symbol(name,result))return 0;
    if((entry->st_info>>4)==STB_WEAK){*result=(KljElfTlsIndex){0,0};return 0;}
    err("Undefined ELF TLS symbol %s",name);return -1;
}
static int klj_image_tls_relocate(kl_image *image,const Elf64_Rela *relocation,uint32_t type){
    KljElfTlsIndex index;
    size_t width=type==1031 /* R_AARCH64_TLSDESC */?16:8;
    if(relocation->r_offset>image->span||width>image->span-relocation->r_offset){
        err("Invalid ELF TLS relocation target");return -1;
    }
    if(klj_image_tls_index(image,ELF64_R_SYM(relocation->r_info),&index))return -1;
    uint64_t *target=(uint64_t *)(image->base+relocation->r_offset);
    if(type==1028 /* R_AARCH64_TLS_DTPMOD64 */){*target=index.module;return 0;}
    index.offset+=(uintptr_t)relocation->r_addend;
    if(type==1029 /* R_AARCH64_TLS_DTPREL64 */){*target=index.offset;return 0;}
    if(type==1030 /* R_AARCH64_TLS_TPREL64 */){
        uintptr_t offset;
        if(klj_elf_tls_static_offset(&index,&offset)){
            err("ELF initial-exec TLS has no compatible TP-relative storage: %s",strerror(errno));return -1;
        }
        *target=offset;return 0;
    }
    struct klj_elf_tls_descriptor_node *node=malloc(sizeof *node);
    if(!node){err("ELF TLS descriptor allocation: %s",strerror(errno));return -1;}
    *node=(struct klj_elf_tls_descriptor_node){index,image->tls_descriptors};image->tls_descriptors=node;
    target[0]=(uintptr_t)klj_elf_tls_resolver;target[1]=(uintptr_t)&node->index;
    return 0;
}
