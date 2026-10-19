#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#include <media/NdkMediaFormat.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
extern void klj_extractor_esds(AMediaFormat *,const void *,size_t);
#include <stdlib.h>
#include <string.h>
#include "MediaExtractorOwner.h"
#include "guest-paths.h"
#include "network-policy.h"
#include "KleptonDemuxBuffer.inc"
#include "media-data-loader.m"

static int64_t micros(CMTime value) {
    return CMTIME_IS_NUMERIC(value) ? CMTimeConvertScale(value,1000000,kCMTimeRoundingMethod_RoundTowardZero).value : -1;
}
static const char *mime(CMFormatDescriptionRef format) {
    FourCharCode type=CMFormatDescriptionGetMediaType(format),sub=CMFormatDescriptionGetMediaSubType(format);
    if(type==kCMMediaType_Video)switch(sub){
        case kCMVideoCodecType_H264:return "video/avc";
        case kCMVideoCodecType_HEVC:return "video/hevc";
        case kCMVideoCodecType_JPEG:return "video/mjpeg";
        case kCMVideoCodecType_MPEG4Video:return "video/mp4v-es";
        case kCMVideoCodecType_MPEG2Video:return "video/mpeg2";
        case kCMVideoCodecType_VP9:return "video/x-vnd.on2.vp9";
        case kCMVideoCodecType_AV1:return "video/av01";
    }
    if(type==kCMMediaType_Audio)switch(sub){
        case kAudioFormatMPEG4AAC:case kAudioFormatMPEG4AAC_HE:case kAudioFormatMPEG4AAC_HE_V2:return "audio/mp4a-latm";
        case kAudioFormatMPEGLayer3:return "audio/mpeg";
        case kAudioFormatMPEGLayer2:return "audio/mpeg-L2";
        case kAudioFormatMPEGLayer1:return "audio/mpeg-L1";
        case kAudioFormatLinearPCM:return "audio/raw";
        case kAudioFormatAppleLossless:return "audio/alac";
        case kAudioFormatFLAC:return "audio/flac";
        case kAudioFormatOpus:return "audio/opus";
        case kAudioFormatAC3:return "audio/ac3";
        case kAudioFormatEnhancedAC3:return "audio/eac3";
        case kAudioFormatAMR:return "audio/3gpp";
    }
    if(type==kCMMediaType_Subtitle||type==kCMMediaType_Text)return "text/plain";
    return "application/octet-stream";
}

