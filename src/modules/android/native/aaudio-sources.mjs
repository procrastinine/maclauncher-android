/** Extend the original stream objects at their native ownership boundary. */
export function adaptAAudioOwnership(source){
 const replace=(from,to)=>{if(source.split(from).length!==2)throw Error('Original AAudio ownership boundary changed: '+from.slice(0,80));source=source.replace(from,to);};
 const callbacks='    klaa_error_cb error_cb;  void *error_user;';
 if(source.split(callbacks).length!==3)throw Error('Original AAudio callback owners changed');
 source=source.replaceAll(callbacks,callbacks+'\n    KljAAudioCallback owned_callbacks[3];\n    void (*presentation_cb)(void *,void *); void *presentation_user;\n    int32_t content_spatialized;');
 replace('    int32_t buffer_size;', '    int32_t content_type,usage,privacy_sensitive,capture_policy;\n    int64_t frames_read_seen;\n    _Atomic int resources_released;\n    int32_t buffer_size;');
 replace('} klaa_stream;', '} klaa_stream;\nstatic aaudio_result_t owned_aaudio_release(klaa_stream *s);');
 replace('    unsigned long callbacks, frames;', '    _Atomic unsigned long callbacks, frames;');
 replace('    b->sharing_mode     = 1;               // AAUDIO_SHARING_MODE_SHARED', '    b->sharing_mode     = 1;               // AAUDIO_SHARING_MODE_SHARED\n    b->content_type=2; b->usage=1; b->capture_policy=1;\n    b->privacy_sensitive=-1;');
 replace('static void klaa_setIsContentSpatialized(klaa_builder *b, uint8_t v)    { (void)b; (void)v; }', 'static void klaa_setIsContentSpatialized(klaa_builder *b, uint8_t v)    { if(b)b->content_spatialized=!!v; }');
 const begin=source.indexOf('static aaudio_result_t klaa_close(klaa_stream *s) {'),end=source.indexOf('static int32_t klaa_getFramesPerBurst',begin);
 if(begin<0||end<0)throw Error('Original AAudio close boundary changed');
 source=source.slice(0,begin)+`static aaudio_result_t klaa_close(klaa_stream *s) {
    aaudio_result_t result=owned_aaudio_release(s);
    if(result!=AAUDIO_OK)return result;
    pthread_mutex_destroy(&s->lock); free(s); return AAUDIO_OK;
}

`+source.slice(end);
 for(const name of ['klaa_requestStart','klaa_requestStop']){
  replace(`static aaudio_result_t ${name}(klaa_stream *s) {\n    if (!s) return AAUDIO_ERROR_NULL;`, `static aaudio_result_t ${name}(klaa_stream *s) {\n    if (!s) return AAUDIO_ERROR_NULL;\n    pthread_mutex_lock(&s->lock);\n    int unavailable=s->resources_released||owned_feeding==s;\n    pthread_mutex_unlock(&s->lock);\n    if(unavailable)return AAUDIO_ERROR_INVALID_STATE;`);
 }
 const startGuard='    if(unavailable)return AAUDIO_ERROR_INVALID_STATE;';
 const startAt=source.indexOf('static aaudio_result_t klaa_requestStart('),guardAt=source.indexOf(startGuard,startAt);
 if(guardAt<0)throw Error('Original AAudio start guard changed');
 source=source.slice(0,guardAt)+source.slice(guardAt).replace(startGuard,startGuard+`\n    // A callback can return STOP while its joinable feeder still owns a thread.\n    pthread_mutex_lock(&s->lock);\n    int reap=s->thread_live&&s->state==AAUDIO_STREAM_STATE_STOPPED;\n    pthread_mutex_unlock(&s->lock);\n    if(reap)klaa_stop_join(s);`);
 replace('if (s->direction != AAUDIO_DIRECTION_INPUT) kl_audio_pause();', 'if (s->direction != AAUDIO_DIRECTION_INPUT && g_open_streams==1) kl_audio_pause();');
 replace('if (s->direction != AAUDIO_DIRECTION_INPUT) kl_audio_flush();', 'if (s->direction != AAUDIO_DIRECTION_INPUT && g_open_streams==1) kl_audio_flush();');
 // Read/write on a released stream must not reach retired capture/staging data.
 source=source.replaceAll('if (!s || !buffer) return AAUDIO_ERROR_NULL;', 'if (!s || !buffer) return AAUDIO_ERROR_NULL;\n    if(s->resources_released)return AAUDIO_ERROR_INVALID_STATE;');
 return source;
}
