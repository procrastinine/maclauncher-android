typedef struct {
    KljImageStartup value;
    char **reference_argv, **reference_envp;
    size_t envc;
} KljStartupStorage;
static _Atomic(KljStartupStorage *) klj_startup;
static void klj_startup_vector_free(char **vector) {
    if(vector){for(size_t i=0;vector[i];i++)free(vector[i]);free(vector);}
}
static char **klj_startup_vector(size_t count,const char *const *values) {
    if(!values || count>SIZE_MAX/sizeof(char *)-1 || values[count])return NULL;
    char **copy=calloc(count+1,sizeof(char *));if(!copy)return NULL;
    for(size_t i=0;i<count;i++)if(!values[i] || !(copy[i]=strdup(values[i]))){klj_startup_vector_free(copy);return NULL;}
    return copy;
}
static void klj_startup_free(KljStartupStorage *value) {
    if(!value)return;
    klj_startup_vector_free(value->value.argv);klj_startup_vector_free(value->value.envp);
    klj_startup_vector_free(value->reference_argv);klj_startup_vector_free(value->reference_envp);free(value);
}
static int klj_startup_same(KljStartupStorage *value,int argc,const char *const *argv,size_t envc,const char *const *envp) {
    if(value->value.argc!=argc || value->envc!=envc)return 0;
    for(int i=0;i<argc;i++)if(strcmp(value->reference_argv[i],argv[i]))return 0;
    for(size_t i=0;i<envc;i++)if(strcmp(value->reference_envp[i],envp[i]))return 0;
    return 1;
}
int klj_library_set_startup(int argc,const char *const *argv,size_t envc,const char *const *envp) {
    if(argc<1 || !argv || !argv[0] || !*argv[0] || envc>64 || !envp)return 0;
    for(int i=0;i<argc;i++)if(!argv[i])return 0;
    if(argv[argc] || envp[envc])return 0;
    size_t bytes=0;
    for(size_t i=0;i<envc;i++){
        const unsigned char *text=(const unsigned char *)envp[i];if(!text)return 0;
        size_t length=strnlen((const char *)text,16385);
        if(length>=16384 || length+1>16384-bytes)return 0;
        bytes+=length+1;size_t at=0;
        for(;text[at] && text[at]!='=';at++)
            if(!((text[at]>='A'&&text[at]<='Z') || (text[at]>='a'&&text[at]<='z') || text[at]=='_' || (at&&text[at]>='0'&&text[at]<='9')))return 0;
        if(!at || text[at]!='=')return 0;
        for(size_t j=0;j<i;j++)if(!strncmp(envp[j],(const char *)text,at) && envp[j][at]=='=')return 0;
    }
    KljStartupStorage *next=calloc(1,sizeof(*next));if(!next)return 0;
    next->value.argc=argc;next->envc=envc;
    next->value.argv=klj_startup_vector((size_t)argc,argv);next->value.envp=klj_startup_vector(envc,envp);
    next->reference_argv=klj_startup_vector((size_t)argc,argv);next->reference_envp=klj_startup_vector(envc,envp);
    if(!next->value.argv||!next->value.envp||!next->reference_argv||!next->reference_envp){klj_startup_free(next);return 0;}
    pthread_mutex_lock(&g_lock);
    KljStartupStorage *current=atomic_load_explicit(&klj_startup,memory_order_acquire);
    int ready=current?klj_startup_same(current,argc,argv,envc,envp):!klj_image_registry_snapshot(&klj_images).count;
    if(ready&&!current){atomic_store_explicit(&klj_startup,next,memory_order_release);next=NULL;}
    pthread_mutex_unlock(&g_lock);klj_startup_free(next);return ready;
}
const KljImageStartup *klj_library_startup(void) {
    KljStartupStorage *value=atomic_load_explicit(&klj_startup,memory_order_acquire);
    return value?&value->value:NULL;
}
