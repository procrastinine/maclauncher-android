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
 writeChanged(path.join(build,'capture-state.h'),'struct KljAudioCapture {int32_t rate,channels,format;\n'+original.slice(start,end)+'unsigned long frames;\n};\n');
 writeChanged(path.join(build,'capture-original.inc'),original.slice(first,last).replaceAll('klaa_stream','KljAudioCapture').replaceAll('klaa_lowpass','klj_capture_original_lowpass').replaceAll('klaa_capture_into','klj_capture_original_capture_into'));
}

export function adaptAudioCapture(source){
 const replace=(before,after)=>{if(source.split(before).length!==2)throw Error('Original capture boundary changed: '+before.slice(0,70));source=source.replace(before,after);};
 const start=source.indexOf('    // Capture path (direction == INPUT only).'),end=source.indexOf('    unsigned long callbacks, frames;',start);
 const first=source.indexOf('static void klaa_lowpass('),last=source.indexOf('// The capture feeder',first);
 if(start<0||end<0||first<0||last<0)throw Error('Original native capture source changed');
 source=source.slice(0,first)+'static int32_t klaa_capture_into(klaa_stream *s,void *dst,int32_t frames,int64_t timeout){int32_t count=klj_audio_capture_read(s->capture,dst,frames,timeout);s->frames+=(unsigned long)count;return count;}\n\n'+source.slice(last);
 source=source.slice(0,start)+'    KljAudioCapture *capture;\n\n'+source.slice(end);
 replace('if (kl_audio_mic_open((unsigned)rate, (unsigned)channels) != 0) {','KljAudioCapture *capture=klj_audio_capture_create((unsigned)rate,(unsigned)channels,format);\n        if (!capture) {');
 replace('if (g_input_streams == 0) kl_audio_mic_close();   // we just opened it','klj_audio_capture_destroy(capture);');
 replace('if (g_input_streams == 0) kl_audio_mic_close();','klj_audio_capture_destroy(capture);');
 replace('        is->mic_rate = kl_audio_mic_rate();\n        is->mic_ch   = kl_audio_mic_channels();','        is->capture = capture;');
 replace('if (--g_input_streams <= 0) { g_input_streams = 0; kl_audio_mic_close(); }\n        free(s->in_hold);','if (--g_input_streams <= 0) g_input_streams = 0;\n        klj_audio_capture_destroy(s->capture);');
 source=source.replaceAll('is->mic_rate','klj_audio_capture_rate(is->capture)').replaceAll('is->mic_ch','klj_audio_capture_channels(is->capture)');
 return includeDependencies(source,'#include '+JSON.stringify(path.join(import.meta.dirname,'audio-capture.h')));
}
