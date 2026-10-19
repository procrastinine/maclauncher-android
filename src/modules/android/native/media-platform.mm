#include "media-platform.h"
#include "audio-device.h"
#include "audio-capture.h"
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#include <mutex>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cerrno>
extern "C" void klj_audio_capture_select(uint32_t);
static std::mutex mediaLock;
static bool configured,closed;
static NSString *audioUID,*cameraUID;
static uint32_t audioDevice;
static uint64_t nextToken=1,audioToken;
static KljAudioCapture *audioStream;
static int audioChannels,audioRate,audioFormat;
static bool audioRecording;
static void stopCamera();
static NSArray *audioDevices(){
    uint32_t *devices=nullptr;size_t count=0;if(klj_audio_device_list(&devices,&count))return @[];
    NSMutableArray *result=[NSMutableArray array];
    for(size_t i=0;i<count;i++){
        KljAudioDevice device;if(klj_audio_device_get(devices[i],1,&device))continue;
        if(device.channels&&device.uid&&device.name){
            NSString *u=(__bridge NSString*)device.uid,*n=(__bridge NSString*)device.name;
            if(u.length&&u.length<=512&&n.length&&n.length<=512)[result addObject:@{@"uid":u,@"name":n,@"device":@(devices[i])}];
        }
        klj_audio_device_release(&device);
    }
    free(devices);return result;
}
static NSArray<AVCaptureDevice*> *videoDevices(){
    NSArray *types;if(@available(macOS 14.0,*))types=@[AVCaptureDeviceTypeExternal];else types=@[AVCaptureDeviceTypeExternalUnknown];
    return [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified].devices;
}
static NSString *obsIdentity(){return [NSDictionary dictionaryWithContentsOfFile:@"/Applications/OBS.app/Contents/PlugIns/mac-virtualcam.plugin/Contents/Info.plist"][@"OBSCameraDeviceUUID"];}
static AVCaptureDevice *selectedCamera(){for(AVCaptureDevice *device in videoDevices())if([device.uniqueID isEqual:cameraUID])return device;return nil;}
static void stopAudio(){if(audioStream){klj_audio_capture_destroy(audioStream);audioStream=nullptr;}audioRecording=false;}
static void closeAudio(){stopAudio();audioToken=0;}
char *klj_media_devices(void){@autoreleasepool{
    NSMutableArray *audio=[NSMutableArray array],*video=[NSMutableArray array];for(NSDictionary *record in audioDevices())[audio addObject:@{@"uid":record[@"uid"],@"name":record[@"name"]}];
    NSString *wanted=obsIdentity();if(wanted.length)for(AVCaptureDevice *device in videoDevices())if([device.uniqueID caseInsensitiveCompare:wanted]==NSOrderedSame)[video addObject:@{@"uid":device.uniqueID,@"name":device.localizedName}];
    NSData *data=[NSJSONSerialization dataWithJSONObject:@{@"audioInputs":audio,@"cameraInputs":video} options:0 error:nil];
    NSString *json=[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding];return json?strdup(json.UTF8String):nullptr;
}}
int klj_media_configure(const char *audio,const char *video){@autoreleasepool{
    std::lock_guard guard(mediaLock);if(configured||closed)return KLJ_MEDIA_CONFIGURED;
    NSString *a=[NSString stringWithUTF8String:audio?audio:""],*v=[NSString stringWithUTF8String:video?video:""];
    if(!a||!v||a.length>512||v.length>512)return KLJ_MEDIA_INVALID;
    uint32_t device=0;if(a.length)for(NSDictionary *record in audioDevices())if([record[@"uid"] isEqual:a]){device=[record[@"device"] unsignedIntValue];break;}
    if(a.length&&!device)return KLJ_MEDIA_NOT_FOUND;
    audioUID=a;cameraUID=v;audioDevice=device;configured=true;
    klj_audio_capture_select(device&&[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]==AVAuthorizationStatusAuthorized?device:0);
    return 0;
}}
int klj_media_authorized(int video){std::lock_guard guard(mediaLock);return configured&&!closed&&(video?cameraUID.length:audioUID.length)&&[AVCaptureDevice authorizationStatusForMediaType:video?AVMediaTypeVideo:AVMediaTypeAudio]==AVAuthorizationStatusAuthorized;}
uint32_t klj_media_audio_device(void){std::lock_guard guard(mediaLock);return configured&&!closed&&[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]==AVAuthorizationStatusAuthorized?audioDevice:0;}
int64_t klj_media_audio_open_format(int rate,int channels,int format){std::lock_guard guard(mediaLock);
    if(!configured||closed||!audioDevice||audioToken||rate<4000||rate>192000||channels<1||channels>8||(format!=1&&format!=2))return 0;
    if([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]!=AVAuthorizationStatusAuthorized)return KLJ_MEDIA_DENIED;
    audioToken=nextToken++;audioChannels=channels;audioRate=rate;audioFormat=format;return (int64_t)audioToken;
}
int64_t klj_media_audio_open(int rate,int channels){return klj_media_audio_open_format(rate,channels,1);}
int klj_media_audio_start(uint64_t token){std::lock_guard guard(mediaLock);
    if(!audioToken||audioToken!=token||closed)return false;if(audioRecording)return true;
    if([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]!=AVAuthorizationStatusAuthorized)return false;
    audioStream=klj_audio_capture_create(audioRate,audioChannels,audioFormat);audioRecording=audioStream!=nullptr;return audioRecording;
}
int klj_media_audio_read_frames(uint64_t token,void *data,size_t frames){
    if(!data||frames>INT32_MAX)return -2;
    std::lock_guard guard(mediaLock);if(!audioStream||audioToken!=token||closed||!audioRecording)return -3;
    return klj_audio_capture_read(audioStream,data,(int32_t)frames,0);
}
int klj_media_audio_read(uint64_t token,int16_t *data,size_t samples){
    if(!data||samples>8192)return -2;
    std::lock_guard guard(mediaLock);if(!audioStream||audioToken!=token||closed||!audioRecording)return -3;
    if(audioFormat!=1)return -2;
    int frames=klj_audio_capture_read(audioStream,data,(int)(samples/audioChannels),0);
    if(frames<0)return -6;int count=frames*audioChannels;return count>(int)samples?-1:count;
}
void klj_media_audio_stop(uint64_t token){std::lock_guard guard(mediaLock);if(audioToken==token)stopAudio();}
void klj_media_audio_close(uint64_t token){std::lock_guard guard(mediaLock);if(audioToken==token)closeAudio();}

