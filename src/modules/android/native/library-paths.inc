/* Included by the existing image registry. Java and native callers share this
 * path lookup; it does not load images or maintain another library registry. */
static unsigned klj_library_elf_class=2;
unsigned klj_library_word_bytes(void){return klj_library_elf_class==1?4:8;}
static int kl_dep_is_shim_served(const char *name);
static KljLibraryNamespaces *klj_library_namespaces;
static KljLibraryNamespace *klj_library_namespace;
static pthread_once_t klj_library_namespace_once=PTHREAD_ONCE_INIT;
static void klj_create_library_namespace(void){
    klj_library_namespaces=klj_namespaces_create();
    if(klj_library_namespaces){
        char paths[sizeof g_libdir+32];
        snprintf(paths,sizeof paths,"%s:/system/%s",g_libdir,klj_library_elf_class==1?"lib":"lib64");
        klj_library_namespace=klj_namespace_create(klj_library_namespaces,"default",NULL,paths,0,NULL,NULL,NULL);
        klj_namespace_export(klj_library_namespace);
    }
}
static KljLibraryNamespace *klj_default_library_namespace(void){
    pthread_once(&klj_library_namespace_once,klj_create_library_namespace);
    return klj_library_namespace;
}
static void klj_update_library_namespace(void){
    char paths[sizeof g_libdir+32];
    snprintf(paths,sizeof paths,"%s:/system/%s",g_libdir,klj_library_elf_class==1?"lib":"lib64");
    (void)klj_namespace_paths(klj_default_library_namespace(),NULL,paths);
}
void klj_set_library_elf_class(unsigned value) {
    if(value==1||value==2){klj_library_elf_class=value;klj_update_library_namespace();}
}
static int klj_library_candidate(void *unused,const char *name,char *out,size_t capacity) {
    (void)unused;char mapped[PATH_MAX];
    const char *translated=klj_mapped_guest_path(name,mapped,sizeof mapped);
    if(translated)name=translated;
    if(!name||strlen(name)>=capacity)return 0;
    if(!kl_can_load(name))return 0;
    /* Translated Mach-O containers remain supported by kl_load_auto. Raw ELF
     * files must match the configured application ABI. */
    FILE *file=fopen(name,"rb");unsigned char header[5]={0};
    if(file){size_t count=fread(header,1,sizeof header,file);fclose(file);
        if(count==sizeof header&&!memcmp(header,"\177ELF",4)&&header[4]!=klj_library_elf_class)return 0;
    }
    memcpy(out,name,strlen(name)+1);return 1;
}
int klj_resolve_library_path(const char *name,const char *requester,char *out,size_t capacity) {
    if(!name||!*name||!out||!capacity)return 0;
    const char *base=strrchr(name,'/');base=base?base+1:name;
    if(kl_dep_is_shim_served(base))return 0;
    char origin[PATH_MAX]={0};
    if(requester){
        const char *slash=strrchr(requester,'/');
        if(slash&&(size_t)(slash-requester)<sizeof origin){memcpy(origin,requester,(size_t)(slash-requester));origin[slash-requester]=0;}
    }
    return klj_namespace_resolve(klj_default_library_namespace(),name,origin,
        klj_library_candidate,NULL,out,capacity)!=NULL;
}
