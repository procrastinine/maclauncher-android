static void klj_reader_retain(void*pointer){
    klm_reader*r=pointer;pthread_mutex_lock(&klj_readers_lock);r->refs++;pthread_mutex_unlock(&klj_readers_lock);
}
static void klj_reader_release(void*pointer){
    klm_reader*r=pointer;pthread_mutex_lock(&klj_readers_lock);
    if(--r->refs){pthread_mutex_unlock(&klj_readers_lock);return;}
    klj_reader_link**link=&klj_readers;while(*link&&(*link)->reader!=r)link=&(*link)->next;
    if(*link){klj_reader_link*old=*link;*link=old->next;free(old);}
    g_life_published+=r->n_queued;g_life_acquired+=r->n_acquired;pthread_mutex_unlock(&klj_readers_lock);
    while(r->count){CVPixelBufferRelease((CVPixelBufferRef)r->q[r->head].pb);r->head=(r->head+1)%KLM_READER_MAX;r->count--;}
    kl_ndk_window_free(r->window);pthread_mutex_destroy(&r->lock);pthread_cond_destroy(&r->cv);r->magic=0;free(r);
}
static void klm_AImageReader_delete(void*pointer){
    klm_reader*r=pointer;if(!r)return;
    pthread_mutex_lock(&r->lock);r->running=0;r->have_listener=0;pthread_cond_broadcast(&r->cv);pthread_mutex_unlock(&r->lock);
    if(r->thread_started&&!pthread_equal(pthread_self(),r->dispatch))pthread_join(r->dispatch,NULL);
    klj_reader_release(r);
}
