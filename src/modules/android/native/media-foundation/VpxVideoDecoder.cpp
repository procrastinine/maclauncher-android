#include "VideoDecoder.h"
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace {
constexpr size_t kOutputLimit=32,kPoolLimit=48,kReferenceLimit=256u<<20,kFrameLimit=64u<<20;
bool dimensions(unsigned width,unsigned height){return width&&height&&width<=KLJ_VPX_MAX_WIDTH&&height<=KLJ_VPX_MAX_HEIGHT;}
struct Frame {
    CVPixelBufferRef pixels;
    int64_t pts;
    Frame(CVPixelBufferRef value,int64_t time):pixels(value),pts(time){}
    Frame(const Frame&)=delete;Frame&operator=(const Frame&)=delete;
    Frame(Frame&&other)noexcept:pixels(std::exchange(other.pixels,nullptr)),pts(other.pts){}
    ~Frame(){if(pixels)CVPixelBufferRelease(pixels);}
};
class VpxDecoder final:public KLJVideoDecoder {
    vpx_codec_ctx_t codec={};
    vpx_codec_iface_t *const interface;
    const bool vp9;
    bool initialized=false,closed=false;
    int error=0;
    unsigned count=0,poolWidth=0,poolHeight=0;
    bool poolFullRange=false;
    CVPixelBufferPoolRef pool=nullptr;
    std::deque<Frame> frames;
    std::mutex mutex;
    std::condition_variable finished;
    void(*notify)(void*)=nullptr;void *context=nullptr;
    unsigned notifying=0;
    std::atomic<size_t> referenceBytes{0};
    // VP9's original allocator callback prevents unbounded reference pictures
    // before they reach the platform output pool. libvpx owns reference use.
    static int allocate(void *opaque,size_t minimum,vpx_codec_frame_buffer_t *buffer){
        auto *self=static_cast<VpxDecoder*>(opaque);
        if(minimum>kFrameLimit)return -1;
        size_t prior=self->referenceBytes.fetch_add(minimum);
        if(prior>kReferenceLimit-minimum){self->referenceBytes.fetch_sub(minimum);return -1;}
        auto *bytes=static_cast<uint8_t*>(std::calloc(minimum,1));
        if(!bytes){self->referenceBytes.fetch_sub(minimum);return -1;}
        *buffer={bytes,minimum,nullptr};return 0;
    }
    static int release(void *opaque,vpx_codec_frame_buffer_t *buffer){
        auto *self=static_cast<VpxDecoder*>(opaque);
        std::free(buffer->data);self->referenceBytes.fetch_sub(buffer->size);
        *buffer={};return 0;
    }
    bool initialize(){
        vpx_codec_dec_cfg_t config={};config.threads=4;
        initialized=vpx_codec_dec_init(&codec,interface,&config,0)==VPX_CODEC_OK;
        if(initialized&&vp9&&vpx_codec_set_frame_buffer_functions(&codec,allocate,release,this)!=VPX_CODEC_OK){vpx_codec_destroy(&codec);initialized=false;}
        return initialized;
    }
    void signal(){
        std::unique_lock lock(mutex);
        auto fn=notify;auto opaque=context;if(!fn||closed)return;
        notifying++;lock.unlock();fn(opaque);lock.lock();notifying--;finished.notify_all();
    }
    bool preparePool(unsigned width,unsigned height,bool fullRange){
        if(pool&&poolWidth==width&&poolHeight==height&&poolFullRange==fullRange)return true;
        if(pool)CVPixelBufferPoolRelease(pool);pool=nullptr;
        int32_t w=width,h=height,format=fullRange?kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        CFNumberRef wn=CFNumberCreate(nullptr,kCFNumberSInt32Type,&w),hn=CFNumberCreate(nullptr,kCFNumberSInt32Type,&h),fn=CFNumberCreate(nullptr,kCFNumberSInt32Type,&format);
        CFDictionaryRef io=CFDictionaryCreate(nullptr,nullptr,nullptr,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        const void *keys[]={kCVPixelBufferWidthKey,kCVPixelBufferHeightKey,kCVPixelBufferPixelFormatTypeKey,kCVPixelBufferIOSurfacePropertiesKey};
        const void *values[]={wn,hn,fn,io};
        CFDictionaryRef attributes=CFDictionaryCreate(nullptr,keys,values,4,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        auto result=CVPixelBufferPoolCreate(nullptr,nullptr,attributes,&pool);
        CFRelease(attributes);CFRelease(io);CFRelease(wn);CFRelease(hn);CFRelease(fn);
        poolWidth=width;poolHeight=height;poolFullRange=fullRange;return result==kCVReturnSuccess;
    }
    CVPixelBufferRef copy(const vpx_image_t &image){
        if(image.fmt!=VPX_IMG_FMT_I420||image.bit_depth!=8||image.x_chroma_shift!=1||image.y_chroma_shift!=1||!dimensions(image.d_w,image.d_h))return nullptr;
        if(!preparePool(image.d_w,image.d_h,image.range==VPX_CR_FULL_RANGE))return nullptr;
        int32_t limit=kPoolLimit;
        CFNumberRef number=CFNumberCreate(nullptr,kCFNumberSInt32Type,&limit);
        const void *keys[]={kCVPixelBufferPoolAllocationThresholdKey},*values[]={number};
        CFDictionaryRef auxiliary=CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        CVPixelBufferRef pixels=nullptr;auto result=CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr,pool,auxiliary,&pixels);
        CFRelease(auxiliary);CFRelease(number);
        if(result!=kCVReturnSuccess)return nullptr;
        if(CVPixelBufferLockBaseAddress(pixels,0)!=kCVReturnSuccess){CVPixelBufferRelease(pixels);return nullptr;}
        auto *luma=static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels,0));
        auto *chroma=static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels,1));
        const size_t yStride=CVPixelBufferGetBytesPerRowOfPlane(pixels,0),uvStride=CVPixelBufferGetBytesPerRowOfPlane(pixels,1);
        // Zero row padding because byte-buffer clients receive native strides.
        std::memset(luma,0,yStride*CVPixelBufferGetHeightOfPlane(pixels,0));
        std::memset(chroma,128,uvStride*CVPixelBufferGetHeightOfPlane(pixels,1));
        for(unsigned y=0;y<image.d_h;y++)std::memcpy(luma+y*yStride,image.planes[0]+y*image.stride[0],image.d_w);
        for(unsigned y=0;y<(image.d_h+1)/2;y++){
            auto *uv=chroma+y*uvStride;auto *u=image.planes[1]+y*image.stride[1],*v=image.planes[2]+y*image.stride[2];
            for(unsigned x=0;x<(image.d_w+1)/2;x++){uv[2*x]=u[x];uv[2*x+1]=v[x];}
        }
        CVPixelBufferUnlockBaseAddress(pixels,0);
        CFStringRef matrix=nullptr,primaries=nullptr,transfer=nullptr;
        switch(image.cs){
            case VPX_CS_BT_709:matrix=kCVImageBufferYCbCrMatrix_ITU_R_709_2;primaries=kCVImageBufferColorPrimaries_ITU_R_709_2;transfer=kCVImageBufferTransferFunction_ITU_R_709_2;break;
            case VPX_CS_BT_601:case VPX_CS_SMPTE_170:matrix=kCVImageBufferYCbCrMatrix_ITU_R_601_4;primaries=kCVImageBufferColorPrimaries_SMPTE_C;transfer=kCVImageBufferTransferFunction_ITU_R_709_2;break;
            case VPX_CS_SMPTE_240:matrix=kCVImageBufferYCbCrMatrix_SMPTE_240M_1995;primaries=kCVImageBufferColorPrimaries_SMPTE_C;transfer=kCVImageBufferTransferFunction_SMPTE_240M_1995;break;
            case VPX_CS_BT_2020:matrix=kCVImageBufferYCbCrMatrix_ITU_R_2020;primaries=kCVImageBufferColorPrimaries_ITU_R_2020;transfer=kCVImageBufferTransferFunction_ITU_R_2020;break;
            default:break;
        }
        if(matrix)CVBufferSetAttachment(pixels,kCVImageBufferYCbCrMatrixKey,matrix,kCVAttachmentMode_ShouldPropagate);
        if(primaries)CVBufferSetAttachment(pixels,kCVImageBufferColorPrimariesKey,primaries,kCVAttachmentMode_ShouldPropagate);
        if(transfer)CVBufferSetAttachment(pixels,kCVImageBufferTransferFunctionKey,transfer,kCVAttachmentMode_ShouldPropagate);
        return pixels;
    }
    int decode(const uint8_t *data,size_t size,int64_t pts){
        if(closed||error||!initialized)return -1;
        if(size>std::numeric_limits<unsigned>::max()||frames.size()>=kOutputLimit)return error=-1;
        if(size){
            vpx_codec_stream_info_t stream={};stream.sz=sizeof stream;
            // Original libvpx parses key/intra dimensions before allocation.
            // Inter frames reuse its verified reference dimensions.
            vpx_codec_peek_stream_info(interface,data,static_cast<unsigned>(size),&stream);
            if((stream.w||stream.h)&&!dimensions(stream.w,stream.h))return error=-1;
            // The VP9 frame marker/profile prefix is present in every frame.
            // Only profile 0 (8-bit 4:2:0) belongs to this native output path.
            if(vp9&&((data[0]&0xc0)!=0x80||(data[0]&0x30)))return error=-1;
        }
        static_assert(sizeof(uintptr_t)==sizeof(int64_t));
        void *timestamp=reinterpret_cast<void*>(std::bit_cast<uintptr_t>(pts));
        if(vpx_codec_decode(&codec,data,static_cast<unsigned>(size),timestamp,0)!=VPX_CODEC_OK)return error=-1;
        vpx_codec_iter_t iterator=nullptr;
        while(auto *image=vpx_codec_get_frame(&codec,&iterator)){
            if(frames.size()>=kOutputLimit)return error=-1;
            CVPixelBufferRef pixels=copy(*image);if(!pixels)return error=-1;
            int64_t time=std::bit_cast<int64_t>(reinterpret_cast<uintptr_t>(image->user_priv));
            try{frames.emplace_back(pixels,time);}catch(...){CVPixelBufferRelease(pixels);return error=-1;}
            // Hidden reference packets do not consume output leases. Count
            // displayed frames, matching the existing owner's release count.
            count++;
        }
        return 0;
    }
