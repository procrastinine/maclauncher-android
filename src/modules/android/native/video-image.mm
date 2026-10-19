#import <Accelerate/Accelerate.h>
#include "video-image.h"
#include "video-pixels.hpp"
#include "angle-metal.hpp"
#include <mutex>
#include <memory>
#include <cstring>
#include <utility>

namespace {
constexpr uint32_t fourcc(char a,char b,char c,char d){return uint32_t(a)|(uint32_t(b)<<8)|(uint32_t(c)<<16)|(uint32_t(d)<<24);}
constexpr auto NV12=fourcc('N','V','1','2'),NV21=fourcc('N','V','2','1'),YV12=fourcc('Y','V','1','2'),P010=fourcc('P','0','1','0');
struct Image {
 KljVideoBuffer source;void *display;KljVideoResolve resolve;
 id<MTLDevice> device;id<MTLCommandQueue> queue;id<MTLTexture> texture;
 id<MTLSharedEvent> ready,consumed;id<MTLCommandBuffer> last;
 uint64_t serial=0;std::mutex mutex;
 KljVideoInterop interop{};
 ~Image(){[last waitUntilCompleted];if(interop.owner)interop.destroy(interop.owner);}
 template<class T>T function(const char*name){return reinterpret_cast<T>(resolve(name));}
};
struct Colour {int standard,transfer;bool full;};
Colour colour(int32_t data){
 // android/data_space.h bitfields, plus original graphics.h legacy values.
 if(data==0x104)return {1,3,false}; // BT709
 if(data==0x102)return {2,3,false}; // BT601_625
 if(data==0x103)return {4,3,false}; // BT601_525
 if(data==0x101)return {2,3,true};  // JFIF
 return {int((uint32_t(data)>>16)&63),int((uint32_t(data)>>22)&31),((uint32_t(data)>>27)&7)==1};
}
void attach(CVPixelBufferRef pixels,Colour c){
 auto matrix=c.standard==1?kCVImageBufferYCbCrMatrix_ITU_R_709_2:c.standard==6||c.standard==7?kCVImageBufferYCbCrMatrix_ITU_R_2020:kCVImageBufferYCbCrMatrix_ITU_R_601_4;
 auto primaries=c.standard==6||c.standard==7?kCVImageBufferColorPrimaries_ITU_R_2020:c.standard==4||c.standard==5?kCVImageBufferColorPrimaries_SMPTE_C:c.standard==2||c.standard==3?kCVImageBufferColorPrimaries_EBU_3213:kCVImageBufferColorPrimaries_ITU_R_709_2;
 auto transfer=c.transfer==1?kCVImageBufferTransferFunction_Linear:c.transfer==2?kCVImageBufferTransferFunction_sRGB:c.transfer==7?kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ:c.transfer==8?kCVImageBufferTransferFunction_ITU_R_2100_HLG:kCVImageBufferTransferFunction_ITU_R_709_2;
 CVBufferSetAttachment(pixels,kCVImageBufferYCbCrMatrixKey,matrix,kCVAttachmentMode_ShouldPropagate);
 CVBufferSetAttachment(pixels,kCVImageBufferColorPrimariesKey,primaries,kCVAttachmentMode_ShouldPropagate);
 CVBufferSetAttachment(pixels,kCVImageBufferTransferFunctionKey,transfer,kCVAttachmentMode_ShouldPropagate);
}
CVPixelBufferRef pixels(const KljVideoBuffer&s,Colour c){
 // NV21 differs from CoreVideo NV12 only by paired chroma byte order.
 // Accelerate permutes the chroma view; luma and all original plane layout
 // remain shared. The temporary view is retained by this CVPixelBuffer only.
 void* chroma=nullptr;size_t chromaRow=0;
 if(s.fourcc==NV21){
  auto&p=s.planes[1];chromaRow=p.stride;chroma=malloc(chromaRow*p.height);if(!chroma)return nullptr;
  auto*input=(uint8_t*)(uintptr_t)(s.address+p.offset);auto*output=(uint8_t*)chroma;
  vImage_Buffer from={input,p.height,p.width/2,p.stride},to={output,p.height,p.width/2,p.stride};
  const uint8_t permutation[]={1,0,3,2};
  if(p.width>=2&&vImagePermuteChannels_ARGB8888(&from,&to,permutation,kvImageNoFlags)){free(chroma);return nullptr;}
  if(p.width&1)for(size_t y=0;y<p.height;y++){size_t x=y*p.stride+(p.width-1)*2;output[x]=input[x+1];output[x+1]=input[x];}
 }
 if(s.fourcc==YV12){
  auto&cb=s.planes[2];auto&cr=s.planes[1];chromaRow=(cb.width*2+15)&~size_t(15);chroma=malloc(chromaRow*cb.height);if(!chroma)return nullptr;
  vImage_Buffer cbView={(void*)(uintptr_t)(s.address+cb.offset),cb.height,cb.width,cb.stride},crView={(void*)(uintptr_t)(s.address+cr.offset),cr.height,cr.width,cr.stride};
  const vImage_Buffer*from[]={&cbView,&crView};void*to[]={chroma,(uint8_t*)chroma+1};
  if(vImageConvert_PlanarToChunky8(from,to,2,2,cb.width,cb.height,chromaRow,kvImageNoFlags)){free(chroma);return nullptr;}
 }
 OSType format=s.fourcc==P010?(c.full?kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange):(c.full?kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
 void*base[3];size_t widths[3],heights[3],rows[3];
 for(unsigned n=0;n<2;n++){auto&p=s.planes[n];base[n]=(void*)(uintptr_t)(s.address+p.offset);widths[n]=p.width;heights[n]=p.height;rows[n]=p.stride;}
 if(chroma){base[1]=chroma;rows[1]=chromaRow;}
 CVPixelBufferRef out=nullptr;
 auto release=[](void*context,const void*,size_t,size_t,const void*[]){free(context);};
 if(CVPixelBufferCreateWithPlanarBytes(nullptr,s.width,s.height,format,nullptr,0,2,base,widths,heights,rows,release,chroma,nullptr,&out)!=kCVReturnSuccess){free(chroma);return nullptr;}
 attach(out,c);return out;
}
bool valid(const KljVideoBuffer&s){
 unsigned count=s.fourcc==YV12?3:s.fourcc==NV12||s.fourcc==NV21||s.fourcc==P010?2:0;
 if(!s.address||!s.width||!s.height||s.plane_count!=count||!count||s.address>UINT64_MAX-s.length)return false;
 for(unsigned i=0;i<count;i++){
  auto&p=s.planes[i];uint64_t w=i?(uint64_t(s.width)+1)/2:s.width,h=i?(uint64_t(s.height)+1)/2:s.height;
  uint64_t bpp=(s.fourcc==P010?2:1)*(i&&count==2?2:1);
  if(p.width!=w||p.height!=h||p.stride<w*bpp||p.offset>s.length||h>(s.length-p.offset)/p.stride)return false;
 }return true;
}
}
namespace {
void *create(const KljVideoBuffer*s,void*device,void*display,KljVideoResolve resolve,const KljVideoInterop*interop,void**image){@autoreleasepool{
 auto state=std::make_unique<Image>();if(interop)state->interop=*interop;
 if(!s||!valid(*s)||!device||!display||!resolve||!image)return nullptr;*image=nullptr;
 for(auto name:{"eglCreateImageKHR","eglCreateSync","eglDestroySync","eglWaitSync","eglClientWaitSync","eglGetCurrentContext","glFlush"})if(!resolve(name))return nullptr;
 state->source=*s;state->display=display;state->resolve=resolve;state->device=(__bridge id<MTLDevice>)device;
 state->queue=[state->device newCommandQueue];state->ready=interop?(__bridge id<MTLSharedEvent>)interop->ready:[state->device newSharedEvent];state->consumed=interop?(__bridge id<MTLSharedEvent>)interop->consumed:[state->device newSharedEvent];
 auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s->fourcc==P010?MTLPixelFormatRGBA16Float:MTLPixelFormatRGBA8Unorm width:s->width height:s->height mipmapped:NO];descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsageRenderTarget;
 state->texture=[state->device newTextureWithDescriptor:descriptor];
 if(!state->queue||!state->ready||!state->consumed||!state->texture)return nullptr;
 const int attributes[]={0x3038};
 *image=interop?interop->import_texture(interop->owner,(__bridge void*)state->texture,s->width,s->height,s->fourcc==P010?97:37):state->function<void*(*)(void*,void*,unsigned,void*,const int*)>("eglCreateImageKHR")(display,nullptr,0x34a7,(__bridge void*)state->texture,attributes);
 return *image?state.release():nullptr;
}}
}
extern "C" void *klj_video_image_create(const KljVideoBuffer*s,void*device,void*display,KljVideoResolve resolve,void**image){return create(s,device,display,resolve,nullptr,image);}
extern "C" void *klj_video_image_create_interop(const KljVideoBuffer*s,void*display,KljVideoResolve resolve,const KljVideoInterop*interop,void**image){return create(s,interop?interop->device:nullptr,display,resolve,interop,image);}
extern "C" int klj_video_image_refresh(void*handle,int32_t dataspace){@autoreleasepool{
 if(!handle)return -22;auto&s=*static_cast<Image*>(handle);std::lock_guard lock(s.mutex);
 if(!s.function<void*(*)()>("eglGetCurrentContext")())return -22;
 // Signal all earlier reads/acquire waits in the current original EGL context,
 // convert on Metal, then enqueue the inverse wait before the new image read.
 uint64_t serial=++s.serial;void *consumed=nullptr,*ready=nullptr;
 auto destroy=s.function<unsigned(*)(void*,void*)>("eglDestroySync");
 auto clean=[&]{if(consumed)destroy(s.display,consumed);if(ready)destroy(s.display,ready);};
 if(!s.interop.owner){consumed=klj_angle::sharedSync(s.display,s.consumed,serial,false,s.resolve);ready=klj_angle::sharedSync(s.display,s.ready,serial,true,s.resolve);if(!consumed||!ready){clean();return -22;}}
 // A non-IOSurface CVPixelBuffer may be uploaded by CoreImage on the CPU.
 // Observe the original EGL acquire/previous-read fence before exposing its
 // bytes to that system API; never race a GPU producer or buffer reuse.
 s.function<void(*)()>("glFlush")();
 if(s.interop.owner?s.interop.begin(s.interop.owner,serial)!=0:s.function<unsigned(*)(void*,void*,int,uint64_t)>("eglClientWaitSync")(s.display,consumed,0,UINT64_MAX)!=0x30f6){clean();return -22;}
 CVPixelBufferRef source=pixels(s.source,colour(dataspace));if(!source){clean();return -22;}
 s.function<void(*)()>("glFlush")();auto commands=[s.queue commandBuffer];commands.label=@"Android planar video image";[commands encodeWaitForEvent:s.consumed value:serial];
 // Original Skia applies the buffer's dataspace to the resulting RGB texture.
 // Keep encoded RGB in that same space instead of converting twice to sRGB.
 auto attachments=CVBufferCopyAttachments(source,kCVAttachmentMode_ShouldPropagate);
 auto output=attachments?CVImageBufferCreateColorSpaceFromAttachments(attachments):nullptr;
 bool rendered=klj_video_render(source,s.device,commands,s.texture,output);
 if(output)CGColorSpaceRelease(output);if(attachments)CFRelease(attachments);
 if(!rendered){CVPixelBufferRelease(source);clean();return -22;}
 [commands encodeSignalEvent:s.ready value:serial];[commands addCompletedHandler:^(id<MTLCommandBuffer>){CVPixelBufferRelease(source);}];[commands commit];s.last=commands;
 auto result=s.interop.owner?s.interop.end(s.interop.owner,serial):(s.function<unsigned(*)(void*,void*,int)>("eglWaitSync")(s.display,ready,0)?0:-22);clean();return result;
}}
extern "C" void klj_video_image_destroy(void*handle){
 if(!handle)return;std::unique_ptr<Image> state(static_cast<Image*>(handle));
 // The original image owner may unmap source bytes after this returns. Normal
 // retirement waits only for this image's pending conversion read.
 // Image's owner waits for this conversion and the corresponding Vulkan
 // queue wait before retiring its two per-image timeline semaphores.
}
