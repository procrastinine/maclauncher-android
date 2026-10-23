import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from './upstream/source-files.mjs';
import {includeDependencies} from './upstream/source-includes.mjs';

/** Move the original capture conversion into the physical audio owner. AAudio
 * retains its original builder, callback, stream state and guest ABI. */
export function prepareAudioCaptureSources(klepton,build){
 const original=fs.readFileSync(path.join(klepton,'runtime/media/kl_aaudio.c'),'utf8');
 const start=original.indexOf('    // Capture path (direction == INPUT only).'),end=original.indexOf('    unsigned long callbacks, frames;',start);
 const first=original.indexOf('static void klaa_lowpass('),last=original.indexOf('// The capture feeder',first);
 if(start<0||end<0||first<0||last<0)throw Error('Original native capture source changed');
 writeChanged(path.join(build,'capture-state.h'),'struct KljAudioCapture {int32_t rate,channels,format;\n'+original.slice(start,end)+'unsigned long frames;uint64_t cursor;unsigned overruns;int disconnected;\n};\n');
 let conversion=original.slice(first,last).replaceAll('klaa_stream','KljAudioCapture').replaceAll('klaa_lowpass','klj_capture_original_lowpass').replaceAll('klaa_capture_into','klj_capture_original_capture_into');
 if(conversion.split('kl_audio_mic_read_i16(s->in_hold').length!==2||conversion.split('            if (got > 0) {').length!==2)throw Error('Original capture read boundary changed');
 conversion=conversion.replace('kl_audio_mic_read_i16(s->in_hold','klj_capture_read_pcm(s,s->in_hold');
 conversion=conversion.replace('            if (got > 0) {','            if(got<0)return produced?produced:got;\n            if (got > 0) {');
 writeChanged(path.join(build,'capture-original.inc'),conversion);
}

export function adaptAudioCapture(source){
 const replace=(before,after)=>{if(source.split(before).length!==2)throw Error('Original capture boundary changed: '+before.slice(0,70));source=source.replace(before,after);};
 const start=source.indexOf('    // Capture path (direction == INPUT only).'),end=source.indexOf('    unsigned long callbacks, frames;',start);
 const first=source.indexOf('static void klaa_lowpass('),last=source.indexOf('// The capture feeder',first);
 if(start<0||end<0||first<0||last<0)throw Error('Original native capture source changed');
 source=source.slice(0,first)+'static int32_t klaa_capture_into(klaa_stream *s,void *dst,int32_t frames,int64_t timeout){int32_t count=klj_audio_capture_read(s->capture,dst,frames,timeout);if(count>0)s->frames+=(unsigned long)count;return count;}\n\n'+source.slice(last);
 source=source.slice(0,start)+'    KljAudioCapture *capture;\n\n'+source.slice(end);
 replace('if (kl_audio_mic_open((unsigned)rate, (unsigned)channels) != 0) {','KljAudioCapture *capture=klj_audio_capture_create((unsigned)rate,(unsigned)channels,format);\n        if (!capture) {');
 replace('if (g_input_streams == 0) kl_audio_mic_close();   // we just opened it','klj_audio_capture_destroy(capture);');
 replace('if (g_input_streams == 0) kl_audio_mic_close();','klj_audio_capture_destroy(capture);');
 replace('        is->mic_rate = kl_audio_mic_rate();\n        is->mic_ch   = kl_audio_mic_channels();','        is->capture = capture;');
 replace('if (--g_input_streams <= 0) { g_input_streams = 0; kl_audio_mic_close(); }\n        free(s->in_hold);','if (--g_input_streams <= 0) g_input_streams = 0;\n        klj_audio_capture_destroy(s->capture);');
 replace('        int32_t got = klaa_capture_into(s, s->scratch, frames, budget);',`        int32_t got = klaa_capture_into(s, s->scratch, frames, budget);
        if(got<0){
            pthread_mutex_lock(&s->lock);s->running=0;s->state=AAUDIO_STREAM_STATE_DISCONNECTED;pthread_mutex_unlock(&s->lock);
            if(s->error_cb)s->error_cb(s,s->error_user,got);
            break;
        }`);
 source=source.replaceAll('is->mic_rate','klj_audio_capture_rate(is->capture)').replaceAll('is->mic_ch','klj_audio_capture_channels(is->capture)');
 return includeDependencies(source,'#include '+JSON.stringify(path.join(import.meta.dirname,'audio-capture.h')));
}

/** The original OpenSL recorder retains its buffer queue and callback thread,
 * while borrowing the same physical capture owner and original converter. */
export function adaptOpenSlCapture(source){
 const replace=(before,after)=>{if(source.split(before).length!==2)throw Error('Original OpenSL capture boundary changed: '+before.slice(0,70));source=source.replace(before,after);};
 const fields=source.indexOf('    int16_t *in_hold;                        // mic frames pulled, not yet consumed');
 const end=source.indexOf('    unsigned long filled;',fields);
 if(fields<0||end<fields)throw Error('Original OpenSL recorder storage changed');
 source=source.slice(0,fields)+'    KljAudioCapture *capture;\n'+source.slice(end);
 const first=source.indexOf('static int rec_pull('),last=source.indexOf('// The record feeder',first);
 if(first<0||last<first)throw Error('Original OpenSL capture conversion changed');
 source=source.slice(0,first)+'static int rec_pull(kl_sl_recorder *r,int16_t *dst,int frames){return klj_audio_capture_read(r->capture,dst,frames,0);}\n\n'+source.slice(last);
 replace('    if (!r->started) return;\n    pthread_mutex_lock(&r->lock);',`    if (!r->started) {
        if(r->capture){klj_audio_capture_destroy(r->capture);r->capture=NULL;pthread_mutex_destroy(&r->lock);pthread_cond_destroy(&r->wake);pthread_cond_destroy(&r->cb_done);}
        return;
    }
    pthread_mutex_lock(&r->lock);`);
 replace('    kl_audio_mic_close();\n    free(r->in_hold); r->in_hold = NULL; r->in_hold_cap = 0; r->in_have = 0;','    klj_audio_capture_destroy(r->capture);r->capture=NULL;');
 replace('    if (kl_audio_mic_open(r->rate, r->channels) != 0) {','    r->capture=klj_audio_capture_create(r->rate,r->channels,1);\n    if (!r->capture) {');
 replace('    r->mic_rate = kl_audio_mic_rate();\n    r->mic_ch   = kl_audio_mic_channels();','    r->mic_rate = klj_audio_capture_rate(r->capture);\n    r->mic_ch   = klj_audio_capture_channels(r->capture);');
 replace('            done += got;',`            if(got<0){pthread_mutex_lock(&r->lock);r->running=0;r->state=SL_RECORDSTATE_STOPPED;pthread_mutex_unlock(&r->lock);break;}
            done += got;`);
 return includeDependencies(source,'#include '+JSON.stringify(path.join(import.meta.dirname,'audio-capture.h')));
}