public:
    explicit VpxDecoder(bool isVp9):interface(isVp9?vpx_codec_vp9_dx():vpx_codec_vp8_dx()),vp9(isVp9){initialize();}
    ~VpxDecoder()override{close();if(pool)CVPixelBufferPoolRelease(pool);}
    bool valid()const{return initialized;}
    // VP8/VP9 packets carry their decode configuration. Matroska CodecPrivate
    // metadata is not an encoded frame; libvpx validates actual packet headers.
    int configuration(const uint8_t*,size_t)override{return 0;}
    int submit(const uint8_t *data,size_t size,int64_t pts)override{
        int result;{std::lock_guard lock(mutex);result=(!data||!size)?-1:decode(data,size,pts);}signal();return result;
    }
    CVPixelBufferRef pull(int64_t *pts)override{
        std::lock_guard lock(mutex);if(frames.empty())return nullptr;
        auto &frame=frames.front();if(pts)*pts=frame.pts;auto pixels=std::exchange(frame.pixels,nullptr);frames.pop_front();return pixels;
    }
    int status()override{std::lock_guard lock(mutex);return error;}
    unsigned submitted()override{std::lock_guard lock(mutex);return count;}
    int drain()override{int result;{std::lock_guard lock(mutex);result=decode(nullptr,0,0);}signal();return result;}
    void flush()override{
        {std::lock_guard lock(mutex);if(closed)return;frames.clear();if(initialized)vpx_codec_destroy(&codec);initialized=false;codec={};error=initialize()?0:-1;}signal();
    }
    void close()override{
        std::unique_lock lock(mutex);if(closed)return;closed=true;notify=nullptr;context=nullptr;
        finished.wait(lock,[&]{return notifying==0;});frames.clear();if(initialized)vpx_codec_destroy(&codec);initialized=false;
    }
    void callback(void(*fn)(void*),void *opaque)override{
        std::unique_lock lock(mutex);notify=nullptr;context=nullptr;finished.wait(lock,[&]{return notifying==0;});
        if(!closed){notify=fn;context=opaque;}
    }
};
}
std::shared_ptr<KLJVideoDecoder> klj_vpx_decoder(const char *mime){
    if(!mime)return {};
    const bool vp9=!strcmp(mime,"video/x-vnd.on2.vp9");
    if(!vp9&&strcmp(mime,"video/x-vnd.on2.vp8"))return {};
    auto decoder=std::make_shared<VpxDecoder>(vp9);return decoder->valid()?decoder:nullptr;
}