@interface KljExtractorTrack : NSObject {
@public
    NSLock *readerLock;
    BOOL cancelled;
    AVAssetTrack *track;
    AVAssetReader *reader;
    AVAssetReaderTrackOutput *output;
    AMediaFormat *format;
    KljSampleBuffer *sample;
    BOOL selected,started,annexB;
    int error;
}
- (int)start:(CMTime)position mode:(int)mode;
- (int)pull;
- (void)retireReader;
- (void)cancel;
@end
@implementation KljExtractorTrack
- (instancetype)init { if((self=[super init]))readerLock=[NSLock new];return self; }
- (void)cancel { [readerLock lock];cancelled=YES;AVAssetReader *retired=reader;[readerLock unlock];[retired cancelReading]; }
- (void)retireReader { [readerLock lock];AVAssetReader *retired=reader;reader=nil;output=nil;[readerLock unlock];[retired cancelReading];started=NO;if(sample){sample->eos=1;free(sample->cur);sample->cur=NULL;sample->cur_len=0;} }
- (void)dealloc { [self retireReader];if(format)AMediaFormat_delete(format);if(sample)free(sample->psets);free(sample); }
- (int)start:(CMTime)position mode:(int)mode {
    [self retireReader];error=0;
    // AVFoundation owns the actual sample table. Select an actual sync sample
    // using its cursor, retaining Android's previous/next/closest semantics.
    if(CMTIME_IS_NUMERIC(position)&&CMTimeCompare(position,kCMTimeZero)>0){
        AVSampleCursor *cursor=[track makeSampleCursorWithPresentationTimeStamp:position];
        if(cursor){
            AVSampleCursor *previous=[cursor copy],*next=[cursor copy];
            while((!previous.currentSampleSyncInfo.sampleIsFullSync||CMTimeCompare(previous.presentationTimeStamp,position)>0)&&[previous stepInDecodeOrderByCount:-1]!=0){}
            while((!next.currentSampleSyncInfo.sampleIsFullSync||CMTimeCompare(next.presentationTimeStamp,position)<0)&&[next stepInDecodeOrderByCount:1]!=0){}
            if(mode==0)position=previous.presentationTimeStamp;
            else if(mode==1)position=next.presentationTimeStamp;
            else {double a=fabs(CMTimeGetSeconds(CMTimeSubtract(position,previous.presentationTimeStamp))),b=fabs(CMTimeGetSeconds(CMTimeSubtract(next.presentationTimeStamp,position)));position=a<=b?previous.presentationTimeStamp:next.presentationTimeStamp;}
        }
    }
    NSError *failure=nil;AVAssetReader *next=[AVAssetReader assetReaderWithAsset:track.asset error:&failure];
    if(!next){fprintf(stderr,"Android media reader: %s\n",failure.localizedDescription.UTF8String?:"creation failed");return error=-EIO;}
    AVAssetReaderTrackOutput *nextOutput=[AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
    nextOutput.alwaysCopiesSampleData=NO;
    if(![next canAddOutput:nextOutput])return error=-ENOTSUP;
    [next addOutput:nextOutput];
    if(CMTIME_IS_NUMERIC(position)&&CMTimeCompare(position,kCMTimeZero)>0)next.timeRange=CMTimeRangeMake(position,kCMTimePositiveInfinity);
    [readerLock lock];BOOL stopped=cancelled;if(!stopped){reader=next;output=nextOutput;}[readerLock unlock];
    if(stopped)return error=-ECANCELED;
    BOOL active=[next startReading];
    [readerLock lock];stopped=cancelled;[readerLock unlock];
    if(stopped){[next cancelReading];return error=-ECANCELED;}
    if(!active){fprintf(stderr,"Android media reader: %s\n",next.error.localizedDescription.UTF8String?:"start failed");return error=-EIO;}
    started=YES;sample->eos=0;return [self pull]<0?error:0;
}
- (int)pull {
    [readerLock lock];AVAssetReader *activeReader=reader;AVAssetReaderTrackOutput *activeOutput=output;BOOL stopped=cancelled;[readerLock unlock];
    if(stopped)return error=-ECANCELED;
    for(;;){
        CMSampleBufferRef buffer=[activeOutput copyNextSampleBuffer];
        if(!buffer){sample->eos=1;free(sample->cur);sample->cur=NULL;sample->cur_len=0;if(activeReader.status==AVAssetReaderStatusFailed){fprintf(stderr,"Android media reader: %s\n",activeReader.error.localizedDescription.UTF8String?:"sample read failed");error=-EIO;}return error?error:0;}
        // AVAssetReader can emit timing/discontinuity markers without sample bytes.
        // Preserve Klepton's existing skip behavior for these markers.
        if(!CMSampleBufferGetDataBuffer(buffer)){CFRelease(buffer);continue;}
        BOOL okay=NO;
        if(annexB)okay=demux_convert(sample,buffer);
        else {
            CMBlockBufferRef data=CMSampleBufferGetDataBuffer(buffer);
            if(data){size_t size=CMBlockBufferGetDataLength(data);unsigned char *bytes=malloc(size?size:1);
                if(bytes&&CMBlockBufferCopyDataBytes(data,0,size,bytes)==noErr){free(sample->cur);sample->cur=bytes;sample->cur_len=size;sample->cur_key=1;okay=YES;}else free(bytes);
            }
        }
        if(okay)sample->cur_pts_us=micros(CMSampleBufferGetPresentationTimeStamp(buffer));
        CFRelease(buffer);
        if(okay)return 1;
        // A nonempty sample that cannot allocate/convert is an error, never EOS.
        fprintf(stderr,"Android media sample conversion failed\n");return error=-EIO;
    }
}
@end

@interface KljExtractor : NSObject {
@public
    pthread_mutex_t operations,control;
    atomic_bool closed;
    AVURLAsset *asset;
    id sourceOwner;
    klj_media_source *source;
    NSArray<KljExtractorTrack *> *tracks;
    AMediaFormat *fileFormat;
    BOOL setting,configured;
    int sourceError;
    int64_t seekTime;
    int seekMode;
    NSString *logSession;
}
- (int)setAsset:(AVURLAsset *)value owner:(id)owner source:(klj_media_source *)callback;
- (KljExtractorTrack *)current;
- (int)sampleError;
- (void)close;
@end

static KljExtractorTrack *describeTrack(AVAssetTrack *track,int64_t duration) {
    CMFormatDescriptionRef description=(__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
    if(!description)return nil;
    KljExtractorTrack *result=[KljExtractorTrack new];result->track=track;
    result->sample=calloc(1,sizeof(*result->sample));result->format=AMediaFormat_new();
    if(!result->sample||!result->format)return nil;
    result->sample->nal_len_size=4;result->sample->eos=1;
    const char *type=mime(description);AMediaFormat *format=result->format;
    AMediaFormat_setString(format,AMEDIAFORMAT_KEY_MIME,type);
    AMediaFormat_setInt64(format,AMEDIAFORMAT_KEY_DURATION,duration);
    AMediaFormat_setInt32(format,"track-id",track.trackID);
    if(track.languageCode)AMediaFormat_setString(format,AMEDIAFORMAT_KEY_LANGUAGE,track.languageCode.UTF8String);
    if(track.estimatedDataRate>0)AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_BIT_RATE,(int32_t)fmin(INT_MAX,track.estimatedDataRate));
    FourCharCode mediaType=CMFormatDescriptionGetMediaType(description),codec=CMFormatDescriptionGetMediaSubType(description);
    if(mediaType==kCMMediaType_Video){
        CMVideoDimensions size=CMVideoFormatDescriptionGetDimensions(description);
        AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_WIDTH,size.width);AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_HEIGHT,size.height);
        if(track.nominalFrameRate>0)AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_FRAME_RATE,(int32_t)llround(track.nominalFrameRate));
        int64_t bytes=(int64_t)size.width*size.height;if(bytes>0&&bytes<=INT_MAX)AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,(int32_t)bytes);
        CGAffineTransform transform=track.preferredTransform;int rotation=(int)llround(atan2(transform.b,transform.a)*180/M_PI);if(rotation<0)rotation+=360;
        if(rotation)AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_ROTATION,rotation);
        result->annexB=codec==kCMVideoCodecType_H264||codec==kCMVideoCodecType_HEVC;
        if(result->annexB){
            if(!demux_build_psets(result->sample,description,codec==kCMVideoCodecType_HEVC))return nil;
            // Existing native decoder accepts Annex-B. Preserve the same sample
            // bytes and expose the actual codec initialization data to both APIs.
            if(codec==kCMVideoCodecType_HEVC)AMediaFormat_setBuffer(format,"csd-0",result->sample->psets,result->sample->psets_len);
            else {size_t count=0;int width=0;const uint8_t *bytes=NULL;size_t length=0;
                if(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description,0,&bytes,&length,&count,&width)==noErr){
                    for(size_t i=0;i<count;i++){if(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description,i,&bytes,&length,NULL,NULL)!=noErr)break;
                        NSMutableData *data=[NSMutableData dataWithBytes:k_startcode length:4];[data appendBytes:bytes length:length];
                        char key[32];snprintf(key,sizeof key,"csd-%zu",i);AMediaFormat_setBuffer(format,key,data.mutableBytes,data.length);
                    }
                }
            }
        }
    } else if(mediaType==kCMMediaType_Audio){
        const AudioStreamBasicDescription *audio=CMAudioFormatDescriptionGetStreamBasicDescription(description);
        if(audio){AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_SAMPLE_RATE,(int32_t)audio->mSampleRate);AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_CHANNEL_COUNT,(int32_t)audio->mChannelsPerFrame);
            if(codec==kAudioFormatLinearPCM){int encoding=audio->mFormatFlags&kAudioFormatFlagIsFloat?4:audio->mBitsPerChannel==8?3:audio->mBitsPerChannel==16?2:audio->mBitsPerChannel==24?21:audio->mBitsPerChannel==32?22:0;if(encoding)AMediaFormat_setInt32(format,"pcm-encoding",encoding);}
        }
        size_t cookieSize=0;const void *cookie=CMAudioFormatDescriptionGetMagicCookie(description,&cookieSize);
        if(codec==kAudioFormatMPEG4AAC||codec==kAudioFormatMPEG4AAC_HE||codec==kAudioFormatMPEG4AAC_HE_V2){
            if(cookie&&cookieSize)klj_extractor_esds(format,cookie,cookieSize);
            NSDictionary *atoms=(__bridge NSDictionary*)CMFormatDescriptionGetExtension(description,kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
            NSData *esds=atoms[@"esds"];if([esds isKindOfClass:NSData.class]&&esds.length>4)klj_extractor_esds(format,(const uint8_t*)esds.bytes+4,esds.length-4);
        }
    }
    return result;
}

