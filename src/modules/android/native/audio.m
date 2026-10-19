// Android audio decoding through the included codecs and shared PCM output.
#import <Foundation/Foundation.h>
#include <jni.h>
// Explicit ownership prevents a JVM load from invoking a dependency initializer.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm,void *reserved){(void)vm;(void)reserved;return JNI_VERSION_1_6;}
#include <sndfile.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <errno.h>
#include "mpeg-player.h"
#include "audio-volume.h"
#include "audio-pcm.h"

JNIEXPORT jboolean JNICALL Java_maclauncher_android_media_HostAudioManager_nativeStreamActive(JNIEnv *env,jclass type,jint stream,jint past){
    (void)env;(void)type;return klj_audio_stream_active(stream,past)?JNI_TRUE:JNI_FALSE;
}

typedef struct { const unsigned char *bytes; sf_count_t size, position; } MemoryAudio;
static sf_count_t length(void *p) { return ((MemoryAudio *)p)->size; }
static sf_count_t tell(void *p) { return ((MemoryAudio *)p)->position; }
static sf_count_t seek_audio(sf_count_t off, int whence, void *p) {
    MemoryAudio *m=p;
    sf_count_t base=whence==SEEK_SET?0:whence==SEEK_CUR?m->position:whence==SEEK_END?m->size:-1;
    if(base<0 || off < -base || off > m->size-base) return -1;
    return m->position=base+off;
}
static sf_count_t read_audio(void *dest, sf_count_t count, void *p) {
    MemoryAudio *m=p;
    if(count<0)return 0;
    if(count>m->size-m->position)count=m->size-m->position;
    memcpy(dest,m->bytes+m->position,(size_t)count);m->position+=count;return count;
}
static sf_count_t write_audio(const void *src, sf_count_t count, void *p) {
    (void)src;(void)count;(void)p;return 0;
}
static void fail(JNIEnv *env,const char *type,const char *message) {
    if(!(*env)->ExceptionCheck(env))(*env)->ThrowNew(env,(*env)->FindClass(env,type),message);
}
static NSData *read_source(JNIEnv *env,jstring path,jobject descriptor,jlong start,jlong bytes) {
    int fd=-1;BOOL owns=NO;
    if(path){const char *s=(*env)->GetStringUTFChars(env,path,NULL);if(!s)return nil;
        fd=open(s,O_RDONLY);(*env)->ReleaseStringUTFChars(env,path,s);owns=YES;
    }else if(descriptor){jfieldID field=(*env)->GetFieldID(env,(*env)->GetObjectClass(env,descriptor),"fd","I");
        if(!field)return nil;fd=(*env)->GetIntField(env,descriptor,field);
    }
    struct stat st;
    if(fd<0||fstat(fd,&st)!=0||!S_ISREG(st.st_mode)){
        if(owns&&fd>=0)close(fd);fail(env,"java/io/IOException","Audio source is not a readable regular file");return nil;
    }
    if(bytes==-1 && start>=0 && start<=st.st_size)bytes=st.st_size-start;
    if(start<0||bytes<1||bytes>64*1024*1024||start>st.st_size||bytes>st.st_size-start){
        if(owns)close(fd);fail(env,"java/io/IOException","Audio source range is invalid or exceeds 64 MiB");return nil;
    }
    NSMutableData *data=[NSMutableData dataWithLength:(NSUInteger)bytes];
    size_t done=0;
    while(done<(size_t)bytes){ssize_t n=pread(fd,(char *)data.mutableBytes+done,(size_t)bytes-done,start+(off_t)done);
        if(n<=0){if(owns)close(fd);fail(env,"java/io/IOException","Cannot read complete audio source");return nil;}done+=(size_t)n;
    }
    if(owns)close(fd);return data;
}
static void u16(unsigned char *p,uint16_t v){p[0]=v;p[1]=v>>8;}
static void u32(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static NSData *decode(JNIEnv *env,NSData *source,SF_INFO *info,double *rms) {
    MemoryAudio mem={source.bytes,(sf_count_t)source.length,0};
    SF_VIRTUAL_IO io={length,seek_audio,read_audio,write_audio,tell};
    SNDFILE *file=sf_open_virtual(&io,SFM_READ,info,&mem);
    if(!file){fail(env,"java/io/IOException",sf_strerror(NULL));return nil;}
    if(info->channels<1||info->channels>2||info->samplerate<8000||info->samplerate>192000||
        info->frames<1||info->frames>256*1024*1024/(2*info->channels)){
        sf_close(file);fail(env,"java/io/IOException","Unsupported audio format or decoded audio exceeds 256 MiB");return nil;
    }
    // MPEG frame counts can be estimates. Decode to actual EOF rather than
    // treating the estimate as a promise or padding the missing frames.
    NSMutableData *wav=[NSMutableData dataWithLength:44];
    short block[4096*2];sf_count_t frames,total=0;
    while((frames=sf_readf_short(file,block,4096))>0){
        if(total+frames>256*1024*1024/(2*info->channels)){
            sf_close(file);fail(env,"java/io/IOException","Decoded audio exceeds 256 MiB");return nil;
        }
        [wav appendBytes:block length:(NSUInteger)(frames*info->channels*2)];total+=frames;
    }
    int error=sf_error(file);sf_close(file);
    if(total<1||error){fail(env,"java/io/IOException","Audio decode failed");return nil;}
    info->frames=total;uint32_t size=(uint32_t)(total*info->channels*2);
    unsigned char *p=wav.mutableBytes;
    memcpy(p,"RIFF",4);u32(p+4,size+36);memcpy(p+8,"WAVEfmt ",8);u32(p+16,16);
    u16(p+20,1);u16(p+22,info->channels);u32(p+24,info->samplerate);
    u32(p+28,info->samplerate*info->channels*2);u16(p+32,info->channels*2);u16(p+34,16);
    memcpy(p+36,"data",4);u32(p+40,size);
    if(rms){double sum=0;short *samples=(short *)(p+44);for(uint32_t i=0;i<size/2;i++){double f=samples[i]/32768.0;sum+=f*f;}*rms=sqrt(sum/(size/2));}
    return wav;
}
@interface KLJAudioPlayer : NSObject
@property(nonatomic,strong) NSData *data;
@property(nonatomic,assign) void *pcm;
@property(nonatomic,assign) uint64_t frames;
@property(nonatomic,assign) unsigned channels,sampleRate;
@property(nonatomic,assign) float rate;
@property(nonatomic,assign) kl_mediaplayer *mpeg;
@property(nonatomic,assign) int stream;
@property(nonatomic,assign) float left,right;
@property(nonatomic,assign) BOOL released;
@end
@implementation KLJAudioPlayer
- (instancetype)init {if((self=[super init])){_stream=3;_left=_right=_rate=1;}return self;}
- (void)dealloc { if(_pcm)klj_audio_pcm_destroy(_pcm);if(_mpeg)kl_mediaplayer_release(_mpeg); }
@end
static NSMutableDictionary<NSNumber *,KLJAudioPlayer *> *players;
static NSObject *lock;
static jlong nextID=1;
static void init_registry(void){static dispatch_once_t once;dispatch_once(&once,^{players=[NSMutableDictionary new];lock=[NSObject new];});}

// The registry only owns membership and identities. Retain the owner before
// leaving it; all native player access and retirement use that owner's monitor.
// Never wait for an owner while holding the registry monitor.
static KLJAudioPlayer *find_player(jlong id){
    init_registry();KLJAudioPlayer *owned;
    @synchronized(lock){owned=players[@(id)];}
    return owned;
}
static BOOL live_player(JNIEnv *env,KLJAudioPlayer *owned){
    if(owned&&!owned.released)return YES;
    fail(env,"java/lang/IllegalStateException","Audio player was released");return NO;
}
static void apply_gain(KLJAudioPlayer *owned){
    float gain=klj_audio_stream_gain(owned.stream),left=owned.left*gain,right=owned.right*gain;
    if(owned.mpeg){klj_mpeg_volume(owned.mpeg,left,right);return;}
    klj_audio_pcm_controls(owned.pcm,owned.sampleRate*(double)owned.rate,left,right,0);
}
JNIEXPORT void JNICALL Java_maclauncher_android_media_HostAudioManager_nativeStreamVolume(JNIEnv *env,jclass cls,jint stream,jint index){
    (void)cls;@autoreleasepool{
        if(!klj_audio_set_stream_volume(stream,index)){fail(env,"java/lang/IllegalArgumentException","Invalid audio stream volume");return;}
        init_registry();NSArray<KLJAudioPlayer *> *snapshot;
        @synchronized(lock){snapshot=players.allValues;}
        for(KLJAudioPlayer *owned in snapshot)@synchronized(owned){
            if(!owned.released&&owned.stream==stream){apply_gain(owned);if(owned.mpeg&&klj_mpeg_error(owned.mpeg))fail(env,"java/lang/IllegalStateException","MPEG audio volume update failed");}
        }
    }
}
JNIEXPORT void JNICALL Java_maclauncher_android_media_AudioHost_setStream(JNIEnv *env,jclass cls,jlong id,jint stream){
    (void)cls;@autoreleasepool{KLJAudioPlayer *owned=find_player(id);@synchronized(owned){
        if(!live_player(env,owned))return;
        if(stream<0||stream>=KLJ_AUDIO_STREAMS){fail(env,"java/lang/IllegalArgumentException","Invalid audio stream type");return;}
        owned.stream=stream;apply_gain(owned);
    }}
}

// Content dispatch only: existing Ogg/WAV/MP3 decoding is unchanged. Klepton
// supplies MPEG program-stream audio and raw Layer I/II that it already owns.
static BOOL mpeg_source(NSData *source) {
    const unsigned char *p=source.bytes;size_t n=source.length,start=0;
    if(n>=10&&!memcmp(p,"ID3",3)){
        for(int i=6;i<10;i++)if(p[i]&0x80)return NO;
        start=10+((size_t)p[6]<<21)+((size_t)p[7]<<14)+((size_t)p[8]<<7)+p[9];
    }
    if(start>n||n-start<4)return NO;p+=start;
    if(!p[0]&&!p[1]&&p[2]==1&&(p[3]==0xba||p[3]==0xbb))return YES;
    int layer=(p[1]>>1)&3,version=(p[1]>>3)&3,bitrate=p[2]>>4;
    return p[0]==0xff&&(p[1]&0xe0)==0xe0&&version!=1&&layer>=2&&bitrate>0&&bitrate<15&&((p[2]>>2)&3)!=3;
}
static kl_mediaplayer *load_mpeg(JNIEnv *env,NSData *source) {
    // Borrow the validated file/range snapshot directly during indexing.
    // Klepton retains its own elementary stream; no temporary file or reread.
    kl_mediaplayer *player=kl_mediaplayer_new();
    if(player)klj_mpeg_prepare_bytes(player,source.bytes,source.length);
    if(!klj_mpeg_ready(player)){
        int error=klj_mpeg_error(player);if(player)kl_mediaplayer_release(player);
        char message[160];if(error)snprintf(message,sizeof message,"Cannot prepare MPEG audio (output status %d)",error);
        else snprintf(message,sizeof message,"MPEG source has no playable audio or exceeds available decoder memory");
        fail(env,"java/io/IOException",message);return NULL;
    }
    return player;
}
static jlong add_player(JNIEnv *env,KLJAudioPlayer *player) {
    init_registry();@synchronized(player){
        jlong id=0;
        @synchronized(lock){
            if(nextID>0&&nextID<INT64_MAX){id=nextID++;players[@(id)]=player;}
        }
        if(!id){fail(env,"java/lang/IllegalStateException","Audio player identities are exhausted");return 0;}
        // Publish before reading the current stream gain: concurrent volume
        // changes either precede this read or include this owner in their snapshot.
        apply_gain(player);return id;
    }
}
static jlong load_player(JNIEnv *env,jstring path,jobject fd,jlong offset,jlong size,BOOL media){
    @autoreleasepool{
        NSData *source=read_source(env,path,fd,offset,size);if(!source)return 0;
        KLJAudioPlayer *owned=[KLJAudioPlayer new];
        if(media&&mpeg_source(source)){
            owned.mpeg=load_mpeg(env,source);return owned.mpeg?add_player(env,owned):0;
        }
        SF_INFO info={0};NSData *wav=decode(env,source,&info,NULL);if(!wav)return 0;
        owned.data=wav;owned.frames=info.frames;owned.channels=info.channels;owned.sampleRate=info.samplerate;
        owned.pcm=klj_audio_pcm_clip((const int16_t *)((const char *)wav.bytes+44),owned.frames,owned.channels,owned.sampleRate);
        if(!owned.pcm){fail(env,"java/io/IOException","Audio output cannot be opened");return 0;}
        return add_player(env,owned);
    }
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_media_AudioHost_load(JNIEnv *env,jclass cls,jstring path,jobject fd,jlong offset,jlong size){
    (void)cls;return load_player(env,path,fd,offset,size,NO);
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_media_AudioHost_loadMedia(JNIEnv *env,jclass cls,jstring path,jobject fd,jlong offset,jlong size){
    (void)cls;return load_player(env,path,fd,offset,size,YES);
}
JNIEXPORT jboolean JNICALL Java_maclauncher_android_media_AudioHost_action(JNIEnv *env,jclass cls,jlong id,jint action){
    (void)cls;@autoreleasepool{KLJAudioPlayer *owned=find_player(id);@synchronized(owned){
        if(!live_player(env,owned))return JNI_FALSE;
        // A retained lookup may race retirement. Mark under the same owner
        // monitor before detaching, so it can never touch disposed native state.
        if(action==3){
            owned.released=YES;
            @synchronized(lock){[players removeObjectForKey:@(id)];}
            if(owned.mpeg){
                kl_mediaplayer *mp=owned.mpeg;owned.mpeg=NULL;
                kl_mediaplayer_release(mp);
            }else{klj_audio_pcm_destroy(owned.pcm);owned.pcm=NULL;owned.data=nil;}
            return JNI_TRUE;
        }
        if(owned.mpeg){
            kl_mediaplayer *mp=owned.mpeg;
            int error=klj_mpeg_error(mp);if(error){char message[128];snprintf(message,sizeof message,"MPEG audio output failed (status %d)",error);fail(env,"java/lang/IllegalStateException",message);return JNI_FALSE;}
            BOOL result=YES;
            switch(action){case 0:result=klj_mpeg_start(mp);break;case 1:kl_mediaplayer_pause(mp);break;
                case 2:kl_mediaplayer_stop(mp);break;case 4:result=kl_mediaplayer_is_playing(mp);break;
                case 5:result=klj_mpeg_ready(mp);break;default:fail(env,"java/lang/IllegalArgumentException","Unknown audio action");return JNI_FALSE;}
            error=klj_mpeg_error(mp);if(error){char message[128];snprintf(message,sizeof message,"MPEG audio output failed (status %d)",error);fail(env,"java/lang/IllegalStateException",message);return JNI_FALSE;}
            return result;
        }
        switch(action){case 0:klj_audio_pcm_state(owned.pcm,0);return JNI_TRUE;case 1:klj_audio_pcm_state(owned.pcm,1);return JNI_TRUE;case 2:klj_audio_pcm_state(owned.pcm,3);return JNI_TRUE;
        case 4:return klj_audio_pcm_playing(owned.pcm);
        case 5:return owned.pcm!=NULL;default:fail(env,"java/lang/IllegalArgumentException","Unknown audio action");return JNI_FALSE;}
    }}
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_media_AudioHost_copy(JNIEnv *env,jclass cls,jlong id){
    (void)cls;@autoreleasepool{KLJAudioPlayer *owned=find_player(id),*copy=[KLJAudioPlayer new];
        @synchronized(owned){
            if(!owned||owned.released||!owned.pcm){fail(env,"java/lang/IllegalStateException","Audio sample was unloaded or cannot be copied");return 0;}
            copy.data=owned.data;copy.frames=owned.frames;copy.channels=owned.channels;copy.sampleRate=owned.sampleRate;
        }
        // The retained immutable source outlives a concurrent template release.
        copy.pcm=klj_audio_pcm_clip((const int16_t *)((const char *)copy.data.bytes+44),copy.frames,copy.channels,copy.sampleRate);
        if(!copy.pcm){fail(env,"java/lang/IllegalStateException","Audio output cannot be opened");return 0;}
        return add_player(env,copy);
    }
}
JNIEXPORT void JNICALL Java_maclauncher_android_media_AudioHost_configure(JNIEnv *env,jclass cls,jlong id,jfloat left,jfloat right,jint loops,jfloat rate){
    (void)cls;@autoreleasepool{KLJAudioPlayer *owned=find_player(id);@synchronized(owned){
        if(!live_player(env,owned))return;
        if(!isfinite(left)||!isfinite(right)||!isfinite(rate)||left<0||left>1||right<0||right>1||rate<0.5||rate>2||loops< -1){
            fail(env,"java/lang/IllegalArgumentException","Invalid audio playback options");return;
        }
        if(owned.mpeg){
            if(rate!=1||(loops!=0&&loops!=-1)){fail(env,"java/lang/UnsupportedOperationException","MPEG audio supports normal playback rate and continuous looping");return;}
            owned.left=left;owned.right=right;apply_gain(owned);kl_mediaplayer_set_looping(owned.mpeg,loops==-1);
            if(klj_mpeg_error(owned.mpeg))fail(env,"java/lang/IllegalStateException","MPEG audio output configuration failed");return;
        }
        owned.left=left;owned.right=right;owned.rate=rate;apply_gain(owned);
        klj_audio_pcm_clip_loops(owned.pcm,loops);
    }}
}
JNIEXPORT jdoubleArray JNICALL Java_maclauncher_android_media_AudioHost_decodeStats(JNIEnv *env,jclass cls,jstring path,jobject fd,jlong offset,jlong size){
    (void)cls;@autoreleasepool{NSData *source=read_source(env,path,fd,offset,size);if(!source)return NULL;
        SF_INFO info={0};double rms=0;if(!decode(env,source,&info,&rms))return NULL;
        double values[]={info.frames,info.samplerate,info.channels,rms};jdoubleArray out=(*env)->NewDoubleArray(env,4);
        if(out)(*env)->SetDoubleArrayRegion(env,out,0,4,values);return out;
    }
}
JNIEXPORT jint JNICALL Java_maclauncher_android_media_AudioHost_time(JNIEnv *env,jclass cls,jlong id,jboolean position){
    (void)cls;@autoreleasepool{KLJAudioPlayer *owned=find_player(id);@synchronized(owned){
        if(!live_player(env,owned))return 0;
        if(owned.mpeg)return position?kl_mediaplayer_position_ms(owned.mpeg):kl_mediaplayer_duration_ms(owned.mpeg);
        uint64_t frames=owned.frames;
        if(position){frames=klj_audio_pcm_position(owned.pcm);uint64_t current=frames%owned.frames;frames=current||!frames||klj_audio_pcm_playing(owned.pcm)?current:owned.frames;}
        double ms=1000.0*(double)frames/owned.sampleRate;
        return (jint)fmin((double)INT32_MAX,fmax(0.0,ms));
    }}
}
JNIEXPORT void JNICALL Java_maclauncher_android_media_AudioHost_seek(JNIEnv *env,jclass cls,jlong id,jlong milliseconds){
    (void)cls;@autoreleasepool{KLJAudioPlayer *owned=find_player(id);@synchronized(owned){
        if(!live_player(env,owned))return;
        if(milliseconds<0)milliseconds=0;
        if(owned.mpeg){kl_mediaplayer_seek_ms(owned.mpeg,(int)(milliseconds>INT32_MAX?INT32_MAX:milliseconds));if(klj_mpeg_error(owned.mpeg))fail(env,"java/lang/IllegalStateException","MPEG audio seek failed");}
        else klj_audio_pcm_clip_seek(owned.pcm,(uint64_t)fmin((double)owned.frames,milliseconds*(double)owned.sampleRate/1000.0));
    }}
}
