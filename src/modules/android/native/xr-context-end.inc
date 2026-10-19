/* All registry mutations occur after the real ANGLE operation succeeds. A
 * missing/overflowed or unrelated sharegroup is never treated as equivalent. */
int klj_glfb_configure(void *(*resolver)(const char *)) {
    if(!resolver)return 0;
    pthread_mutex_lock(&klj_glfb_lock);
    int okay=klj_glfb_resolver==resolver||(!klj_glfb_resolver&&!g_ready);
    if(okay)klj_glfb_resolver=resolver;
    pthread_mutex_unlock(&klj_glfb_lock);return okay;
}
int klj_glfb_init_owned(void) {
    pthread_mutex_lock(&klj_glfb_init_lock);
    pthread_mutex_lock(&klj_glfb_lock);
    /* Never replace a live guest-owned context or its sharegroup. */
    if(g_ready){int owned=klj_glfb_owns_context;pthread_mutex_unlock(&klj_glfb_lock);pthread_mutex_unlock(&klj_glfb_init_lock);return owned;}
    for(int i=0;i<128;i++)if(klj_contexts[i].context){pthread_mutex_unlock(&klj_glfb_lock);pthread_mutex_unlock(&klj_glfb_init_lock);return 0;}
    klj_glfb_owns_context=1;
    pthread_mutex_unlock(&klj_glfb_lock);
    /* Upstream initialization can run optional diagnostic GL/fence work;
     * never hold the context registry lock across those calls. */
    int ready=kl_glfb_init();
    if(!ready)klj_glfb_owns_context=0;
    pthread_mutex_unlock(&klj_glfb_init_lock);return ready;
}
void klj_glfb_context_created(void *display,void *context,void *shared) {
    if(!context)return;
    pthread_mutex_lock(&klj_glfb_lock);
    uint64_t group=0;int freeSlot=-1;
    for(int i=0;i<128;i++){
        if(klj_contexts[i].context==shared&&klj_contexts[i].display==display)group=klj_contexts[i].group;
        if(!klj_contexts[i].context&&freeSlot<0)freeSlot=i;
    }
    if(!shared)group=klj_next_group++;
    if(group&&freeSlot>=0)klj_contexts[freeSlot]=(typeof(klj_contexts[0])){context,display,group};
    pthread_mutex_unlock(&klj_glfb_lock);
}
void klj_glfb_context_destroyed(void *context) {
    pthread_mutex_lock(&klj_glfb_lock);
    for(int i=0;i<128;i++)if(klj_contexts[i].context==context){if(klj_contexts[i].current)klj_contexts[i].destroyPending=1;else memset(&klj_contexts[i],0,sizeof klj_contexts[i]);}
    pthread_mutex_unlock(&klj_glfb_lock);
}
void klj_glfb_current(void *display,void *context);
static void klj_binding_exit(void *unused){(void)unused;klj_glfb_current(NULL,NULL);}
static void klj_binding_create(void){pthread_key_create(&klj_binding_key,klj_binding_exit);}
void klj_glfb_current(void *display,void *context) {
    pthread_once(&klj_binding_once,klj_binding_create);
    pthread_mutex_lock(&klj_glfb_lock);
    if(context!=klj_current_context){
        for(int i=0;i<128;i++)if(klj_contexts[i].context==klj_current_context&&klj_current_context){
            if(klj_contexts[i].current)--klj_contexts[i].current;
            if(!klj_contexts[i].current&&klj_contexts[i].destroyPending)memset(&klj_contexts[i],0,sizeof klj_contexts[i]);
        }
        for(int i=0;i<128;i++)if(klj_contexts[i].context==context&&context)++klj_contexts[i].current;
    }
    klj_current_context=context;klj_current_display=context?display:NULL;
    pthread_setspecific(klj_binding_key,context);
    pthread_mutex_unlock(&klj_glfb_lock);
}
static int klj_projection_current(void) {
    if(!klj_glfb_resolver||klj_glfb_owns_context)return g_ready;
    void *(*current)(void)=asym("eglGetCurrentContext");
    void *(*display)(void)=asym("eglGetCurrentDisplay");
    if(!current||!display||!klj_current_context||current()!=klj_current_context||display()!=klj_current_display)return 0;
    pthread_mutex_lock(&klj_glfb_lock);
    uint64_t group=0;
    for(int i=0;i<128;i++)if(klj_contexts[i].context==klj_current_context&&klj_contexts[i].display==klj_current_display)group=klj_contexts[i].group;
    int okay=group&&(!klj_projection_group||klj_projection_group==group)&&(!g_dpy||g_dpy==klj_current_display);
    if(okay&&!g_ready){
        a_eglGetProcAddress=asym("eglGetProcAddress");a_glGetError=asym("glGetError");
        a_glReadPixels=asym("glReadPixels");a_glFinish=asym("glFinish");a_glGetIntegerv=asym("glGetIntegerv");
        a_glCheckFramebufferStatus=asym("glCheckFramebufferStatus");
        a_glGetFramebufferAttachmentParameteriv=asym("glGetFramebufferAttachmentParameteriv");
        a_glGetString=asym("glGetString");
        okay=a_eglGetProcAddress&&a_glGetIntegerv&&a_glCheckFramebufferStatus;
        if(okay){g_dpy=klj_current_display;g_ctx=klj_current_context;klj_projection_group=group;g_ready=1;}
    }
    pthread_mutex_unlock(&klj_glfb_lock);return okay;
}
void klj_glfb_consumer(void *event) {
    pthread_mutex_lock(&klj_glfb_lock);klj_consumer_event=event;pthread_mutex_unlock(&klj_glfb_lock);
}
uint64_t klj_glfb_retire_consumer(void) {
    pthread_mutex_lock(&klj_glfb_lock);klj_consumer_event=NULL;
    uint64_t value=kl_glfb_gpu_fence_value();pthread_mutex_unlock(&klj_glfb_lock);return value;
}
static void klj_wait_consumer(uint64_t value) {
    pthread_mutex_lock(&klj_glfb_lock);
    if(klj_consumer_event){
        unsigned (*wait)(void*,void*,int)=asym("eglWaitSync");
        const intptr_t attrs[]={0x34D9,(intptr_t)klj_consumer_event,0x34DA,(intptr_t)(uint32_t)value,0x34DB,(intptr_t)(uint32_t)(value>>32),0x30F8,0x34DC,0x3038};
        void *next=a_eglCreateSync(g_dpy,0x34D8,attrs);
        /* ANGLE's queue waits before any later writes. No CPU fence wait. */
        if(!next||!wait||!wait(g_dpy,next,0)){
            fprintf(stderr,"[android-xr] ANGLE could not order projection texture reuse\n");
            if(next&&a_eglDestroySync)a_eglDestroySync(g_dpy,next);
            /* Refuse subsequent frames rather than sample unordered storage. */
            klj_consumer_failed=1;klj_consumer_event=NULL;
        }else{
            if(klj_consumer_sync&&a_eglDestroySync)a_eglDestroySync(g_dpy,klj_consumer_sync);
            klj_consumer_sync=next;
        }
    }
    pthread_mutex_unlock(&klj_glfb_lock);
}
int klj_glfb_consumer_failed(void){pthread_mutex_lock(&klj_glfb_lock);int failed=klj_consumer_failed;pthread_mutex_unlock(&klj_glfb_lock);return failed;}
void *klj_glfb_display(void){pthread_mutex_lock(&klj_glfb_lock);void *display=g_dpy;pthread_mutex_unlock(&klj_glfb_lock);return display;}
