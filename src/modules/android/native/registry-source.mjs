import fs from 'node:fs';import path from 'node:path';import crypto from 'node:crypto';import {fileURLToPath} from 'node:url';
import {adaptRegistryBinding} from './registry-binding-source.mjs';
const root=path.dirname(fileURLToPath(import.meta.url));
export function prepareRegistrySource(vendor,build){
 const source=fs.readFileSync(path.join(vendor,'runtime/kl_dl.c'),'utf8'),hash=s=>crypto.createHash('sha256').update(s).digest('hex'),anchor='static int    g_nimgs = 0;';
 if(hash(source)!=='3e3638301ae96780b964793dedea7dc0caf40bc769e43a0530ed752d42cfdd42'||source.split(anchor).length!==2)throw Error('Pinned native registry source changed');
 let adapted='#include '+JSON.stringify(path.join(root,'native-import-registry.h'))+'\n'+'#include '+JSON.stringify(path.join(root,'library-registry.h'))+'\n#include '+JSON.stringify(path.join(root,'library-namespace.h'))+'\nstatic void klj_update_library_namespace(void);\n'+source;
 adapted='#include <dlfcn.h>\n'+['image-format.h','image-unwind.h','proc-maps.h','elf-tls.h'].map(name=>'#include '+JSON.stringify(path.join(root,name))).join('\n')+'\n'+adapted;
 const replace=(from,to)=>{if(adapted.split(from).length!==2)throw Error('Pinned native initialization anchor changed: '+from);adapted=adapted.replace(from,to);};
 replace('void kl_set_library_path(const char *dir) { snprintf(g_libdir, sizeof g_libdir, "%s", dir); }',
  'void kl_set_library_path(const char *dir) { snprintf(g_libdir, sizeof g_libdir, "%s", dir);klj_update_library_namespace(); }\n#include <limits.h>\n#include '+JSON.stringify(path.join(root,'guest-paths.h'))+'\n#include '+JSON.stringify(path.join(root,'library-paths.inc')));
 replace('int kl_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data) {',`uintptr_t dl_unwind_find_exidx(uintptr_t pc,int *count) {
    *count=0;
    KlImageRegistrySnapshot snapshot=klj_image_registry_snapshot(&klj_images);
    for(size_t index=0;index<snapshot.count;index++) {
        kl_image *image=snapshot.entries[index].img;uintptr_t base=(uintptr_t)kl_base(image);
        if(pc<base || pc-base>=kl_span(image))continue;
        unsigned length=0;const KlProgramHeader *headers=kl_phdrs(image,&length);
        for(unsigned n=0;n<length;n++)if(headers[n].type==0x70000001u) {
            *count=(int)(headers[n].memsz/8);return base+headers[n].vaddr;
        }
        break;
    }
    return 0;
}
int kl_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data) {`);
 replace('        if (strchr(names[i], \'/\')) snprintf(full, sizeof full, "%s", names[i]);\n        else snprintf(full, sizeof full, "%s/%s", g_libdir, names[i]);',
  '        if (!klj_resolve_library_path(names[i],path,full,sizeof full)) {\n            fprintf(stderr,"  [klepton] DT_NEEDED %s is unavailable in the application and system library paths\\n",names[i]);\n            continue;\n        }');
 replace('    if (strchr(path, \'/\')) snprintf(full, sizeof full, "%s", path);\n    else                   snprintf(full, sizeof full, "%s/%s", g_libdir, path);',
  '    if (!klj_resolve_library_path(path,NULL,full,sizeof full)) {\n        snprintf(g_dlerr,sizeof g_dlerr,"dlopen failed: library \\"%s\\" not found",basename_of(path));\n        return NULL;\n    }');
 replace('           kl_openxr_claims(path) || kl_core_shim_claims(path) || kl_can_load(path);',
  '           kl_openxr_claims(path) || kl_core_shim_claims(path) || klj_resolve_library_path(path,NULL,(char[PATH_MAX]){0},PATH_MAX);');
 replace('"  [klepton] guest dlopen(\\"%s\\") FAILED: %s\\n", path, kl_error());',
  '"  [klepton] guest dlopen(\\"%s\\") FAILED: %s\\n", path, klj_image_load_error());');
 replace('    kl_shim_override = kl_dl_shim_override;','    (void)klj_register_import_resolver(kl_dl_shim_override);');
 replace('#define KL_MAX_IMAGES 64\ntypedef struct { char soname[128]; kl_image *img; } entry;\nstatic entry  g_imgs[KL_MAX_IMAGES];\n'+anchor,'static KlImageRegistry klj_images;');
 replace('void kl_register_image(const char *soname, kl_image *img) {',
  'extern int klj_guest_mapping_claim(void *,size_t,int);\nextern void klj_image_capture_identity(kl_image *);\nextern const char *klj_image_soname(kl_image *);\nextern const char *klj_image_path(kl_image *);\nvoid kl_register_image(const char *soname, kl_image *img) {\n    if(klj_guest_mapping_claim(kl_base(img),kl_span(img),1))abort();');
 replace('    const char *base = basename_of(soname);\n    pthread_mutex_lock(&g_lock);',
  '    const char *base = basename_of(soname);\n    pthread_mutex_lock(&g_lock);\n    klj_image_capture_identity(img);');
 replace('    for (int i = 0; i < g_nimgs; i++)\n        if (strcmp(g_imgs[i].soname, b) == 0) return g_imgs[i].img;',
  '    for (int i = 0; i < g_nimgs; i++) {\n        const char *declared=klj_image_soname(g_imgs[i].img),*file=klj_image_path(g_imgs[i].img);\n        if (!strcmp(g_imgs[i].soname,b) || (declared && !strcmp(declared,b)) || (file && !strcmp(basename_of(file),b))) return g_imgs[i].img;\n    }');
 replace('    if (g_nimgs < KL_MAX_IMAGES) {\n        snprintf(g_imgs[g_nimgs].soname, sizeof g_imgs[0].soname, "%s", base);\n        g_imgs[g_nimgs].img = img;\n        g_nimgs++;\n    }',`    if (!klj_image_registry_append(&klj_images,base,img) || !klj_namespace_publish(klj_default_library_namespace(),img,1)) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr,"Unable to allocate native image registry\\n");
        abort();
    }`);
 replace('kl_image *kl_load_recursive(const char *path) {',`extern int klj_run_initializers(kl_image *,int (*)(void *),void *);
extern const char *klj_image_path(kl_image *);
static pthread_mutex_t klj_mapping_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local char klj_mapping_error[1024];
const char *klj_image_load_error(void) { return klj_mapping_error[0]?klj_mapping_error:kl_error(); }
static const char *klj_registry_path(void *context,void *image){(void)context;return klj_image_path(image);}
static void *klj_registry_map(void *context,const char *path){(void)context;return kl_load_auto(path);}
static void klj_registry_unmap(void *context,void *image){(void)context;kl_unload(image);}
static int klj_registry_validate(void *context,void *image,char *error,size_t capacity){
    (void)context;const kl_stats *stats=kl_get_stats(image);
    if(!stats->tls_refused&&!stats->x18_refused)return 1;
    snprintf(error,capacity,"Android native library has %u unsupported TLS sites and %u unsupported register sites",stats->tls_refused,stats->x18_refused);
    return 0;
}
static int klj_registry_publish(void *context,const char *name,void *image){(void)context;kl_register_image(name,image);return 1;}
extern int klj_image_group_supported(const char *);
extern kl_image *klj_image_map_unlinked(const char *,char *,size_t);
extern int klj_image_dependencies(const char *,int (*)(void *,const char *),void *,char *,size_t);
extern int klj_image_relocate(kl_image *,void *(*)(void *,const char *),void *,char *,size_t);
extern int klj_image_set_dependencies(kl_image *,void *const *,size_t);
extern void klj_image_visit_dependencies(kl_image *,void (*)(void *,void *),void *);
extern const char *klj_image_runpath(kl_image *);
static int klj_group_contains(void *context,void *space,void *image){(void)context;return klj_namespace_contains(space,image);}
static void *klj_group_map(void *context,void *space,const char *path,char *error,size_t capacity){(void)context;(void)space;return klj_image_map_unlinked(path,error,capacity);}
static int klj_group_dependencies(void *context,const char *path,int (*visit)(void *,const char *),void *opaque,char *error,size_t capacity){(void)context;return klj_image_dependencies(path,visit,opaque,error,capacity);}
static int klj_group_resolve(void *context,void *space,void *requester_image,const char *requester,const char *name,void **provider,char *path,size_t capacity,char *error,size_t error_capacity){
    (void)context;*provider=space;
    if(kl_dep_is_shim_served(basename_of(name)))return 2;
    KljLibraryNamespace *found=klj_namespace_resolve_from(space,name,requester,klj_image_runpath(requester_image),klj_library_elf_class,klj_library_candidate,NULL,path,capacity);
    if(found){*provider=found;return 1;}
    snprintf(error,error_capacity,"Native dependency %s required by %s is unavailable",name,requester);return 0;
}
static void *klj_group_symbol(void *context,void *image,const char *name){(void)context;return kl_sym(image,name);}
static int klj_group_relocate(void *context,void *image,void *(*lookup)(void *,const char *),void *opaque,char *error,size_t capacity){(void)context;return klj_image_relocate(image,lookup,opaque,error,capacity);}
static int klj_group_dependencies_set(void *context,void *image,void *const *children,size_t count){(void)context;return klj_image_set_dependencies(image,children,count);}
static void klj_group_children(void *context,void *image,void (*visit)(void *,void *),void *opaque){(void)context;klj_image_visit_dependencies(image,visit,opaque);}
static void *klj_group_public_symbol(void *image,const char *name){
    const KljLibraryGroupOperations ops={.contains=klj_group_contains,.symbol=klj_group_symbol,.children=klj_group_children};
    return klj_library_group_symbol(&ops,klj_default_library_namespace(),image,NULL,name);
}
extern void klj_image_visit_needed(kl_image *,void (*)(void *,const char *),void *);
struct klj_symbol_scope {KlImageRegistrySnapshot snapshot;KljLibraryNamespace *space;};
struct klj_needed_scope {struct klj_symbol_scope *scope;KljLibraryScopeVisit visit;void *context;};
static void *klj_scope_symbol(void *context,void *image,const char *name){
    (void)context;void *value=kl_sym(image,name);
    return value&&klj_run_initializers(image,NULL,NULL)?value:NULL;
}
static void *klj_scope_provider(void *context,const char *provider,const char *name){
    (void)context;(void)provider;
    /* These are the existing shim-served DT_NEEDED owners. Keep their ABI
     * adapters and current export surface; never search unrelated guest images. */
    return kl_shim_lookup(name);
}
static void klj_scope_needed(void *opaque,const char *name){
    struct klj_needed_scope *call=opaque;
    if(kl_dep_is_shim_served(name)){call->visit(call->context,NULL,name);return;}
    const char *base=basename_of(name);
    for(size_t i=0;i<call->scope->snapshot.count;i++){
        kl_image *image=call->scope->snapshot.entries[i].img;
        if(!klj_namespace_contains(call->scope->space,image))continue;
        const char *declared=klj_image_soname(image),*file=klj_image_path(image);
        if(!strcmp(call->scope->snapshot.entries[i].soname,base)||
           (declared&&!strcmp(declared,base))||(file&&!strcmp(basename_of(file),base))){
            call->visit(call->context,image,NULL);return;
        }
    }
}
static void klj_scope_children(void *context,void *image,KljLibraryScopeVisit visit,void *opaque){
    struct klj_needed_scope call={context,visit,opaque};
    klj_image_visit_needed(image,klj_scope_needed,&call);
}
static void *klj_scoped_image_symbol(kl_image *image,const char *name){
    struct klj_symbol_scope scope={klj_image_registry_snapshot(&klj_images),klj_default_library_namespace()};
    const KljLibraryScopeOperations ops={&scope,klj_scope_symbol,klj_scope_provider,klj_scope_children};
    return klj_library_scope_symbol(&ops,image,name);
}
static int klj_group_publish(void *context,const KlImageRegistryEntry *entries,void *const *spaces,size_t count){
    (void)context;
    /* The existing registry treats publication allocation failure as fatal;
     * no partially registered executable owner is returned to a caller. */
    for(size_t i=0;i<count;i++)if(klj_guest_mapping_claim(kl_base(entries[i].img),kl_span(entries[i].img),1))abort();
    pthread_mutex_lock(&g_lock);
    for(size_t i=0;i<count;i++) {
        klj_image_capture_identity(entries[i].img);
        if(!klj_namespace_publish(spaces[i],entries[i].img,1)){pthread_mutex_unlock(&g_lock);abort();}
    }
    int result=klj_library_registry_publish_group(&klj_images,entries,count);
    pthread_mutex_unlock(&g_lock);if(!result)abort();return 1;
}
struct klj_registry_prepare {int (*function)(kl_image *,void *);void *context;};
static int klj_registry_prepared(void *image,void *opaque){
    struct klj_registry_prepare *call=opaque;return !call->function||call->function(image,call->context);
}
kl_image *klj_load_image(const char *path,const char *soname,int (*prepare)(kl_image *,void *),void *context) {
    const KljLibraryImageOperations ops={.path=klj_registry_path,.map=klj_registry_map,.unmap=klj_registry_unmap,.validate=klj_registry_validate,.publish=klj_registry_publish};
    struct klj_registry_prepare call={prepare,context};
    if(klj_image_group_supported(path)){
        const KljLibraryGroupOperations group={.path=klj_registry_path,.contains=klj_group_contains,.map=klj_group_map,.unmap=klj_registry_unmap,
            .dependencies=klj_group_dependencies,.resolve=klj_group_resolve,.symbol=klj_group_symbol,.global=klj_group_contains,
            .relocate=klj_group_relocate,.dependencies_set=klj_group_dependencies_set,.validate=klj_registry_validate,.publish=klj_group_publish};
        return klj_library_group_load(&klj_mapping_lock,&klj_images,&group,klj_default_library_namespace(),path,soname,klj_registry_prepared,&call,klj_mapping_error,sizeof klj_mapping_error);
    }
    return klj_library_image_load(&klj_mapping_lock,&klj_images,&ops,path,soname,klj_registry_prepared,&call,klj_mapping_error,sizeof klj_mapping_error);
}
kl_image *kl_load_recursive(const char *path) {
    if(klj_image_group_supported(path)){
        kl_image *image=klj_load_image(path,path,NULL,NULL);
        return image&&klj_run_initializers(image,NULL,NULL)?image:NULL;
    }`);
 replace('    kl_image *img = kl_load_auto(path);\n    if (!img) return NULL;\n    kl_register_image(path, img);','    kl_image *img = klj_load_image(path,path,NULL,NULL);\n    if (!img) return NULL;');
 replace('    if (have) return have;','    if (have) return klj_run_initializers(have,NULL,NULL) ? have : NULL;');
 replace('    kl_run_init(img);','    if (!klj_run_initializers(img,NULL,NULL)) return NULL;');
 const loadedLookup=`    pthread_mutex_lock(&g_lock);
    kl_image *found = kl_find_image(path);
    pthread_mutex_unlock(&g_lock);
    if (found) {
        if (klj_run_initializers(found,NULL,NULL)) return found;
        snprintf(g_dlerr,sizeof g_dlerr,"dlopen failed: library initialization previously failed");
        return NULL;
    }`;
 replace('    pthread_mutex_lock(&g_lock);\n    kl_image *found = kl_find_image(path);           // already loaded? refcount is coarse\n    pthread_mutex_unlock(&g_lock);\n    if (found) return found;',
  '    // Android RTLD_NOLOAD is 4 for both guest widths; Darwin uses a different bit.\n    if (flags & 4) {\n        snprintf(g_dlerr,sizeof g_dlerr,"dlopen failed: library \\"%s\\" not loaded",basename_of(path));\n        return NULL;\n    }');
 replace('    if (!path) return (void *)-1;                    // RTLD_DEFAULT-ish: whole process',
  '    if (!path) return (void *)-1;                    // RTLD_DEFAULT-ish: whole process\n'+loadedLookup);
 // Public dlsym waits for a matching image; internal relocation lookup remains
 // available while dependencies and same-thread constructors are being linked.
 const lookupStart=adapted.indexOf('void *klb_dlsym(void *handle, const char *name) {'),lookupEnd=adapted.indexOf('// ---------------------------------------------------------------------------',lookupStart);
 if(lookupStart<0||lookupEnd<0)throw Error('Pinned public native lookup changed');
 let lookup=adapted.slice(lookupStart,lookupEnd);
 lookup=lookup.replace('        s = kl_guest_sym_global(name);',`        for(int i=0;i<g_nimgs;i++) {
            s=kl_sym(g_imgs[i].img,name);
            if(s && klj_run_initializers(g_imgs[i].img,NULL,NULL))break;
            s=NULL;
        }`)
  .replace('            if (v) return v;','            if (v && klj_run_initializers(g_imgs[i].img,NULL,NULL)) return v;')
  .replace('    void *v = kl_sym((kl_image *)handle, name);','    if (!klj_run_initializers((kl_image *)handle,NULL,NULL)) { snprintf(g_dlerr,sizeof g_dlerr,"dlsym failed: library initialization previously failed");return NULL; }\n    void *v = klj_image_format_payload(handle,1)?klj_group_public_symbol(handle,name):klj_scoped_image_symbol((kl_image *)handle,name);');
 adapted=adapted.slice(0,lookupStart)+lookup+adapted.slice(lookupEnd);
 // A native import can belong to a retained Darwin image rather than a guest
 // ELF. Return that real provider's metadata after guest ownership lookup.
 const addressStart=adapted.indexOf('int klb_dladdr(const void *addr, kl_dl_info *info) {'),addressEnd=adapted.indexOf('\n}',addressStart)+2;
 if(addressStart<0||addressEnd<2)throw Error('Pinned native address ownership lookup changed');
 const addressLookup=adapted.slice(addressStart,addressEnd);
 if(addressLookup.split('    return 0;').length!==2)throw Error('Pinned native address ownership fallback changed');
 adapted=adapted.slice(0,addressStart)+addressLookup.replace('    return 0;',`    Dl_info native;
    if (!dladdr(addr,&native)) return 0;
    info->dli_fname=native.dli_fname;
    info->dli_fbase=native.dli_fbase;
    info->dli_sname=native.dli_sname;
    info->dli_saddr=native.dli_saddr;
    return 1;`)+adapted.slice(addressEnd);
 // Capture the table and that table's published count together. Growth cannot
 // pair an old allocation with a newer, larger count in unwind callbacks.
 for(const signature of [
  'int kl_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data) {',
  'kl_image *kl_find_image(const char *soname) {',
  'void *kl_guest_sym_global(const char *name) {',
  'const char *kl_addr_image(const void *addr, size_t *offset) {',
  'void kl_dl_report_images(FILE *f) {',
  'void *klb_dlsym(void *handle, const char *name) {',
  'int klb_dladdr(const void *addr, kl_dl_info *info) {'
 ])replace(signature,signature+'\n    KlImageRegistrySnapshot snapshot=klj_image_registry_snapshot(&klj_images);\n    const KlImageRegistryEntry *g_imgs=snapshot.entries;\n    size_t g_nimgs=snapshot.count;');
 adapted=adapted.replaceAll('for (int i = 0; i < g_nimgs','for (size_t i = 0; i < g_nimgs').replaceAll('for(int i=0;i<g_nimgs','for(size_t i=0;i<g_nimgs').replace('    int n = g_nimgs;','    size_t n = g_nimgs;').replace('for (int i = 0; i < n; i++) {\n        unsigned phnum','for (size_t i = 0; i < n; i++) {\n        unsigned phnum');
 adapted+=`\nextern int klj_image_tls_symbol(kl_image *,const char *,KljElfTlsIndex *);
int klj_guest_tls_symbol(const char *name,KljElfTlsIndex *result) {
    KlImageRegistrySnapshot snapshot=klj_image_registry_snapshot(&klj_images);
    for(size_t n=0;n<snapshot.count;n++)if(klj_image_tls_symbol(snapshot.entries[n].img,name,result))return 1;
    return 0;
}
int klj_proc_maps_image(uint64_t address,KljProcMapFile *result) {
    KlImageRegistrySnapshot snapshot=klj_image_registry_snapshot(&klj_images);
    uint64_t best=UINT64_MAX;int found=0;
    for(size_t index=0;index<snapshot.count;index++) {
        kl_image *image=snapshot.entries[index].img;
        uint64_t page=klj_image_format_payload(image,1)?4096:(uint64_t)getpagesize();
        const char *file=klj_image_path(image);char guest[4096];
        if(!klj_guest_path_of_host(file,guest,sizeof guest))continue;
        unsigned count=0;const KlProgramHeader *headers=kl_phdrs(image,&count);
        KljProcMapFile candidate;
        if(!klj_proc_maps_elf_file((uintptr_t)kl_base(image),headers,count,page,file,guest,address,&candidate))continue;
        uint64_t at=candidate.start>address?candidate.start:address;
        if(at<best){*result=candidate;best=at;found=1;}
    }
    return found;
}\n`;
 adapted+='\n#include '+JSON.stringify(path.join(import.meta.dirname,'image-pool-lease.h'))+`\n
extern size_t klj_image_gap_pools(kl_image *,KljImagePoolLease *,size_t);
size_t klj_image_gap_pool_leases(uintptr_t address,size_t size,KljImagePoolLease *leases,size_t capacity){
    if(size>UINTPTR_MAX-address)return 0;
    KlImageRegistrySnapshot snapshot=klj_image_registry_snapshot(&klj_images);
    for(size_t i=0;i<snapshot.count;i++){
        kl_image *image=snapshot.entries[i].img;uintptr_t base=(uintptr_t)kl_base(image);
        if(address>=base&&address-base<kl_span(image)&&size<=kl_span(image)-(address-base))
            return klj_image_gap_pools(image,leases,capacity);
    }
    return 0;
}\n`;
 adapted=adaptRegistryBinding(adapted,root);
 const file=path.join(build,'kl_dl.c');fs.writeFileSync(file,adapted);return file;
}
