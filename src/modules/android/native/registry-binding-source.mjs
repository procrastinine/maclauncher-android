import path from 'node:path';

/** Preserve Bionic's distinct relocation and dlsym scopes around kl_image. */
export function adaptRegistryBinding(input,root){
 let source='#include '+JSON.stringify(path.join(root,'image-startup.h'))+'\n#include <stdatomic.h>\n#include <limits.h>\n#include '+JSON.stringify(path.join(root,'library-binding.h'))+'\n'+input;
 const replace=(before,after)=>{if(source.split(before).length!==2)throw Error('Native binding scope anchor changed: '+before);source=source.replace(before,after);};
 replace('static KlImageRegistry klj_images;',`static KlImageRegistry klj_images;
static _Atomic unsigned klj_target_sdk;
static _Thread_local unsigned klj_loading_flags;
static char klj_main_program_handle;
static int klj_image_publication_global(kl_image *);
static int klj_group_global(void *,void *,void *);
static void klj_leave_loading_flags(unsigned *previous){klj_loading_flags=*previous;}`);
 replace('klj_namespace_publish(klj_default_library_namespace(),img,1)','klj_namespace_publish(klj_default_library_namespace(),img,klj_image_publication_global(img))');
 replace('klj_namespace_publish(spaces[i],entries[i].img,1)','klj_namespace_publish(spaces[i],entries[i].img,klj_image_publication_global(entries[i].img))');
 replace('struct klj_symbol_scope {KlImageRegistrySnapshot snapshot;KljLibraryNamespace *space;};',
  'struct klj_symbol_scope {KlImageRegistrySnapshot snapshot;KljLibraryNamespace *space;void *skip_until;int initialize;};');
 replace(`    (void)context;void *value=kl_sym(image,name);
    return value&&klj_run_initializers(image,NULL,NULL)?value:NULL;`,
 `    struct klj_symbol_scope *scope=context;
    if(scope->skip_until){if(scope->skip_until==image)scope->skip_until=NULL;return NULL;}
    void *value=kl_sym(image,name);
    return value&&(!scope->initialize||klj_run_initializers(image,NULL,NULL))?value:NULL;`);
 replace('struct klj_symbol_scope scope={klj_image_registry_snapshot(&klj_images),klj_default_library_namespace()};',
  'struct klj_symbol_scope scope={klj_image_registry_snapshot(&klj_images),klj_default_library_namespace(),NULL,1};');
 const helpers=`unsigned klj_library_target_sdk(void){return atomic_load_explicit(&klj_target_sdk,memory_order_acquire);}
int klj_library_set_target_sdk(unsigned sdk){
    if(!sdk || sdk>INT_MAX)return 0;
    pthread_mutex_lock(&g_lock);unsigned current=klj_library_target_sdk();
    int ready=current==sdk || (!current && !klj_image_registry_snapshot(&klj_images).count);
    if(ready)atomic_store_explicit(&klj_target_sdk,sdk,memory_order_release);
    pthread_mutex_unlock(&g_lock);return ready;
}
#include ${JSON.stringify(path.join(root,'image-startup.inc'))}
static int klj_image_publication_global(kl_image *image){
    unsigned flags=klj_loading_flags;
    if(klj_image_dt_flags_1(image)&2)flags|=0x100; /* DF_1_GLOBAL implies RTLD_GLOBAL. */
    klj_image_set_rtld_flags(image,flags);
    return (klj_image_rtld_flags(image)&0x100)!=0;
}
static int klj_group_global(void *unused,void *space,void *image){
    (void)unused;return klj_namespace_contains(space,image) && (klj_image_dt_flags_1(image)&2)!=0;
}
static void klj_group_root(void *unused,void *image,void *root){(void)unused;klj_image_set_local_group(image,root);}
void *klj_guest_sym_for_image(void *opaque,const char *name){
    struct klj_symbol_scope scope={klj_image_registry_snapshot(&klj_images),klj_default_library_namespace(),NULL,0};
    for(size_t i=0;i<scope.snapshot.count;i++){
        kl_image *image=scope.snapshot.entries[i].img;
        if(klj_group_global(NULL,scope.space,image)){void *value=kl_sym(image,name);if(value)return value;}
    }
    const KljLibraryScopeOperations ops={&scope,klj_scope_symbol,klj_scope_provider,klj_scope_children};
    return klj_library_scope_symbol(&ops,opaque,name);
}
static void *klj_registry_default_symbol(const void *address,void *handle,const char *name){
    struct klj_symbol_scope scope={klj_image_registry_snapshot(&klj_images),klj_default_library_namespace(),NULL,1};
    kl_image *caller=NULL;size_t first=0;
    for(size_t i=0;i<scope.snapshot.count;i++){
        kl_image *image=scope.snapshot.entries[i].img;uintptr_t base=(uintptr_t)kl_base(image),pc=(uintptr_t)address;
        if(pc>=base && pc-base<kl_span(image)){caller=image;if(handle==(void *)-1)first=i+1;break;}
    }
    if(handle==(void *)-1 && !caller)return NULL;
    void *value=kl_shim_lookup(name);if(value)return value;
    for(size_t i=first;i<scope.snapshot.count;i++){
        kl_image *image=scope.snapshot.entries[i].img;unsigned sdk=klj_image_target_sdk(image);
        if(!klj_namespace_contains(scope.space,image) ||
           (!(klj_image_rtld_flags(image)&0x100) && !(sdk>0 && sdk<23)))continue;
        value=klj_scope_symbol(&scope,image,name);if(value)return value;
    }
    if(!caller)return NULL;
    if(handle==(void *)-1)scope.skip_until=caller;
    const KljLibraryScopeOperations ops={&scope,klj_scope_symbol,klj_scope_provider,klj_scope_children};
    return klj_library_scope_symbol(&ops,klj_image_local_group(caller),name);
}
`;
 replace('static int klj_group_publish(',helpers+'static int klj_group_publish(');
 replace('.global=klj_group_contains,','.global=klj_group_global,');
 replace('.publish=klj_group_publish};','.publish=klj_group_publish,.group_root=klj_group_root};');
 replace('        v = kl_sym(g_imgs[i].img, name);','        if(klj_group_global(NULL,klj_default_library_namespace(),g_imgs[i].img))v=kl_sym(g_imgs[i].img,name);');
 replace('void *klb_dlopen(const char *path, int flags) {',`void *klb_dlopen(const char *path, int flags) {
    unsigned previous __attribute__((cleanup(klj_leave_loading_flags)))=klj_loading_flags;
    klj_loading_flags=(unsigned)flags;`);
 replace('    if (!path) return (void *)-1;                    // RTLD_DEFAULT-ish: whole process',
  '    if (!path) return &klj_main_program_handle;');
 replace('int klb_dladdr(const void *addr, kl_dl_info *info) {',
  'extern const char *klj_image_guest_path(kl_image *);\nint klb_dladdr(const void *addr, kl_dl_info *info) {');
 replace('            info->dli_fname = g_imgs[i].soname;',
  '            const char *guest=klj_image_guest_path(g_imgs[i].img);\n            info->dli_fname = guest?guest:g_imgs[i].soname;');
 const start=source.indexOf('    if (handle == NULL || handle == (void *)-1)'),end=source.indexOf('    // Anything left',start);
 if(start<0||end<0)throw Error('Native default lookup anchor changed');
 source=source.slice(0,start)+`    if(handle==&klj_main_program_handle || handle==NULL || handle==(void *)-1){
        /* The process handle has global scope, independent of the dlsym caller.
         * Keep the actual RTLD_NEXT sentinel relative to its guest caller. */
        int main_program=handle==&klj_main_program_handle;
        void *value=klj_registry_default_symbol(main_program?NULL:__builtin_return_address(0),main_program?NULL:handle,name);
        if(value)return value;
        snprintf(g_dlerr,sizeof g_dlerr,"klepton: undefined symbol: %s",name);return klb_dlsym_null(name);
    }
`+source.slice(end);
 return source;
}
