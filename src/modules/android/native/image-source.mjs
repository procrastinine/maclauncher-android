import fs from 'node:fs';import path from 'node:path';import {fileURLToPath} from 'node:url';
const root=path.dirname(fileURLToPath(import.meta.url));
export function prepareImageSource(vendor,build) {
 let source=fs.readFileSync(path.join(vendor,'runtime/kl_image.c'),'utf8');
 const replace=(from,to)=>{if(source.split(from).length!==2)throw Error('Pinned ELF initialization source changed: '+from);source=source.replace(from,to);};
 const replaceBetween=(from,to,value)=>{const start=source.indexOf(from),end=source.indexOf(to,start);if(start<0||end<0)throw Error('Pinned ELF source range changed: '+from);source=source.slice(0,start)+value+source.slice(end);};
 replace('        if (gva < s->sh_addr || gva + len > s->sh_addr + s->sh_size) continue;',
  '        if(gva<s->sh_addr || gva-s->sh_addr>s->sh_size || len>s->sh_size-(gva-s->sh_addr))continue;');
 // Relocation precedes this shared page plan. Seal ordinary RELRO as well as
 // pages where RELRO resolves a write/execute collision. Keep any writable
 // neighbor outside the original range when a host page contains both.
 const relroEnd='        if (relro_covers) { bits[p] &= (uint8_t)~PF_W; continue; }\n';
 replace('        if ((bits[p] & (PF_W | PF_X)) != (PF_W | PF_X)) continue;',
         '        if (!(bits[p] & PF_W)) continue;');
 replace('        int has_code = 0;','        if (bits[p] & PF_X) {\n        int has_code = 0;');
 replace('        if (!has_code) { bits[p] &= (uint8_t)~PF_X; continue; }',
         '        if (!has_code) bits[p] &= (uint8_t)~PF_X;\n        }');
 replace(relroEnd,relroEnd+'\n        if (!(bits[p] & PF_X)) continue;\n');
 source='#include '+JSON.stringify(path.join(root,'elf-tls.h'))+'\n#include '+JSON.stringify(path.join(root,'image-constructor.h'))+'\n#include '+JSON.stringify(path.join(root,'native-invocation.h'))+'\n#include '+JSON.stringify(path.join(root,'image-format.h'))+'\n#include '+JSON.stringify(path.join(root,'inline-syscall.h'))+'\n#include '+JSON.stringify(path.join(root,'x18-pools.h'))+'\n#include '+JSON.stringify(path.join(root,'executable-memory/jvm.h'))+'\n#include <stdatomic.h>\n#define DT_INIT 12\n'+source;
 replace('struct kl_image {','#include '+JSON.stringify(path.join(root,'image-code-ranges.h'))+'\n#include '+JSON.stringify(path.join(root,'image-identity.h'))+'\n#include '+JSON.stringify(path.join(root,'library-binding.h'))+'\n\nstruct kl_image {\n    const KlImageFormat *format;\n    void *format_payload;\n    char *soname;\n    int identity_captured;\n    uint64_t binding_flags_1;\n    _Atomic unsigned binding_rtld_flags;\n    unsigned binding_target_sdk;\n    struct kl_image *binding_local_group;\n    struct kl_image **dependencies;\n    size_t dependency_count;\n    KlImageCodeMetadata code_metadata;');
 replace('    size_t     span;','    uintptr_t tls_module;\n    struct klj_elf_tls_descriptor_node *tls_descriptors;\n    size_t inline_prefix_size,inline_pool_count,tls_data_words;\n    KljInlinePool *inline_pools;\n    size_t     span;');
 replace('static int apply_relocs(kl_image *img, const Elf64_Rela *r, size_t count) {',
  '#include '+JSON.stringify(path.join(root,'elf-tls-image.inc'))+'\n\nstatic int apply_relocs(kl_image *img, const Elf64_Rela *r, size_t count) {');
 replace('        case R_AARCH64_RELATIVE:',
  '        case 1028: /* R_AARCH64_TLS_DTPMOD64 */\n        case 1029: /* R_AARCH64_TLS_DTPREL64 */\n        case 1030: /* R_AARCH64_TLS_TPREL64 */\n        case 1031: /* R_AARCH64_TLSDESC */\n            if(klj_image_tls_relocate(img,r,type))return -1;\n            break;\n        case R_AARCH64_RELATIVE:');
 replace('static int bind_dynamic(kl_image *img, const Elf64_Phdr *ph, int phnum, uint64_t lo) {',
  'static int bind_dynamic(kl_image *img, const Elf64_Phdr *ph, int phnum, uint64_t lo) {\n    if(klj_image_tls_register(img,ph,phnum,lo))return -1;');
 replace('    if (bind_dynamic(img, ph, eh->e_phnum, lo) != 0) { free(img); dlclose(h); return NULL; }',
  '    if (bind_dynamic(img, ph, eh->e_phnum, lo) != 0) { klj_image_tls_unregister(img);free(img);dlclose(h);return NULL; }');
 replace('                        kl_stats *st) {','                        kl_stats *st, const klx_code_context *context, const KlImageCodeMetadata *metadata, size_t *data_words) {');
 replace('        if ((w[i] & 0xffffffe0u) == 0xd53bd040u) {',`        if ((w[i] & 0xffffffe0u) == 0xd53bd040u) {
            // Only original typed metadata or established memory-reference
            // evidence can classify a TP-shaped word as data. An uncertain
            // neighborhood remains a refusal, never a successful omission.
            uint64_t address=va+i*4;size_t left=0,right=metadata?metadata->dataCount:0;
            while(left<right){size_t mid=left+(right-left)/2;if(metadata->data[mid].start<=address)left=mid+1;else right=mid;}
            if(left && address<metadata->data[left-1].end){if(data_words)(*data_words)++;continue;}`);
 replace('neighbourhood reads as data. If it IS code, ', 'neighbourhood is not proven executable. If it IS code, ');
 replace('            if (kl_x18_is_data(w, n, i)) {','            if (kl_x18_is_data_context(w, n, i, context)) {');
 // The shared instruction pass emits AIM's actual guest-TP load. A bit flip
 // would expose Darwin's reserved TSD slots as Android's writable TLS block.
 replace('            w[i] |= 0x20u;','            /* The image-owned TP veneer is emitted by the shared instruction pass. */');
 replace(`        for (int i = 0; sh && i < eh->e_shnum; i++)
            if ((sh[i].sh_flags & SHF_EXECINSTR) && sh[i].sh_type != SHT_NOBITS
                && (int64_t)sh[i].sh_addr < pe
                && (int64_t)(sh[i].sh_addr + sh[i].sh_size) > ps)
                has_code = 1;`,`        KlImageCodeCursor code_cursor={0};KlImageCodeRange code;
        while(klj_image_code_next(eh,ph,sh,SIZE_MAX,&code_cursor,&code))
            if((int64_t)code.address<pe && (int64_t)(code.address+code.size)>ps) {
                has_code=1;break;
            }`);
 replace('const char *kl_error(void) { return g_err; }',`const char *kl_error(void) { return g_err; }
static _Atomic(const KlImageFormat *) klj_formats[256];
void klj_register_image_format(const KlImageFormat *format) {
    if(format && format->elf_class<256)atomic_store(&klj_formats[format->elf_class],format);
}
void *klj_image_format_payload(void *opaque,unsigned elf_class) {
    kl_image *image=opaque;
    return image && image->format && image->format->elf_class==elf_class ? image->format_payload : NULL;
}
static const KlImageFormat *klj_file_format(const char *path) {
    unsigned char ident[5];FILE *file=fopen(path,"rb");if(!file)return NULL;
    size_t size=fread(ident,1,sizeof ident,file);fclose(file);
    return size==sizeof ident && !memcmp(ident,"\\177ELF",4) ? atomic_load(&klj_formats[ident[4]]) : NULL;
}
int klj_image_group_supported(const char *path){const KlImageFormat *format=klj_file_format(path);return format&&format->map_unlinked&&format->relocate&&format->dependencies;}
kl_image *klj_image_map_unlinked(const char *path,char *error,size_t capacity){
    const KlImageFormat *format=klj_file_format(path);if(!format||!format->map_unlinked)return NULL;
    kl_image *image=calloc(1,sizeof *image);if(!image)return NULL;
    uintptr_t base=0;image->format=format;
    image->format_payload=format->map_unlinked(format->context,path,&base,&image->span,error,capacity);
    if(!image->format_payload){free(image);return NULL;}
    image->base=(uint8_t *)base;snprintf(image->path,sizeof image->path,"%s",path);return image;
}
int klj_image_dependencies(const char *path,int (*visit)(void *,const char *),void *context,char *error,size_t capacity){
    const KlImageFormat *format=klj_file_format(path);return format&&format->dependencies&&format->dependencies(format->context,path,visit,context,error,capacity);
}
int klj_image_relocate(kl_image *image,void *(*lookup)(void *,const char *),void *context,char *error,size_t capacity){
    return image->format&&image->format->relocate&&image->format->relocate(image->format_payload,lookup,context,error,capacity);
}
int klj_image_set_dependencies(kl_image *image,void *const *dependencies,size_t count){
    if(count>SIZE_MAX/sizeof(void *))return 0;
    void *copy=count?malloc(count*sizeof(void *)):NULL;if(count&&!copy)return 0;
    if(count)memcpy(copy,dependencies,count*sizeof(void *));
    free(image->dependencies);image->dependencies=copy;image->dependency_count=count;return 1;
}
void klj_image_visit_dependencies(kl_image *image,void (*visit)(void *,void *),void *context){
    for(size_t i=0;i<image->dependency_count;i++)visit(context,image->dependencies[i]);
}
const char *klj_image_runpath(kl_image *image){
    return image->format&&image->format->runpath?image->format->runpath(image->format_payload):NULL;
}`);
 replace('kl_image *kl_load_auto(const char *path) {',`kl_image *kl_load_auto(const char *path) {
    const KlImageFormat *format=klj_file_format(path);
    if(format) {
        kl_image *image=calloc(1,sizeof *image);if(!image){err("Image allocation failed");return NULL;}
        uintptr_t base=0;
        image->format=format;
        image->format_payload=format->map(format->context,path,&base,&image->span,g_err,sizeof g_err);
        if(!image->format_payload){free(image);return NULL;}
        image->base=(uint8_t *)base;snprintf(image->path,sizeof image->path,"%s",path);
        return image;
    }`);
 replace('int kl_list_needed(const char *path, char names[][128], int max) {',`int kl_list_needed(const char *path, char names[][128], int max) {
    const KlImageFormat *format=klj_file_format(path);
    if(format)return format->needed(format->context,path,names,max,g_err,sizeof g_err);`);
 replace('    if (!i || !i->base) return NULL;', '    if (!i || !i->base) return NULL;\n    if(i->format)return i->format->phdrs(i->format_payload,count);');
 replace('    if (img->dl_handle) dlclose(img->dl_handle);', '    if (img->format) { img->format->unmap(img->format_payload);free(img);return; }\n    if (img->dl_handle) dlclose(img->dl_handle);');
 replace('    if (!img) return;','    if (!img) return;\n    extern int klj_guest_mapping_claim(void *,size_t,int);\n    if(klj_guest_mapping_claim(img->base,img->span,0))abort();\n    klj_image_tls_unregister(img);\n    klj_executable_code_forget(img->base,img->span);\n    free(img->soname);\n    free(img->dependencies);\n    free(img->inline_pools);\n    klj_code_metadata_release(&img->code_metadata);');
 replace('                         ? (const Elf64_Shdr *)(file + eh->e_shoff) : NULL;',`                         ? (const Elf64_Shdr *)(file + eh->e_shoff) : NULL;
    if (!klj_code_metadata_read(&img->code_metadata,file,(size_t)sb.st_size,eh,ph,sh)) {
        err("Unable to allocate native instruction metadata");
        klj_code_metadata_release(&img->code_metadata);free(img);munmap(file,sb.st_size);return NULL;
    }`);
 replace('                             free(img); munmap(file, sb.st_size); return NULL; }','                             klj_code_metadata_release(&img->code_metadata);free(img); munmap(file, sb.st_size); return NULL; }');
 replace('    if (bind_dynamic(img, ph, eh->e_phnum, lo) != 0) {\n        munmap(file, sb.st_size); return NULL;\n    }','    if (bind_dynamic(img, ph, eh->e_phnum, lo) != 0) {\n        kl_unload(img);munmap(file, sb.st_size); return NULL;\n    }');
 replaceBetween('    for (int i = 0; sh && i < eh->e_shnum; i++) {\n        if (sh[i].sh_type != SHT_PROGBITS || !(sh[i].sh_flags & SHF_EXECINSTR)) continue;\n        if (sh[i].sh_offset + sh[i].sh_size > (uint64_t)sb.st_size) continue;\n        x18_arena +=', '    if (x18_arena) {', `    KlImageCodeCursor reservation_cursor={.metadata=&img->code_metadata};KlImageCodeRange reservation;
    while(klj_image_code_next(eh,ph,sh,(size_t)sb.st_size,&reservation_cursor,&reservation)) {
        size_t sites=klj_inline_syscall_count_context(file+reservation.offset,reservation.size,
            &(klx_code_context){img->code_metadata.marks,img->code_metadata.count,reservation.address,img->code_metadata.reachable,img->code_metadata.reachableCount});
        sites+=(size_t)kl_x18_capacity(file+reservation.offset,reservation.size);
        if(sites>SIZE_MAX-x18_arena) {
            err("Inline instruction arena size overflow");klj_code_metadata_release(&img->code_metadata);free(img);munmap(file,sb.st_size);return NULL;
        }
        x18_arena+=sites;
    }
    if(x18_arena>(SIZE_MAX-(64u<<10)*64-(size_t)pg)/(KLX_VEN_MAX_INSN*4)) {
        err("Inline instruction arena size overflow");klj_code_metadata_release(&img->code_metadata);free(img);munmap(file,sb.st_size);return NULL;
    }
`);
 // The upstream pre-image reservation also supplies the owned pool planner.
 // Preserve exact PT_LOAD gaps for sites beyond either outer arena's reach.
 replace('    img->x18_arena_size = x18_arena;',
  '    img->x18_arena_size = x18_arena;\n    img->inline_prefix_size=x18_arena;\n    if(img->map_size>SIZE_MAX-x18_arena || x18_arena>SIZE_MAX-img->map_size-x18_arena) {\n        err("Inline instruction mapping size overflow");klj_code_metadata_release(&img->code_metadata);free(img);munmap(file,sb.st_size);return NULL;\n    }');
 replace('static int protect_pages(kl_image *img, const Elf64_Ehdr *eh,',
  '#include '+JSON.stringify(path.join(root,'x18-pools.inc'))+'\n#include '+JSON.stringify(path.join(root,'image-inline-pools.h'))+'\n\nstatic int protect_pages(kl_image *img, const Elf64_Ehdr *eh,');
 replaceBetween('    int veneer = sh != NULL;','    // ...and code inside those sections, not whole sections.',`    // Original ELF code can reach a context-completing veneer before any
    // executable mmap import. Capture the live VM's dispositions at load time.
    if(klj_executable_signal_enable()) {
        err("Native signal adaptation: %s",strerror(errno));
        kl_unload(img);munmap(file,sb.st_size);return NULL;
    }

`);
 replaceBetween('    uint8_t *x18_arena_base = img->x18_arena_size',
  '    // Measured guest patches,',`    if(klj_image_inline_pools(img,ph,eh->e_phnum,lo)) {
        err("Inline instruction arena metadata: %s",strerror(errno));
        kl_unload(img);munmap(file,sb.st_size);return NULL;
    }
    struct x18_img_ctx ictx={sh,sh?eh->e_shnum:0,(uint64_t)(uintptr_t)img->base-lo};
    kl_x18_image ximg={x18_img_data_at,&ictx};
    KlImageCodeCursor rewrite_cursor={.metadata=&img->code_metadata};KlImageCodeRange code;
    while(klj_image_code_next(eh,ph,sh,(size_t)sb.st_size,&rewrite_cursor,&code)) {
        uint64_t cursor=code.address,cva;size_t csz;
        while(kl_x18_next_code(code.address,code.size,skip,nskip,&cursor,&cva,&csz)) {
            uint8_t *p=img->base+cva-lo;
            // The upstream recogniser observes producers before any ordinary
            // per-instruction rewrite. The shared planner still chooses exact
            // reachable pools, including genuine gaps within large images.
            klx_code_context context={img->code_metadata.marks,img->code_metadata.count,cva,img->code_metadata.reachable,img->code_metadata.reachableCount};
            kl_x18_stats tables;
            if(klj_image_tables_patch_context(p,csz,img->inline_pools,img->inline_pool_count,&ximg,&tables,&context)) {
                err("Native jump-table adaptation: %s",strerror(errno));
                kl_unload(img);munmap(file,sb.st_size);return NULL;
            }
            rewrite_tls(p,csz,cva,path,&img->stats,&context,&img->code_metadata,&img->tls_data_words);
            kl_x18_stats xs;
            if(klj_image_instruction_patch_context(p,csz,img->inline_pools,img->inline_pool_count,&xs,&context)) {
                err("Linux inline instruction adaptation: %s",strerror(errno));
                kl_unload(img);munmap(file,sb.st_size);return NULL;
            }
            img->stats.x18_sites+=xs.sites+tables.jt_sites;
            img->stats.x18_patched+=xs.patched+tables.jt_sites;
            img->stats.x18_refused+=xs.refused;
            img->stats.x18_data_words+=xs.data_words;
            img->stats.ctr_sites+=xs.ctr_sites;
            img->stats.ctr_patched+=xs.ctr_patched;
            img->stats.ctr_refused+=xs.ctr_refused;
        }
    }
`);
 replace('    if (protect_pages(img, eh, ph, sh, lo) != 0) {\n        munmap(file, (size_t)sb.st_size); return NULL;',
  '    if (protect_pages(img, eh, ph, sh, lo) != 0 || klj_inline_syscall_pools_seal(img->inline_pools,img->inline_pool_count) != 0) {\n        kl_unload(img);munmap(file, (size_t)sb.st_size); return NULL;');
 replace('        uint64_t *slot = (uint64_t *)(img->base + r->r_offset);\n        uint32_t type = ELF64_R_TYPE(r->r_info);',
  '        uint32_t type = ELF64_R_TYPE(r->r_info);\n        if (type == 0) continue; // R_AARCH64_NONE has no target, addend or symbol.\n        uint64_t *slot = (uint64_t *)(img->base + r->r_offset);');
 replace('    void     (**init_array)(void);','    KlImageInitialization initialization;\n    void      (*init_function)(void);\n    void     (**init_array)(void);');
 replace('            case DT_INIT_ARRAY:','            case DT_INIT: img->init_function = dp->d_val ? (void (*)(void))(img->base + dp->d_val) : NULL; break;\n            case DT_INIT_ARRAY:');
 replace('void kl_run_init(kl_image *img) {\n    for (size_t i = 0; i < img->init_count; i++)\n        if (img->init_array[i]) img->init_array[i]();\n}',`static _Atomic(int (*)(void *)) klj_initializer_observer;
static _Atomic(void *) klj_initializer_context;
void klj_set_initializer_observer(int (*observer)(void *),void *context) {
    atomic_store(&klj_initializer_context,context);
    atomic_store(&klj_initializer_observer,observer);
}
struct klj_init_call {kl_image *image;int (*failed)(void *);void *context;};
int klj_run_initializers(kl_image *,int (*)(void *),void *);
static int klj_initialize_image(void *opaque) {
    struct klj_init_call *call=opaque;kl_image *img=call->image;
    for(size_t i=0;i<img->dependency_count;i++)if(!klj_run_initializers(img->dependencies[i],call->failed,call->context))return 0;
    if(img->format)return img->format->initialize(img->format_payload);
    if(img->init_function)KLJ_INVOKE_GUEST(klj_guest_thread_environment(),img->init_function());
    if(call->failed && call->failed(call->context))return 0;
    for(size_t i=0;i<img->init_count;i++) {
        void (*initializer)(void)=img->init_array[i];
        if(initializer && (uintptr_t)initializer!=UINTPTR_MAX)KLJ_INVOKE_GUEST(klj_guest_thread_environment(),initializer());
        if(call->failed && call->failed(call->context))return 0;
    }
    return 1;
}
int klj_run_initializers(kl_image *img,int (*failed)(void *),void *context) {
    if(!img)return 0;
    if(!failed) { failed=atomic_load(&klj_initializer_observer);context=atomic_load(&klj_initializer_context); }
    struct klj_init_call call={img,failed,context};
    return klj_image_construct(&img->initialization,klj_initialize_image,&call);
}
void kl_run_init(kl_image *img) { (void)klj_run_initializers(img,NULL,NULL); }
int klj_image_initialization_failed(kl_image *img) { return klj_image_failed(&img->initialization); }
void klj_reject_image_initialization(kl_image *img) { klj_image_reject_new(&img->initialization); }
const char *klj_image_path(kl_image *img) { return img->path; }
void klj_image_capture_identity(kl_image *img) {
    if(!img->identity_captured) {
        img->soname=klj_elf_identity(img->path,&img->binding_flags_1);
        img->binding_flags_1|=img->needed.flags_1;
        img->binding_target_sdk=klj_library_target_sdk();
        img->identity_captured=1;
    }
}
const char *klj_image_soname(kl_image *img) { return img->soname; }
uint64_t klj_image_dt_flags_1(void *opaque){kl_image *image=opaque;klj_image_capture_identity(image);return image->binding_flags_1;}
unsigned klj_image_rtld_flags(void *opaque){return atomic_load_explicit(&((kl_image *)opaque)->binding_rtld_flags,memory_order_acquire);}
unsigned klj_image_target_sdk(void *opaque){kl_image *image=opaque;klj_image_capture_identity(image);return image->binding_target_sdk;}
void klj_image_set_rtld_flags(void *opaque,unsigned flags){atomic_fetch_or_explicit(&((kl_image *)opaque)->binding_rtld_flags,flags,memory_order_acq_rel);}
void *klj_image_local_group(void *opaque){kl_image *image=opaque;return image->binding_local_group?image->binding_local_group:image;}
void klj_image_set_local_group(void *opaque,void *root){((kl_image *)opaque)->binding_local_group=root;}`);
 replace('void *kl_sym(kl_image *img, const char *name) {','void *kl_sym(kl_image *img, const char *name) {\n    if (klj_image_initialization_failed(img)) return NULL;\n    if(img->format)return img->format->symbol(img->format_payload,name);');
 replace('            return img->base + s->st_value;',
  '            return (s->st_info&15)==6 ? klj_elf_tls_address(&(KljElfTlsIndex){img->tls_module,s->st_value}) : img->base+s->st_value;');
 replaceBetween('//   1. Drop X if no SHF_EXECINSTR SECTION lands on the page.','//   2. Otherwise drop W',
  '//   1. Drop X only when the shared executable-region iterator finds no code\n//      on the page. ELF sections and typed segments exclude known data; omitted\n//      executable ranges keep the segment permission.\n');
 replaceBetween('    // Section headers are read here rather than at the rewrite passes below','    const Elf64_Shdr *sh =',
  '    // Section metadata supplies data exclusions to the shared executable\n    // iterator used by placement, veneer reservation and adaptation.\n');
 replaceBetween('    // Sized by counting the sites in the FILE\'s executable sections','    size_t x18_arena = 0;',
  '    // Count the same executable regions that the adaptation pass visits.\n    // Upstream data detection still excludes constants inside those regions.\n');
 replaceBetween('    // Executable *sections*, not the executable segment.','    // The authoritative .dynsym entry count',
  '    // Executable mappings own coverage. Sections and typed metadata remove\n    // known data; section gaps remain eligible for the shared instruction pass.\n\n');
 source+='\n#include '+JSON.stringify(path.join(root,'image-pool-lease.h'))+`\n
size_t klj_image_gap_pools(kl_image *image,KljImagePoolLease *leases,size_t capacity){
    if(image->format)return 0;
    size_t count=0;
    for(size_t i=0;i<image->inline_pool_count;i++){
        KljInlinePool *pool=&image->inline_pools[i];
        if(!pool->packed||pool->used>=pool->size)continue;
        if(count<capacity)leases[count]=(KljImagePoolLease){image,i,pool->used,
            {(char *)pool->base+pool->used,pool->size-pool->used,0,1}};
        count++;
    }
    return count;
}
void klj_image_gap_pool_commit(KljImagePoolLease *lease,size_t used){
    kl_image *image=lease->image;KljInlinePool *pool=&image->inline_pools[lease->index];
    if(pool->used!=lease->previous||used>pool->size-pool->used)abort();
    pool->used+=used;
}
int klj_image_gap_pool_release(KljImagePoolLease *lease,size_t used){
    kl_image *image=lease->image;KljInlinePool *pool=&image->inline_pools[lease->index];
    if(pool->used!=lease->previous+used)return 0;
    pool->used=lease->previous;return 1;
}\n`;
 source='#include '+JSON.stringify(path.join(root,'image-dependencies.h'))+'\n'+source;
 replace('    char *soname;','    KlImageDependencies needed;\n    char *soname;');
 replace('    free(img->soname);','    klj_image_dependencies_release(&img->needed);\n    free(img->soname);');
 replace('klj_image_tls_unregister(img);free(img);dlclose(h);','klj_image_tls_unregister(img);klj_image_dependencies_release(&img->needed);free(img);dlclose(h);');
 replace('    if (!img->symtab || !img->strtab) { err("missing DT_SYMTAB/DT_STRTAB"); return -1; }',
  '    if (!img->symtab || !img->strtab) { err("missing DT_SYMTAB/DT_STRTAB"); return -1; }\n    for(int i=0;i<phnum;i++)if(ph[i].p_type==PT_DYNAMIC && !klj_image_dependencies_capture(&img->needed,img->base,img->span,\n        (const uint64_t *)(img->base+ph[i].p_vaddr-lo),(size_t)ph[i].p_filesz,img->strtab)) {\n        err("Unable to retain original ELF dependency metadata");return -1;\n    }');
 source+='\nvoid klj_image_visit_needed(kl_image *image,void (*visit)(void *,const char *),void *context){\n    for(size_t i=0;i<image->needed.count;i++)visit(context,image->needed.names[i]);\n}\n';
 replace('    if (!ext) ext = kl_guest_sym_global(nm);','    if (!ext) ext = klj_guest_sym_for_image(img,nm);');
 source='#include '+JSON.stringify(path.join(root,'image-startup.h'))+'\n'+source;
 replace('    if(img->format)return img->format->initialize(img->format_payload);',
  '    if(img->format)return img->format->initialize(img->format_payload);\n    const KljImageStartup *startup=klj_library_startup();\n    if(!startup){err("Android native process startup is not configured");return 0;}');
 replace('img->init_function());','((void (*)(int,char **,char **))img->init_function)(startup->argc,startup->argv,startup->envp));');
 replace('initializer());','((void (*)(int,char **,char **))initializer)(startup->argc,startup->argv,startup->envp));');
 // Program-header identity belongs to the loaded image. Keep the mapped
 // pointer and original count as Bionic's soinfo does; content stays live.
 replace('    KlImageCodeMetadata code_metadata;',
  '    KlImageCodeMetadata code_metadata;\n    const Elf64_Phdr *mapped_phdr;\n    unsigned mapped_phnum;');
 replace(`    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)i->base;
    if (memcmp(eh->e_ident, "\\177ELF", 4) != 0) return NULL;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return NULL;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > i->span) return NULL;
    if (count) *count = eh->e_phnum;
    return i->base + eh->e_phoff;`,
 `    if (count) *count = i->mapped_phnum;
    return i->mapped_phdr;`);
 const phdrCapture=`static void klj_retain_mapped_phdrs(kl_image *image) {
    if(!image->base || image->span<sizeof(Elf64_Ehdr))return;
    const Elf64_Ehdr *header=(const Elf64_Ehdr *)image->base;
    if(memcmp(header->e_ident,"\\177ELF",4) || header->e_phentsize!=sizeof(Elf64_Phdr))return;
    uint64_t bytes=(uint64_t)header->e_phnum*sizeof(Elf64_Phdr);
    if(header->e_phoff>image->span || bytes>image->span-header->e_phoff)return;
    image->mapped_phdr=(const Elf64_Phdr *)(image->base+header->e_phoff);
    image->mapped_phnum=header->e_phnum;
}
`;
 replace('const void *kl_phdrs(kl_image *i, unsigned *count) {',phdrCapture+'const void *kl_phdrs(kl_image *i, unsigned *count) {');
 replace('    // ---- PT_DYNAMIC, then relocate ----',
  '    klj_retain_mapped_phdrs(img);\n\n    // ---- PT_DYNAMIC, then relocate ----');
 replace('    img->span = (size_t)(hi - lo);\n\n    if (bind_dynamic',
  '    img->span = (size_t)(hi - lo);\n    klj_retain_mapped_phdrs(img);\n\n    if (bind_dynamic');
 // A loaded image keeps the canonical pathname from its admitted namespace.
 // Filesystem renames and later alias publication do not replace soinfo identity.
 source='#include '+JSON.stringify(path.join(root,'guest-paths.h'))+'\n'+source;
 replace('    char *soname;','    char *soname;\n    char *guest_path;');
 replace('    free(img->soname);','    free(img->guest_path);\n    free(img->soname);');
 replace('        img->identity_captured=1;',`        KljGuestPathView *view=klj_guest_path_view();
        char path[4096];
        const char *guest=klj_guest_path_view_name(view,img->path,path,sizeof path);
        if(guest)img->guest_path=strdup(guest);
        klj_guest_path_view_release(view);
        img->identity_captured=1;`);
 replace('const char *klj_image_soname(kl_image *img) { return img->soname; }',
  'const char *klj_image_guest_path(kl_image *img) { return img->guest_path; }\nconst char *klj_image_soname(kl_image *img) { return img->soname; }');
 const file=path.join(build,'kl_image.c');fs.writeFileSync(file,source);return file;
}