// One AVCapture session and retained latest frame; the callback has no VM state.
@interface KLJCameraFrames : NSObject<AVCaptureVideoDataOutputSampleBufferDelegate> {
    NSLock *_lock;CVPixelBufferRef _latest;BOOL _closed;
}
-(CVPixelBufferRef)take;
-(void)close;
@end
@implementation KLJCameraFrames
-(instancetype)init {if((self=[super init]))_lock=[NSLock new];return self;}
-(void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection*)connection {
    (void)output;(void)connection;CVPixelBufferRef frame=CMSampleBufferGetImageBuffer(sample);if(!frame)return;
    [_lock lock];if(!_closed){CVPixelBufferRetain(frame);if(_latest)CVPixelBufferRelease(_latest);_latest=frame;}[_lock unlock];
}
-(CVPixelBufferRef)take {[_lock lock];CVPixelBufferRef frame=_latest;_latest=nullptr;[_lock unlock];return frame;}
-(void)close {[_lock lock];_closed=YES;if(_latest)CVPixelBufferRelease(_latest);_latest=nullptr;[_lock unlock];}
@end
static AVCaptureSession *cameraSession;
static KLJCameraFrames *cameraFrames;
static uint64_t cameraToken;
static int cameraWidth,cameraHeight;
static void stopCamera(){
    [cameraFrames close];[cameraSession stopRunning];
    for(AVCaptureOutput *output in cameraSession.outputs)if([output isKindOfClass:AVCaptureVideoDataOutput.class])[(AVCaptureVideoDataOutput*)output setSampleBufferDelegate:nil queue:nullptr];
    cameraFrames=nil;cameraSession=nil;
}
static std::vector<int> sizes(AVCaptureDevice *device){
    std::vector<int> result;
    for(AVCaptureDeviceFormat *format in device.formats){CMVideoDimensions size=CMVideoFormatDescriptionGetDimensions(format.formatDescription);
        if(size.width<=0||size.height<=0||(size.width&1)||(size.height&1)||size.width>1920||size.height>1080)continue;
        bool found=false;for(size_t i=0;i<result.size();i+=2)if(result[i]==size.width&&result[i+1]==size.height)found=true;
        if(!found&&result.size()<64){result.push_back(size.width);result.push_back(size.height);}
    }return result;
}
int *klj_media_camera_sizes(size_t *count){@autoreleasepool{std::lock_guard guard(mediaLock);
    auto values=configured&&!closed?sizes(selectedCamera()):std::vector<int>();*count=values.size();
    int *result=(int*)malloc(values.size()*sizeof(int));if(result&&!values.empty())memcpy(result,values.data(),values.size()*sizeof(int));return result;
}}
int64_t klj_media_camera_open(void){@autoreleasepool{std::lock_guard guard(mediaLock);
    if(!configured||closed||cameraToken||!cameraUID.length||!selectedCamera())return 0;
    if([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]!=AVAuthorizationStatusAuthorized)return KLJ_MEDIA_DENIED;
    cameraToken=nextToken++;return (int64_t)cameraToken;
}}
int klj_media_camera_start(uint64_t token,int width,int height){@autoreleasepool{std::lock_guard guard(mediaLock);
    if(!cameraToken||cameraToken!=token||closed||cameraSession)return false;
    AVCaptureDevice *device=selectedCamera();if(!device||[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]!=AVAuthorizationStatusAuthorized)return false;
    AVCaptureDeviceFormat *chosen=nil;for(AVCaptureDeviceFormat *format in device.formats){CMVideoDimensions size=CMVideoFormatDescriptionGetDimensions(format.formatDescription);if(size.width==width&&size.height==height&&width<=1920&&height<=1080&&!(width&1)&&!(height&1)){chosen=format;break;}}
    if(!chosen)return false;NSError *error=nil;AVCaptureDeviceInput *input=[AVCaptureDeviceInput deviceInputWithDevice:device error:&error];if(!input)return false;
    AVCaptureSession *session=[AVCaptureSession new];[session beginConfiguration];AVCaptureVideoDataOutput *output=[AVCaptureVideoDataOutput new];output.alwaysDiscardsLateVideoFrames=YES;
    if(![output.availableVideoCVPixelFormatTypes containsObject:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)]){[session commitConfiguration];return false;}
    output.videoSettings=@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
    if(![session canAddInput:input]||![session canAddOutput:output]){[session commitConfiguration];return false;}
    [session addInput:input];[session addOutput:output];cameraFrames=[KLJCameraFrames new];
    [output setSampleBufferDelegate:cameraFrames queue:dispatch_queue_create("Android selected camera",DISPATCH_QUEUE_SERIAL)];[session commitConfiguration];
    if(![device lockForConfiguration:&error]){[cameraFrames close];[output setSampleBufferDelegate:nil queue:nullptr];cameraFrames=nil;return false;}device.activeFormat=chosen;[device unlockForConfiguration];
    cameraSession=session;cameraWidth=width;cameraHeight=height;[session startRunning];if(!session.isRunning){stopCamera();return false;}return true;
}}
int klj_media_camera_read(uint64_t token,void *bytes,size_t capacity){std::lock_guard guard(mediaLock);
    if(!cameraToken||cameraToken!=token||closed||!cameraSession||!cameraSession.isRunning)return -1;
    const int count=cameraWidth*cameraHeight*3/2;if(!bytes||capacity<(size_t)count)return -2;
    CVPixelBufferRef frame=[cameraFrames take];if(!frame)return 0;
    if(CVPixelBufferGetWidth(frame)!=(size_t)cameraWidth||CVPixelBufferGetHeight(frame)!=(size_t)cameraHeight||CVPixelBufferGetPlaneCount(frame)!=2){CVPixelBufferRelease(frame);return -1;}
    if(CVPixelBufferLockBaseAddress(frame,kCVPixelBufferLock_ReadOnly)!=kCVReturnSuccess){CVPixelBufferRelease(frame);return -1;}
    auto *out=(uint8_t*)bytes,*y=(uint8_t*)CVPixelBufferGetBaseAddressOfPlane(frame,0),*uv=(uint8_t*)CVPixelBufferGetBaseAddressOfPlane(frame,1);
    size_t ys=CVPixelBufferGetBytesPerRowOfPlane(frame,0),uvs=CVPixelBufferGetBytesPerRowOfPlane(frame,1);
    for(int row=0;row<cameraHeight;row++)memcpy(out+row*cameraWidth,y+row*ys,cameraWidth);
    auto *vu=out+cameraWidth*cameraHeight;
    for(int row=0;row<cameraHeight/2;row++)for(int x=0;x<cameraWidth;x+=2){vu[row*cameraWidth+x]=uv[row*uvs+x+1];vu[row*cameraWidth+x+1]=uv[row*uvs+x];}
    CVPixelBufferUnlockBaseAddress(frame,kCVPixelBufferLock_ReadOnly);CVPixelBufferRelease(frame);return count;
}
void klj_media_camera_stop(uint64_t token){std::lock_guard guard(mediaLock);if(cameraToken==token)stopCamera();}
void klj_media_camera_close(uint64_t token){std::lock_guard guard(mediaLock);if(cameraToken==token){stopCamera();cameraToken=0;}}
void klj_media_shutdown(void){std::lock_guard guard(mediaLock);if(closed)return;closed=true;closeAudio();stopCamera();klj_audio_capture_select(0);audioUID=nil;cameraUID=nil;}
