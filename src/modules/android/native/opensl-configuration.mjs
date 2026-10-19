import fs from 'node:fs';
import path from 'node:path';

const replace=(source,before,after)=>{
 if(source.split(before).length!==2)throw Error('Original OpenSL configuration boundary changed: '+before.slice(0,80));
 return source.replace(before,after);
};
const section=(source,first,last)=>{
 const start=source.indexOf(first),end=source.indexOf(last,start);
 if(start<0||end<0)throw Error('Original Android OpenSL configuration source changed');
 return source.slice(start,end);
};

/** Keep Wilhelm's complete scalar configuration algorithms in the existing
 * native stream owner. Audio-server enum names become private value tokens;
 * they never cross the guest ABI or select a second physical audio owner. */
export function prepareOpenSlConfiguration(source,build){
 const player=fs.readFileSync(path.join(source,'src/android/AudioPlayer_to_android.cpp'),'utf8');
 const recorder=fs.readFileSync(path.join(source,'src/android/AudioRecorder_to_android.cpp'),'utf8');
 const definitions=fs.readFileSync(path.join(source,'src/android/android_defs.h'),'utf8');
 const modes=section(definitions,'#define ANDROID_PERFORMANCE_MODE_NONE','#define ANDROID_PERFORMANCE_MODE_ALL');
 const aliases=`
typedef uint8_t SLchar;
typedef int32_t SLint32;
#include "opensl-ids/source/include/SLES/OpenSLES_AndroidConfiguration.h"
#define SL_RESULT_PARAMETER_INVALID 2
#define SL_RESULT_PRECONDITIONS_VIOLATED 1
#define SL_RESULT_BUFFER_INSUFFICIENT 7
#define SL_RESULT_INTERNAL_ERROR 13
#define SL_OBJECT_STATE_UNREALIZED 1
typedef SLint32 audio_stream_type_t;
typedef SLint32 audio_source_t;
enum { AUDIO_STREAM_DEFAULT=-1,
 AUDIO_STREAM_VOICE_CALL=SL_ANDROID_STREAM_VOICE, AUDIO_STREAM_SYSTEM=SL_ANDROID_STREAM_SYSTEM,
 AUDIO_STREAM_RING=SL_ANDROID_STREAM_RING, AUDIO_STREAM_MUSIC=SL_ANDROID_STREAM_MEDIA,
 AUDIO_STREAM_ALARM=SL_ANDROID_STREAM_ALARM, AUDIO_STREAM_NOTIFICATION=SL_ANDROID_STREAM_NOTIFICATION };
enum { AUDIO_SOURCE_DEFAULT, AUDIO_SOURCE_MIC, AUDIO_SOURCE_VOICE_UPLINK,
 AUDIO_SOURCE_VOICE_DOWNLINK, AUDIO_SOURCE_VOICE_CALL, AUDIO_SOURCE_CAMCORDER,
 AUDIO_SOURCE_VOICE_RECOGNITION, AUDIO_SOURCE_VOICE_COMMUNICATION, AUDIO_SOURCE_UNPROCESSED };
#define ANDROID_DEFAULT_OUTPUT_STREAM_TYPE AUDIO_STREAM_MUSIC
${modes}
`;
 fs.writeFileSync(path.join(build,'opensl-configuration-values.h'),aliases);
 const bodies=[
  section(player,'SLresult audioPlayer_setStreamType(','//-----------------------------------------------------------------------------\nvoid audioPlayer_auxEffectUpdate'),
  section(recorder,'SLresult audioRecorder_setPreset(','void audioRecorder_handleNewPos_lockRecord'),
  section(player,'SLresult android_audioPlayer_setConfig(','// Called from android_audioPlayer_realize'),
  section(recorder,'SLresult android_audioRecorder_setConfig(','// Called from android_audioRecorder_realize'),
 ].join('\n');
 const prelude=`#include <assert.h>
typedef kl_sl_player CAudioPlayer;
typedef kl_sl_recorder CAudioRecorder;
#define KEY_STREAM_TYPE_PARAMSIZE sizeof(SLint32)
#define KEY_RECORDING_PRESET_PARAMSIZE sizeof(SLuint32)
#define KEY_PERFORMANCE_MODE_PARAMSIZE sizeof(SLuint32)
#define SL_LOGV(...) ((void)0)
#define SL_LOGE(...) ((void)0)
`;
 fs.writeFileSync(path.join(build,'opensl-configuration-original.inc'),prelude+bodies.replace(/^SLresult /gm,'static SLresult ')+'\n#undef SL_LOGV\n#undef SL_LOGE\n');
}

