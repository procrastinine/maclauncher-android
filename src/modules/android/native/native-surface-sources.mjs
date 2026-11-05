import fs from 'node:fs';import path from 'node:path';
import {prepareMutexOwnership} from './pthread-mutex-sources.mjs';
import {prepareAudioCaptureSources,adaptAudioCapture} from './audio-capture-sources.mjs';
import {adaptAAudioOwnership} from './aaudio-sources.mjs';
import {writeChanged} from './upstream/source-files.mjs';
/** Observe the existing native state transitions; retain their locks and algorithms. */
export function audioActivity(source, receiver, active, output='1'){
 const pattern=new RegExp('\\b('+receiver+')->state = ([A-Za-z_][A-Za-z_0-9]*);','g');
 let count=0;const result=source.replace(pattern,(_,owner,value)=>{count++;return `do { int was_playing=(${output.replaceAll('$',owner)}) && ${owner}->state == ${active}; ${owner}->state = ${value}; klj_audio_activity_transition(3,was_playing,(${output.replaceAll('$',owner)}) && ${owner}->state == ${active}); } while(0);`;});
 if(!count)throw Error('Native audio state owner changed');
 return '#include '+JSON.stringify(path.join(import.meta.dirname,'audio-volume.h'))+'\n'+result;
}
/** Apply the owned stream/thread lifecycle changes to the pinned source. */
export function prepareNativeSurfaceSources(klepton,build){
 prepareAudioCaptureSources(klepton,build);
 const replace=(text,from,to)=>{if(text.split(from).length!==2)throw Error('Pinned native lifecycle source changed: '+from);return text.replace(from,to);};
 let audio=fs.readFileSync(path.join(klepton,'runtime/media/kl_audio.c'),'utf8');
 audio=replace(audio,'AURenderCallbackStruct cb = {render_cb, NULL};','AURenderCallbackStruct cb = {owned_render, NULL};');
 audio=replace(audio,'size_t kl_audio_write_src(const void *src, const void *pcm, size_t bytes) {','size_t kl_audio_write_src(const void *src, const void *pcm, size_t bytes) {\n    owned_note_source(src);');
 audio=replace(audio,'            if (nf > curw) atomic_store_explicit(&g_w, nf, memory_order_release);','            if (nf > curw) atomic_store_explicit(&g_w, nf, memory_order_release);\n            owned_audio_commit(here + take,written,frames);');
 const reset='    atomic_store(&g_w, 0); atomic_store(&g_r, 0);';
 if(audio.split(reset).length!==3)throw Error('Original audio device cursor resets changed');
 audio=audio.replaceAll(reset,'    owned_audio_reset_progress();\n'+reset);
 audio=replace(audio,'    if (g_wd_streak < 4 || (g_wd_streak % 20) == 0)',
   '    if (!owned_recovery_begin(limit)) return;\n    if (g_wd_streak < 4 || (g_wd_streak % 20) == 0)');
 audio=replace(audio,'    g_wd_streak++;\n    kl_audio_restart();',
   '    g_wd_streak++;\n    kl_audio_restart();\n    owned_recovery_end();');
 audio=replace(audio,'    pthread_setname_np("klepton-audio-watchdog");',
   '    pthread_setname_np("klepton-audio-watchdog");\n    atomic_store_explicit(&owned_watchdog_running,1,memory_order_release);');
 audio=replace(audio,'        watchdog();\n        if (now_ns() > deadline)',
   '        if (atomic_load_explicit(&owned_recovery_pending,memory_order_acquire)) break;\n        if (!atomic_load_explicit(&owned_watchdog_running,memory_order_acquire)) {\n            owned_recovery_writer=1; watchdog(); owned_recovery_writer=0;\n        }\n        if (now_ns() > deadline)');
 audio=replace(audio,'static int              g_mic_enabled;','static _Atomic int      g_mic_enabled;');
 audio=replace(audio,'AudioDeviceID dev = 0; UInt32 dsz = sizeof dev;\n    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &dsz, &dev) == noErr && dev)',
   'AudioDeviceID dev = atomic_load_explicit(&owned_capture_device,memory_order_acquire);\n    if (!dev) goto fail;\n    if (dev)');
 audio=replace(audio,'pthread_mutex_lock(&g_mic_lock);\n    // Idempotent:',
   'pthread_mutex_lock(&g_mic_lock);\n    if (!g_mic_enabled) { pthread_mutex_unlock(&g_mic_lock); return -1; }\n    // Idempotent:');
 audio=replace(audio,'if (g_mic_open) { pthread_mutex_unlock(&g_mic_lock); return 0; }',
   'if (g_mic_open) { pthread_mutex_unlock(&g_mic_lock); return -1; }');
 let pthread=fs.readFileSync(path.join(klepton,'runtime/libc/kl_pthread.c'),'utf8');
 // Klepton PR #19: keep enough simultaneously live Bionic semaphores.
 pthread=replace(pthread,'#define KL_MAX_SEMS 1024','#define KL_MAX_SEMS 65536');
 pthread=prepareMutexOwnership(pthread);
 pthread=replace(pthread,'static void tsd_thread_exit(void *p) {\n    kl_tsd_slot *s = p;',
   'static void tsd_thread_exit(void *p) {\n    // A later Darwin TSD pass can clear the borrowed guest TP key. Restore\n    // the same retained native context before invoking original destructors.\n    kl_thread_init();\n    kl_tsd_slot *s = p;');
 pthread=replace(pthread,'        if (ga->stack_size) pthread_attr_setstacksize(&da, ga->stack_size);',
   '        if (ga->stack_size) {\n            int error=klj_pthread_set_guest_stacksize(&da,ga->stack_size);\n            if(error){pthread_attr_destroy(&da);return px(error);}\n        }');
 pthread=replace(pthread,'static void tsd_init(void)','void klj_finish_guest_thread(void) {\n    int retirement=klj_begin_guest_thread_retirement();\n    if(!retirement)return;\n    klj_guest_cxa_thread_finalize();\n    if(atomic_load(&g_nkeys_hw)>0) {\n        kl_tsd_slot *slots=pthread_getspecific(g_tsd_key);\n        if(slots)tsd_thread_exit(slots);\n    }\n    if(retirement==2)klj_detach_guest_thread();\n}\nstatic void tsd_init(void)');
 pthread=replace(pthread,'static void *thread_tramp(void *p) {','static void klj_guest_thread_cleanup(void *unused) {\n    (void)unused;\n    klj_finish_guest_thread();\n    thread_unregister(pthread_self());\n}\nstatic void *thread_tramp(void *p) {');
 pthread=replace(pthread,'    void *r = t.fn(t.arg);\n    thread_unregister(pthread_self());\n    return r;','    void *r;\n    pthread_cleanup_push(klj_guest_thread_cleanup,NULL);\n    r = t.fn(t.arg);\n    pthread_cleanup_pop(1);\n    return r;');
 pthread=replace(pthread,'{ thread_unregister(pthread_self()); pthread_exit(r); }','{ if(klj_jni_request_thread_exit())return; klj_finish_guest_thread(); thread_unregister(pthread_self()); pthread_exit(r); }');
 let aaudio=adaptAAudioOwnership(adaptAudioCapture(fs.readFileSync(path.join(klepton,'runtime/media/kl_aaudio.c'),'utf8')));
 if(aaudio.split('kl_thread_init();').length!==3||aaudio.split('    return NULL;\n}').length!==3)throw Error('Pinned AAudio feeder source changed');
 aaudio=aaudio.replaceAll('kl_thread_init();','owned_feeding=s; kl_thread_init();');
 aaudio=audioActivity(aaudio,'s|is','AAUDIO_STREAM_STATE_STARTED','$->direction == AAUDIO_DIRECTION_OUTPUT');
 const files={'audio-source.c':audio,'pthread-source.c':pthread,'aaudio-source.c':aaudio};for(const [name,text]of Object.entries(files))writeChanged(path.join(build,name),text);
 return ['-DKLEPTON_AAUDIO_SOURCE='+JSON.stringify(path.join(build,'aaudio-source.c')),'-DKLEPTON_AUDIO_PAUSE_SOURCE='+JSON.stringify(path.join(build,'audio-source.c')),'-DKLEPTON_PTHREAD_LIFETIME_SOURCE='+JSON.stringify(path.join(build,'pthread-source.c')),'-DKLEPTON_AUDIO_CAPTURE_STATE='+JSON.stringify(path.join(build,'capture-state.h')),'-DKLEPTON_AUDIO_CAPTURE_SOURCE='+JSON.stringify(path.join(build,'capture-original.inc'))];
}