@implementation KljExtractor
- (instancetype)init { if((self=[super init])){pthread_mutex_init(&operations,NULL);pthread_mutex_init(&control,NULL);atomic_init(&closed,false);seekMode=2;}return self; }
- (void)close {
    if(atomic_exchange(&closed,true))return;
    pthread_mutex_lock(&control);AVURLAsset *retiredAsset=asset;NSArray *retiredTracks=tracks;klj_media_source *retiredSource=source;if(retiredSource)klj_media_source_retain(retiredSource);pthread_mutex_unlock(&control);
    [retiredAsset cancelLoading];for(KljExtractorTrack *track in retiredTracks)[track cancel];
    if(retiredSource){klj_media_source_close(retiredSource);klj_media_source_delete(retiredSource);}
}
- (void)dealloc { [self close];tracks=nil;sourceOwner=nil;asset=nil;if(source)klj_media_source_delete(source);if(fileFormat)AMediaFormat_delete(fileFormat);pthread_mutex_destroy(&control);pthread_mutex_destroy(&operations); }
- (int)setAsset:(AVURLAsset *)value owner:(id)owner source:(klj_media_source *)callback {
    pthread_mutex_lock(&operations);
    if(atomic_load(&closed)||configured||setting){pthread_mutex_unlock(&operations);return -EINVAL;}
    setting=YES;
    pthread_mutex_lock(&control);asset=value;sourceOwner=owner;source=callback;if(source)klj_media_source_retain(source);pthread_mutex_unlock(&control);
    __block NSArray<AVAssetTrack*> *found=nil;__block NSError *failure=nil;
    dispatch_semaphore_t ready=dispatch_semaphore_create(0);
    [value loadValuesAsynchronouslyForKeys:@[@"tracks",@"duration",@"hasProtectedContent"] completionHandler:^{
        if([value statusOfValueForKey:@"tracks" error:&failure]==AVKeyValueStatusLoaded)found=value.tracks;
        dispatch_semaphore_signal(ready);
    }];
    dispatch_semaphore_wait(ready,DISPATCH_TIME_FOREVER);
    int status=0;
    if(atomic_load(&closed))status=-ECANCELED;
    else if(failure||!found)status=-EIO;
    else if(value.hasProtectedContent)status=-ENOTSUP;
    NSMutableArray<KljExtractorTrack*> *descriptions=[NSMutableArray array];int64_t duration=micros(value.duration);
    if(!status)for(AVAssetTrack *track in found){KljExtractorTrack *description=describeTrack(track,duration);if(!description){status=-ENOMEM;break;}[descriptions addObject:description];}
    if(!status){
        fileFormat=AMediaFormat_new();if(!fileFormat)status=-ENOMEM;
        else {AMediaFormat_setInt64(fileFormat,AMEDIAFORMAT_KEY_DURATION,duration);AMediaFormat_setInt32(fileFormat,"track-count",(int32_t)descriptions.count);
            NSString *extension=value.URL.pathExtension.lowercaseString;
            const char *type=[extension isEqualToString:@"mp3"]?"audio/mpeg":[extension isEqualToString:@"wav"]?"audio/x-wav":[extension isEqualToString:@"mov"]?"video/quicktime":([extension isEqualToString:@"mp4"]||[extension isEqualToString:@"m4v"]||[extension isEqualToString:@"m4a"])?"video/mp4":NULL;
            if(type)AMediaFormat_setString(fileFormat,AMEDIAFORMAT_KEY_MIME,type);
            pthread_mutex_lock(&control);tracks=[descriptions copy];pthread_mutex_unlock(&control);configured=YES;
        }
    }
    if(atomic_load(&closed)&&!status)status=-ECANCELED;
    klj_media_source *retired=NULL;id retiredOwner=nil;AVURLAsset *retiredAsset=nil;
    if(status){
        pthread_mutex_lock(&control);retired=source;source=NULL;retiredOwner=sourceOwner;retiredAsset=asset;tracks=nil;sourceOwner=nil;asset=nil;pthread_mutex_unlock(&control);
        if(fileFormat){AMediaFormat_delete(fileFormat);fileFormat=NULL;}configured=NO;
    }
    setting=NO;sourceError=status;
    pthread_mutex_unlock(&operations);
    if(retired){klj_media_source_close(retired);klj_media_source_delete(retired);}
    // Keep the resource-loader delegate alive through cancellation callbacks.
    (void)retiredOwner;(void)retiredAsset;return status;
}
- (int)sampleError { if(atomic_load(&closed))return -ECANCELED;if(!configured)return sourceError?:-EINVAL;for(KljExtractorTrack *track in tracks)if(track->selected&&track->error)return track->error;return 0; }
- (KljExtractorTrack *)current {
    if(atomic_load(&closed)||!configured)return nil;
    KljExtractorTrack *best=nil;
    for(KljExtractorTrack *track in tracks){if(!track->selected)continue;
        if(!track->started&&[track start:CMTimeMake(seekTime,1000000) mode:seekMode]!=0)continue;
        if(track->sample->eos||track->error)continue;
        if(!best||track->sample->cur_pts_us<best->sample->cur_pts_us)best=track;
    }
    return best;
}
@end

