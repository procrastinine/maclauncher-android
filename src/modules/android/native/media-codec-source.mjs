import fs from 'node:fs';
import path from 'node:path';
function replace(source,before,after){if(source.split(before).length!==2)throw Error('Pinned native media consumer changed: '+before.slice(0,80));return source.replace(before,after);}
export function mediaCodecSource(source){
  // Retire the complete former codec object, methods and selected-symbol table.
  // Java and NDK clients now enter the same retained VideoToolbox owner.
  const begin=source.indexOf('// AMediaCodec\n'),end=source.indexOf('// ATrace.',begin);
  if(begin<0||end<begin)throw Error('Pinned native codec implementation changed');
  source=source.slice(0,begin)+source.slice(end);
  source=source.replace(/^\s*M\("AMediaCodec_[^\n]+\n/gm,'');
  source=replace(source,'static unsigned g_life_in, g_life_out, g_life_rendered, g_life_published, g_life_acquired;','static unsigned g_life_published, g_life_acquired;');
  const report=source.indexOf('void kl_mediandk_report(FILE *f) {'),reportEnd=source.indexOf('// The table, and the two doors into it.',report);
  if(report<0||reportEnd<report)throw Error('Pinned native codec report changed');
  source=source.slice(0,report)+`void kl_mediandk_report(FILE *f) {
    pthread_mutex_lock(&klj_readers_lock);
    if(g_ever_created)fprintf(f,"\\n=== media image consumers ===\\n  %u frames published, %u acquired\\n",g_life_published,g_life_acquired);
    pthread_mutex_unlock(&klj_readers_lock);
    kl_vtdec_report(f);
}
\n`+source.slice(reportEnd);
  source='#include '+JSON.stringify(path.join(import.meta.dirname,'media-surface.h'))+'\n'+source;
  source=replace(source,'    int       pending;          // listener calls owed to the guest','    int       pending;          // listener calls owed to the guest\n    unsigned refs;int thread_started,in_callback;');
  // Window identity is resolved under the existing consumer lifetime, never by
  // casting arbitrary tracked native-window pointers into the reader layout.
  source=replace(source,'static klm_reader *g_reader;        // for the report; this guest makes one',`typedef struct klj_reader_link {klm_reader *reader;struct klj_reader_link *next;} klj_reader_link;
static pthread_mutex_t klj_readers_lock=PTHREAD_MUTEX_INITIALIZER;
static klj_reader_link *klj_readers;
static void klj_reader_release(void*);
static void klj_reader_retain(void*);`);
  source=replace(source,'        int have = r->have_listener;','        int have = r->have_listener;\n        r->in_callback=have&&l.onImageAvailable;');
  source=replace(source,'        pthread_mutex_lock(&r->lock);\n    }\n    pthread_mutex_unlock(&r->lock);\n    return NULL;','        pthread_mutex_lock(&r->lock);\n        r->in_callback=0;pthread_cond_broadcast(&r->cv);\n    }\n    pthread_mutex_unlock(&r->lock);\n    klj_reader_release(r);\n    return NULL;');
  const createStart=source.indexOf('    r->running = 1;'),createEnd=source.indexOf('    fprintf(stderr, "  [media] AImageReader %dx%d',createStart);
  if(createStart<0||createEnd<createStart)throw Error('Pinned native reader creation changed');
  source=source.slice(0,createStart)+`    if(!r->window){pthread_mutex_destroy(&r->lock);pthread_cond_destroy(&r->cv);free(r);return AMEDIA_ERROR_UNKNOWN;}
    klj_reader_link *link=calloc(1,sizeof *link);if(!link){kl_ndk_window_free(r->window);pthread_mutex_destroy(&r->lock);pthread_cond_destroy(&r->cv);free(r);return AMEDIA_ERROR_UNKNOWN;}
    r->refs=2;r->running=1;link->reader=r;
    pthread_mutex_lock(&klj_readers_lock);link->next=klj_readers;klj_readers=link;g_ever_created=1;pthread_mutex_unlock(&klj_readers_lock);
    if(pthread_create(&r->dispatch,NULL,reader_dispatch,r)!=0){r->running=0;klj_reader_release(r);klj_reader_release(r);return AMEDIA_ERROR_UNKNOWN;}
    r->thread_started=1;*out=r;
`+source.slice(createEnd);
  source=replace(source,'    if (listener) {\n        // Copied out by value.',`    r->have_listener=0;
    if(!pthread_equal(pthread_self(),r->dispatch))while(r->in_callback)pthread_cond_wait(&r->cv,&r->lock);
    if (listener) {
        // Copied out by value.`);
  const deletion=source.indexOf('static void klm_AImageReader_delete('),publish=source.indexOf('// Called by the codec when a frame is released',deletion);
  if(deletion<0||publish<deletion)throw Error('Pinned native reader deletion changed');
  source=source.slice(0,deletion)+fs.readFileSync(path.join(import.meta.dirname,'media-reader-retain.c'),'utf8')+'\n'+source.slice(publish);
  source=replace(source,'static void klm_reader_publish(klm_reader *r, CVPixelBufferRef pb, int64_t pts_us) {\n    if (!r || r->magic != KLM_READER_MAGIC) return;\n    pthread_mutex_lock(&r->lock);','static int klm_reader_publish(klm_reader *r, CVPixelBufferRef pb, int64_t pts_us) {\n    if (!r || r->magic != KLM_READER_MAGIC) return AMEDIA_ERROR_INVALID_OBJECT;\n    pthread_mutex_lock(&r->lock);\n    if(!r->running){pthread_mutex_unlock(&r->lock);return AMEDIA_ERROR_INVALID_OBJECT;}');
  source=replace(source,'    pthread_cond_signal(&r->cv);\n    pthread_mutex_unlock(&r->lock);\n}', '    pthread_cond_signal(&r->cv);\n    pthread_mutex_unlock(&r->lock);\n    return AMEDIA_OK;\n}');
  const insert=source.indexOf('// ATrace.');source=source.slice(0,insert)+fs.readFileSync(path.join(import.meta.dirname,'media-reader-surface.c'),'utf8')+'\n'+source.slice(insert);
  return replace(source,'void *kl_mediandk_lookup(const char *name) {','void *kl_mediandk_lookup(const char *name) {\n    if(name&&(!strncmp(name,"AMediaCodec",11)||!strncmp(name,"AMEDIACODEC_",12)))return klj_media_format_lookup(name);');
}