export function openSlConfigurationSource(source){
 source=replace(source,'typedef uint32_t SLboolean;','typedef uint32_t SLboolean;\n#include "opensl-configuration-values.h"');
 const configuration='    const struct SLConfigItf_ *cfg_vt;\n\n    pthread_mutex_t lock;';
 if(source.split(configuration).length!==3)throw Error('Original OpenSL player/recorder configuration layout changed');
 source=source.replace(configuration,
  '    const struct SLConfigItf_ *cfg_vt;\n    struct { SLuint32 mState; } mObject;\n    SLint32 mStreamType; SLuint32 mPerformanceMode;\n\n    pthread_mutex_t lock;');
 source=replace(source,'    const struct SLConfigItf_ *cfg_vt;\n\n    pthread_mutex_t lock;',
  '    const struct SLConfigItf_ *cfg_vt;\n    struct { SLuint32 mState; } mObject;\n    audio_source_t mRecordSource; SLuint32 mPerformanceMode;\n\n    pthread_mutex_t lock;');
 const first=source.indexOf('static SLresult cfg_Set('),last=source.indexOf('static const struct SLConfigItf_ g_cfg_vt',first);
 if(first<0||last<0)throw Error('Original OpenSL configuration dispatch changed');
 source=source.slice(0,first)+`#include "opensl-configuration-original.inc"
static SLresult cfg_Set(SLConfigItf self,const char *key,const void *value,SLuint32 size) {
 if(!key||!value||!size)return SL_RESULT_PARAMETER_INVALID;
 for(int i=0;i<KL_SL_MAX_PLAYERS;i++)if(self==&g_players[i].cfg_vt){
  kl_sl_player *p=&g_players[i];pthread_mutex_lock(&p->lock);
  SLresult result=android_audioPlayer_setConfig(p,(const SLchar*)key,value,size);
  pthread_mutex_unlock(&p->lock);return result;
 }
 if(self==&g_recorder.cfg_vt){pthread_mutex_lock(&g_recorder.lock);
  SLresult result=android_audioRecorder_setConfig(&g_recorder,(const SLchar*)key,value,size);
  pthread_mutex_unlock(&g_recorder.lock);return result;
 }
 return SL_RESULT_PARAMETER_INVALID;
}
static SLresult cfg_Get(SLConfigItf self,const char *key,SLuint32 *size,void *value) {
 if(!key||!size)return SL_RESULT_PARAMETER_INVALID;
 for(int i=0;i<KL_SL_MAX_PLAYERS;i++)if(self==&g_players[i].cfg_vt){
  kl_sl_player *p=&g_players[i];pthread_mutex_lock(&p->lock);
  SLresult result=android_audioPlayer_getConfig(p,(const SLchar*)key,size,value);
  pthread_mutex_unlock(&p->lock);return result;
 }
 if(self==&g_recorder.cfg_vt){pthread_mutex_lock(&g_recorder.lock);
  SLresult result=android_audioRecorder_getConfig(&g_recorder,(const SLchar*)key,size,value);
  pthread_mutex_unlock(&g_recorder.lock);return result;
 }
 return SL_RESULT_PARAMETER_INVALID;
}
`+source.slice(last);
 source=replace(source,'    p->cfg_vt = &g_cfg_vt;',`    p->cfg_vt = &g_cfg_vt;
    p->mObject.mState=SL_OBJECT_STATE_UNREALIZED;
    p->mStreamType=ANDROID_DEFAULT_OUTPUT_STREAM_TYPE;
    p->mPerformanceMode=ANDROID_PERFORMANCE_MODE_NONE;`);
 source=replace(source,'    r->cfg_vt = &g_cfg_vt;',`    r->cfg_vt = &g_cfg_vt;
    r->mObject.mState=SL_OBJECT_STATE_UNREALIZED;
    r->mRecordSource=AUDIO_SOURCE_DEFAULT;
    r->mPerformanceMode=ANDROID_PERFORMANCE_MODE_NONE;`);
 // Report the actual shared buffered output path, rather than claiming a
 // requested Android low-latency/power profile was granted by CoreAudio.
 source=replace(source,'        p->started = p->running = 1;',`        pthread_mutex_lock(&p->lock);
        p->mObject.mState=SL_OBJECT_STATE_REALIZED;
        p->mPerformanceMode=ANDROID_PERFORMANCE_MODE_NONE;
        pthread_mutex_unlock(&p->lock);
        p->started = p->running = 1;`);
 source=replace(source,'        r->started = r->running = 1;',`        pthread_mutex_lock(&r->lock);
        r->mObject.mState=SL_OBJECT_STATE_REALIZED;
        r->mPerformanceMode=ANDROID_PERFORMANCE_MODE_NONE;
        pthread_mutex_unlock(&r->lock);
        r->started = r->running = 1;`);
 return source;
}