static KljExtractor *extractor(KLJExtractor *value){return (__bridge KljExtractor*)value;}
KLJExtractor *klj_extractor_create(void){@autoreleasepool{return (KLJExtractor*)CFBridgingRetain([KljExtractor new]);}}
void klj_extractor_retain(KLJExtractor *value){if(value)CFRetain(value);}
void klj_extractor_release(KLJExtractor *value){if(value)CFRelease(value);}
void klj_extractor_close(KLJExtractor *value){@autoreleasepool{KljExtractor *owner=extractor(value);[owner close];}}

int klj_extractor_set_source(KLJExtractor *value,klj_media_source *source){
    if(!value||!source)return -EINVAL;@autoreleasepool{KljExtractor *extract=extractor(value);void *owner=NULL;AVURLAsset *asset=klj_media_data_asset(source,&owner);if(!asset)return -EIO;return [extract setAsset:asset owner:CFBridgingRelease(owner) source:source];}
}
int klj_extractor_set_fd(KLJExtractor *value,int fd,int64_t offset,int64_t size){
    if(!value||fd<0||offset<0||size<0)return -EINVAL;klj_media_source *source=klj_media_source_from_fd(fd,offset,size);if(!source)return -errno;int result=klj_extractor_set_source(value,source);klj_media_source_delete(source);return result;
}
int klj_extractor_set_path(KLJExtractor *value,const char *path,const char *const *keys,const char *const *values,size_t count){
    if(!value||!path)return -EINVAL;@autoreleasepool{KljExtractor *owner=extractor(value);NSString *text=[NSString stringWithUTF8String:path];NSURL *url=[NSURL URLWithString:text];
        if(!url.scheme.length){char mapped[PATH_MAX];const char *actual=klj_mapped_guest_path(path,mapped,sizeof mapped);url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:actual?:path]];}
        if(url.host.length&&!klj_network_host_allowed(url.host.UTF8String))return -ENOENT;
        NSMutableDictionary *headers=[NSMutableDictionary dictionary];for(size_t i=0;i<count;i++)if(keys[i]&&values[i])headers[@(keys[i])]=@(values[i]);
        AVURLAsset *asset=[AVURLAsset URLAssetWithURL:url options:headers.count?@{@"AVURLAssetHTTPHeaderFieldsKey":headers}:nil];
        return asset?[owner setAsset:asset owner:nil source:NULL]:-EINVAL;
    }
}
size_t klj_extractor_count(KLJExtractor *value){if(!value)return 0;KljExtractor *owner=extractor(value);pthread_mutex_lock(&owner->operations);size_t result=atomic_load(&owner->closed)?0:owner->tracks.count;pthread_mutex_unlock(&owner->operations);return result;}
AMediaFormat *klj_extractor_format(KLJExtractor *value,int index){if(!value)return NULL;@autoreleasepool{KljExtractor *owner=extractor(value);pthread_mutex_lock(&owner->operations);AMediaFormat *original=!atomic_load(&owner->closed)?index==-1?owner->fileFormat:index>=0&&(NSUInteger)index<owner->tracks.count?owner->tracks[(NSUInteger)index]->format:NULL:NULL;AMediaFormat *copy=original?AMediaFormat_new():NULL;if(copy)AMediaFormat_copy(copy,original);pthread_mutex_unlock(&owner->operations);return copy;}}
int klj_extractor_select(KLJExtractor *value,size_t index,bool selected){if(!value)return -EINVAL;@autoreleasepool{KljExtractor *owner=extractor(value);pthread_mutex_lock(&owner->operations);int status=-EINVAL;if(!atomic_load(&owner->closed)&&index<owner->tracks.count){KljExtractorTrack *track=owner->tracks[index];track->selected=selected;if(!selected)[track retireReader];status=0;}pthread_mutex_unlock(&owner->operations);return status;}}
int klj_extractor_seek(KLJExtractor *value,int64_t time,int mode){if(!value||mode<0||mode>2)return -EINVAL;@autoreleasepool{KljExtractor *owner=extractor(value);pthread_mutex_lock(&owner->operations);int status=-EINVAL;if(!atomic_load(&owner->closed)&&owner->configured){owner->seekTime=time<0?0:time;owner->seekMode=mode;for(KljExtractorTrack *track in owner->tracks)[track retireReader];status=0;}pthread_mutex_unlock(&owner->operations);return status;}}
int klj_extractor_advance(KLJExtractor *value){if(!value)return -EINVAL;@autoreleasepool{KljExtractor *owner=extractor(value);pthread_mutex_lock(&owner->operations);KljExtractorTrack *current=[owner current];int result=current?[current pull]:[owner sampleError];if(result>=0){KljExtractorTrack *next=[owner current];result=[owner sampleError];if(!result)result=next?1:0;}pthread_mutex_unlock(&owner->operations);return result;}}
ssize_t klj_extractor_read(KLJExtractor *value,void *buffer,size_t capacity){if(!value||(!buffer&&capacity))return -EINVAL;@autoreleasepool{KljExtractor *owner=extractor(value);pthread_mutex_lock(&owner->operations);KljExtractorTrack *current=[owner current];ssize_t result=[owner sampleError];if(!result)result=-1;if(current){size_t size=current->sample->cur_len;if(size>SSIZE_MAX)result=-EOVERFLOW;else if(size>capacity)result=-ENOSPC;else{if(size)memcpy(buffer,current->sample->cur,size);result=(ssize_t)size;}}pthread_mutex_unlock(&owner->operations);return result;}}
static int64_t sample_value(KLJExtractor *value,int field){if(!value)return -1;@autoreleasepool{KljExtractor *owner=extractor(value);pthread_mutex_lock(&owner->operations);KljExtractorTrack *current=[owner current];int64_t result=[owner sampleError];if(!result)result=-1;if(current)switch(field){case 0:result=current->sample->cur_pts_us;break;case 1:result=(int64_t)current->sample->cur_len;break;case 2:result=(int64_t)[owner->tracks indexOfObjectIdenticalTo:current];break;case 3:result=current->sample->cur_key?1:0;break;}pthread_mutex_unlock(&owner->operations);return result;}}
int64_t klj_extractor_sample_time(KLJExtractor *value){return sample_value(value,0);}
int64_t klj_extractor_sample_size(KLJExtractor *value){return sample_value(value,1);}
int klj_extractor_sample_track(KLJExtractor *value){return (int)sample_value(value,2);}
int klj_extractor_sample_flags(KLJExtractor *value){return (int)sample_value(value,3);}
int64_t klj_extractor_cached_duration(KLJExtractor *value,bool *eos){(void)value;if(eos)*eos=false;return -1; /* AVAssetReader exposes no Android streaming-cache estimate. */}
void klj_extractor_log_session(KLJExtractor *value,const char *id){if(value)@autoreleasepool{KljExtractor *owner=extractor(value);pthread_mutex_lock(&owner->operations);owner->logSession=id?@(id):nil;pthread_mutex_unlock(&owner->operations);}}

int klj_extractor_sample_time_status(KLJExtractor *value,int64_t *time){if(!value||!time)return -EINVAL;@autoreleasepool{KljExtractor *owner=extractor(value);pthread_mutex_lock(&owner->operations);KljExtractorTrack *current=[owner current];int result=[owner sampleError];if(!result){if(current)*time=current->sample->cur_pts_us;else result=1;}pthread_mutex_unlock(&owner->operations);return result;}}
int klj_extractor_sample_format(KLJExtractor *value,AMediaFormat *format){if(!value||!format)return -EINVAL;@autoreleasepool{KljExtractor *owner=extractor(value);pthread_mutex_lock(&owner->operations);KljExtractorTrack *current=[owner current];int result=[owner sampleError];if(!result){if(!current)result=1;else{AMediaFormat_clear(format);AMediaFormat_setInt32(format,AMEDIAFORMAT_KEY_TRACK_INDEX,(int32_t)[owner->tracks indexOfObjectIdenticalTo:current]);const char *type=NULL;if(AMediaFormat_getString(current->format,AMEDIAFORMAT_KEY_MIME,&type))AMediaFormat_setString(format,AMEDIAFORMAT_KEY_MIME,type);}}pthread_mutex_unlock(&owner->operations);return result;}}
