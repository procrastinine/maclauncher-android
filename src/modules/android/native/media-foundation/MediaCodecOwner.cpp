#include "MediaCodecOwner.h"
#include "MediaCodecCatalog.h"
#include "VideoDecoder.h"
#include <media/NdkMediaError.h>
#include <media/NdkMediaCodec.h>
extern "C" {
void kl_thread_init(void);
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <limits>
#include <utility>

namespace {
using Clock=std::chrono::steady_clock;
constexpr int kInvalid=AMEDIA_ERROR_INVALID_PARAMETER,kState=AMEDIA_ERROR_INVALID_OPERATION,kError=AMEDIA_ERROR_UNKNOWN;
struct FormatDeleter {void operator()(AMediaFormat *value)const{if(value)AMediaFormat_delete(value);}};
using Format=std::unique_ptr<AMediaFormat,FormatDeleter>;
Format copyFormat(const AMediaFormat *value){Format out(AMediaFormat_new());if(out&&value&&AMediaFormat_copy(out.get(),const_cast<AMediaFormat*>(value))!=AMEDIA_OK)return {};return out;}
using Decoder=KLJVideoDecoder;
struct Sink {KLJCodecSurface value;explicit Sink(const KLJCodecSurface&v):value(v){if(value.retain)value.retain(value.context);}~Sink(){if(value.release)value.release(value.context);}};
struct Listener {KLJCodecCallbacks value;explicit Listener(const KLJCodecCallbacks&v):value(v){if(value.retain)value.retain(value.context);}~Listener(){if(value.release)value.release(value.context);}};
struct RenderedListener {KLJCodecRenderedCallbacks value;explicit RenderedListener(const KLJCodecRenderedCallbacks&v):value(v){if(value.retain)value.retain(value.context);}~RenderedListener(){if(value.release)value.release(value.context);}};
struct Input {std::vector<uint8_t> bytes;bool leased=false;};
struct Layout {
    size_t size=0,stride=0;int color=0;
    explicit Layout(CVPixelBufferRef pixels){
        const auto format=CVPixelBufferGetPixelFormatType(pixels);
        if(CVPixelBufferIsPlanar(pixels)){
            const bool wide=format==kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange||format==kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
            color=wide?54:21;
            stride=CVPixelBufferGetBytesPerRowOfPlane(pixels,0)/(wide?2:1);
            for(size_t plane=0;plane<CVPixelBufferGetPlaneCount(pixels);plane++)size+=CVPixelBufferGetBytesPerRowOfPlane(pixels,plane)*CVPixelBufferGetHeightOfPlane(pixels,plane);
        }else{color=16;stride=CVPixelBufferGetBytesPerRow(pixels)/4;size=CVPixelBufferGetBytesPerRow(pixels)*CVPixelBufferGetHeight(pixels);}
    }
};
struct Output {
    CVPixelBufferRef pixels=nullptr;std::vector<uint8_t> linear;KLJCodecBufferInfo info={};bool leased=false,copied=false,locked=false;
    Output()=default;Output(const Output&)=delete;Output&operator=(const Output&)=delete;
    Output(Output&&other)noexcept:pixels(std::exchange(other.pixels,nullptr)),linear(std::move(other.linear)),info(other.info),leased(other.leased),copied(other.copied),locked(std::exchange(other.locked,false)){}
    ~Output(){clear();}
    void clear(){if(pixels){if(locked)CVPixelBufferUnlockBaseAddress(pixels,kCVPixelBufferLock_ReadOnly);CVPixelBufferRelease(pixels);}pixels=nullptr;leased=false;copied=false;locked=false;info={};}
    uint8_t* map(){
        if(!pixels)return nullptr;
        if(!locked){if(CVPixelBufferLockBaseAddress(pixels,kCVPixelBufferLock_ReadOnly)!=kCVReturnSuccess)return nullptr;locked=true;}
        if(!copied){size_t bytes=Layout(pixels).size;if(linear.size()!=bytes)linear.resize(bytes);
            if(CVPixelBufferIsPlanar(pixels)){size_t offset=0;for(size_t plane=0;plane<CVPixelBufferGetPlaneCount(pixels);plane++){size_t length=CVPixelBufferGetBytesPerRowOfPlane(pixels,plane)*CVPixelBufferGetHeightOfPlane(pixels,plane);std::memcpy(linear.data()+offset,CVPixelBufferGetBaseAddressOfPlane(pixels,plane),length);offset+=length;}}
            else std::memcpy(linear.data(),CVPixelBufferGetBaseAddress(pixels),bytes);copied=true;
        }
        return linear.data();
    }
};
}

struct KLJCodec {
    std::atomic<unsigned> references{1};
    const std::string mime,name;
    std::mutex operations,mutex;
    std::condition_variable changed;
    enum State {Initial,Configured,Started,Closed} state=Initial;
    uint64_t generation=0,signal=0,renderedGeneration=0;
    std::shared_ptr<Decoder> decoder;
    std::shared_ptr<Sink> sink;
    std::shared_ptr<Listener> listener;
    std::shared_ptr<RenderedListener> rendered;
    struct RenderedEvent {uint64_t generation;int64_t media,system;};
    std::deque<RenderedEvent> renderedEvents;
    Format input,output;
    std::vector<Input> inputs;
    std::vector<Output> outputs;
    CVPixelBufferRef pending=nullptr;
    int64_t pendingTime=0,eosTime=0;
    bool inputEos=false,outputEos=false,formatPending=false,buffersPending=false,worker=false,inCallback=false,legacyOutput=false,submitting=false;
    std::thread::id workerId;
    size_t inputCapacity=0,inFlight=0;
    int error=0,width=0,height=0;OSType pixelFormat=0;
    KLJCodec(const char *type,const char *codec):mime(type),name(codec){}
    ~KLJCodec(){if(pending)CVPixelBufferRelease(pending);if(decoder)decoder->close();}
    void wake(){++signal;changed.notify_all();}
    static void notify(void *opaque){auto self=static_cast<KLJCodec*>(opaque);std::lock_guard lock(self->mutex);self->wake();}
    void clearBuffers(){for(auto&value:inputs)value.leased=false;for(auto&value:outputs)value.clear();if(pending)CVPixelBufferRelease(pending);pending=nullptr;inputEos=outputEos=false;inFlight=0;error=0;width=height=0;pixelFormat=0;formatPending=buffersPending=false;renderedEvents.clear();}
    bool canInput()const{
        // Backpressure belongs to the active buffer pool, never a frame-drop
        // ring. Reserve decoder reference pictures plus input slots.
        return state==Started&&!inputEos&&!error&&inFlight<inputs.size()+16;
    }
    void fillFormat(CVPixelBufferRef pixels){
        width=(int)CVPixelBufferGetWidth(pixels);height=(int)CVPixelBufferGetHeight(pixels);pixelFormat=CVPixelBufferGetPixelFormatType(pixels);output=copyFormat(input.get());if(!output)throw std::bad_alloc();
        AMediaFormat_setString(output.get(),AMEDIAFORMAT_KEY_MIME,"video/raw");
        AMediaFormat_setInt32(output.get(),AMEDIAFORMAT_KEY_WIDTH,width);AMediaFormat_setInt32(output.get(),AMEDIAFORMAT_KEY_HEIGHT,height);
        Layout layout(pixels);AMediaFormat_setInt32(output.get(),AMEDIAFORMAT_KEY_STRIDE,(int32_t)layout.stride);AMediaFormat_setInt32(output.get(),AMEDIAFORMAT_KEY_SLICE_HEIGHT,height);
        AMediaFormat_setInt32(output.get(),AMEDIAFORMAT_KEY_COLOR_FORMAT,layout.color);
        if(!sink){if(layout.size>INT32_MAX)throw std::bad_alloc();for(auto&buffer:outputs)if(!buffer.leased&&buffer.linear.size()!=layout.size){buffer.linear.resize(layout.size);buffersPending=true;}}
        formatPending=true;
        AMediaFormat_setInt32(output.get(),"crop-left",0);AMediaFormat_setInt32(output.get(),"crop-top",0);AMediaFormat_setInt32(output.get(),"crop-right",width-1);AMediaFormat_setInt32(output.get(),"crop-bottom",height-1);
    }
    ssize_t takeInput(){if(!canInput())return AMEDIACODEC_INFO_TRY_AGAIN_LATER;for(size_t i=0;i<inputs.size();i++)if(!inputs[i].leased){inputs[i].leased=true;return (ssize_t)i;}return AMEDIACODEC_INFO_TRY_AGAIN_LATER;}
    ssize_t takeOutput(KLJCodecBufferInfo *info){
        if(state!=Started)return kState;if(error)return error;
        // A synchronous software decoder can publish before queue_inputs has
        // recorded its output leases. Wake consumers after that accounting,
        // so a fast callback cannot release an as-yet uncounted frame.
        if(submitting)return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        if(!pending){pending=decoder->pull(&pendingTime);if(!pending){int failed=decoder->status();if(failed){error=kError;return error;}}}
        if(pending&&((int)CVPixelBufferGetWidth(pending)!=width||(int)CVPixelBufferGetHeight(pending)!=height||CVPixelBufferGetPixelFormatType(pending)!=pixelFormat))fillFormat(pending);
        if(buffersPending){buffersPending=false;return AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED;}
        if(formatPending){formatPending=false;return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;}
        if(!pending&&(!inputEos||outputEos))return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        size_t index=0;while(index<outputs.size()&&outputs[index].leased)index++;
        if(index==outputs.size())return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
        auto&out=outputs[index];out.clear();out.leased=true;out.pixels=std::exchange(pending,nullptr);
        out.info.presentationTimeUs=out.pixels?pendingTime:eosTime;out.info.flags=out.pixels?0:AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
        if(!out.pixels)outputEos=true;
        if(out.pixels&&!sink){size_t bytes=Layout(out.pixels).size;if(bytes>INT32_MAX){out.clear();return kError;}out.info.size=(int32_t)bytes;}
        if(legacyOutput&&out.pixels&&!sink&&!out.map()){out.clear();return kError;}
        if(info)*info=out.info;return (ssize_t)index;
    }
    void dispatch();
};

extern "C" KLJCodec *klj_codec_create(const char *name,bool isType,bool encoder){
    if(encoder)return nullptr;
    const auto *descriptor=klj_codec_descriptor(name,isType);if(!descriptor)return nullptr;
    try{return new KLJCodec(descriptor->mime,descriptor->name);}catch(...){return nullptr;}
}
extern "C" void klj_codec_retain(KLJCodec*value){if(value)value->references.fetch_add(1,std::memory_order_relaxed);}
extern "C" void klj_codec_release(KLJCodec*value){if(value&&value->references.fetch_sub(1,std::memory_order_acq_rel)==1)delete value;}
static int retire(KLJCodec*value,bool closing){
    if(!value)return kInvalid;std::unique_lock operation(value->operations);std::shared_ptr<Decoder> decoder;std::shared_ptr<Sink> sink;std::shared_ptr<Listener> listener;
    {std::lock_guard lock(value->mutex);if(value->state==KLJCodec::Closed)return closing?0:kState;value->state=closing?KLJCodec::Closed:KLJCodec::Initial;value->generation++;decoder=std::move(value->decoder);sink=std::move(value->sink);if(closing)listener=std::move(value->listener);value->rendered.reset();value->wake();}
    if(decoder)decoder->close();
    {std::lock_guard lock(value->mutex);value->clearBuffers();value->input.reset();value->output.reset();value->inputs.clear();value->outputs.clear();value->wake();}
    operation.unlock();decoder.reset();sink.reset();listener.reset();
    if(closing){std::unique_lock lock(value->mutex);if(value->workerId!=std::this_thread::get_id())value->changed.wait(lock,[&]{return !value->worker;});}
    return 0;
}
extern "C" int klj_codec_close(KLJCodec*v){return retire(v,true);}
extern "C" int klj_codec_reset(KLJCodec*v){return retire(v,false);}
extern "C" int klj_codec_stop(KLJCodec*v){return retire(v,false);}
extern "C" int klj_codec_configure(KLJCodec*v,const AMediaFormat*sourceFormat,const KLJCodecSurface*surface,unsigned flags){
    auto *format=const_cast<AMediaFormat*>(sourceFormat);if(!v||!format)return kInvalid;if(flags&AMEDIACODEC_CONFIGURE_FLAG_ENCODE)return AMEDIA_ERROR_UNSUPPORTED;
    std::lock_guard operation(v->operations);
    {std::lock_guard lock(v->mutex);if(v->state!=KLJCodec::Initial)return kState;}
    const char *mime=nullptr;if(AMediaFormat_getString(format,AMEDIAFORMAT_KEY_MIME,&mime)&&v->mime!=mime)return kInvalid;
    int32_t maxSize=0,w=0,h=0;AMediaFormat_getInt32(format,AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,&maxSize);AMediaFormat_getInt32(format,AMEDIAFORMAT_KEY_WIDTH,&w);AMediaFormat_getInt32(format,AMEDIAFORMAT_KEY_HEIGHT,&h);
    if(maxSize<0||w<0||h<0)return kInvalid;
    size_t capacity=maxSize>0?(size_t)maxSize:w>0&&h>0?size_t(w)*h*3/2:2u<<20;if(!capacity||capacity>INT32_MAX)return kInvalid;
    auto next=klj_video_decoder(v->mime.c_str());if(!next)return AMEDIA_ERROR_UNSUPPORTED;
    if(v->mime=="video/x-vnd.on2.vp8"||v->mime=="video/x-vnd.on2.vp9"){
        int32_t profile=0;
        if((w&&static_cast<unsigned>(w)>KLJ_VPX_MAX_WIDTH)||(h&&static_cast<unsigned>(h)>KLJ_VPX_MAX_HEIGHT)||
           (AMediaFormat_getInt32(format,"profile",&profile)&&profile!=1))return AMEDIA_ERROR_UNSUPPORTED;
    }
    auto input=copyFormat(format);if(!input)return kError;
    // VideoToolbox retains original Annex-B parameter sets; VPx packets carry
    // their own configuration and do not decode container CodecPrivate bytes.
    for(int i=0;;i++){char key[32];snprintf(key,sizeof key,"csd-%d",i);void*data=nullptr;size_t size=0;if(!AMediaFormat_getBuffer(format,key,&data,&size))break;if(size&&next->configuration((const uint8_t*)data,size))return kError;}
    std::shared_ptr<Sink> output=surface?std::make_shared<Sink>(*surface):nullptr;
    next->callback(KLJCodec::notify,v);
    {std::lock_guard lock(v->mutex);v->decoder=std::move(next);v->input=std::move(input);v->output=copyFormat(v->input.get());v->sink=std::move(output);v->inputCapacity=capacity;v->inputs.resize(8);v->outputs.resize(8);v->clearBuffers();v->state=KLJCodec::Configured;v->generation++;v->wake();}
    return 0;
}
extern "C" int klj_codec_set_surface(KLJCodec*v,const KLJCodecSurface*s){if(!v)return kInvalid;auto next=s?std::make_shared<Sink>(*s):nullptr;{std::lock_guard lock(v->mutex);if(v->state!=KLJCodec::Configured&&v->state!=KLJCodec::Started)return kState;v->sink.swap(next);}return 0;}
extern "C" int klj_codec_start(KLJCodec*v){if(!v)return kInvalid;std::lock_guard lock(v->mutex);if(v->state!=KLJCodec::Configured)return kState;v->state=KLJCodec::Started;v->wake();return 0;}
extern "C" int klj_codec_flush(KLJCodec*v){if(!v)return kInvalid;std::lock_guard operation(v->operations);std::shared_ptr<Decoder> d;{std::lock_guard lock(v->mutex);if(v->state!=KLJCodec::Started)return kState;d=v->decoder;v->generation++;v->state=KLJCodec::Configured;v->wake();}d->flush();{std::lock_guard lock(v->mutex);v->clearBuffers();if(!v->listener)v->state=KLJCodec::Started;v->wake();}return 0;}
static Clock::time_point deadline(int64_t us){if(us<0)return Clock::time_point::max();auto now=Clock::now();auto remaining=std::chrono::duration_cast<std::chrono::microseconds>(Clock::time_point::max()-now).count();return us>=remaining?Clock::time_point::max():now+std::chrono::microseconds(us);}
extern "C" ssize_t klj_codec_dequeue_input(KLJCodec*v,int64_t us){if(!v)return kInvalid;auto until=deadline(us);std::unique_lock lock(v->mutex);if(v->listener)return kState;auto epoch=v->generation;for(;;){if(v->state!=KLJCodec::Started||epoch!=v->generation)return kState;if(v->error)return v->error;auto index=v->takeInput();if(index>=0||us==0)return index;if(!v->changed.wait_until(lock,until,[&]{return v->generation!=epoch||v->error||v->state!=KLJCodec::Started||(v->canInput()&&std::any_of(v->inputs.begin(),v->inputs.end(),[](auto&i){return !i.leased;}));}))return AMEDIACODEC_INFO_TRY_AGAIN_LATER;}}
extern "C" uint8_t *klj_codec_input_buffer(KLJCodec*v,size_t index,size_t*size){if(size)*size=0;if(!v)return nullptr;std::lock_guard lock(v->mutex);if(index>=v->inputs.size()||!v->inputs[index].leased)return nullptr;auto&b=v->inputs[index];if(b.bytes.empty())b.bytes.resize(v->inputCapacity);if(size)*size=b.bytes.size();return b.bytes.data();}
extern "C" int klj_codec_queue_inputs(KLJCodec*v,size_t index,const KLJCodecBufferInfo*parts,size_t count){
    if(!v||(!parts&&count))return kInvalid;
    std::lock_guard operation(v->operations);std::shared_ptr<Decoder>decoder;const uint8_t*bytes=nullptr;
    {std::lock_guard lock(v->mutex);if(v->state!=KLJCodec::Started||v->inputEos)return kState;
        if(index>=v->inputs.size()||!v->inputs[index].leased)return kInvalid;auto&buffer=v->inputs[index];
        for(size_t i=0;i<count;i++){auto&part=parts[i];if(part.offset<0||part.size<0||size_t(part.offset)>buffer.bytes.size()||size_t(part.size)>buffer.bytes.size()-size_t(part.offset)||(i+1<count&&(part.flags&AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)))return kInvalid;}
        decoder=v->decoder;bytes=buffer.bytes.data();v->submitting=true;}
    unsigned before=decoder->submitted();int result=0;
    for(size_t i=0;i<count&&!result;i++){auto&part=parts[i];if(part.size)result=decoder->submit(bytes+part.offset,part.size,part.presentationTimeUs);
        if(!result&&(part.flags&AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM))result=decoder->drain();}
    unsigned after=decoder->submitted();
    {std::lock_guard lock(v->mutex);v->inputs[index].leased=false;v->inFlight+=after-before;v->submitting=false;
        if(result)v->error=kError;else if(count&&(parts[count-1].flags&AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)){v->inputEos=true;v->eosTime=parts[count-1].presentationTimeUs;}v->wake();}
    return result?kError:0;
}
extern "C" int klj_codec_queue_input(KLJCodec*v,size_t index,off_t offset,size_t size,int64_t time,unsigned flags){
    if(offset<0||offset>INT32_MAX||size>INT32_MAX)return kInvalid;
    KLJCodecBufferInfo part={(int32_t)offset,(int32_t)size,time,flags};return klj_codec_queue_inputs(v,index,&part,1);
}
extern "C" ssize_t klj_codec_dequeue_output(KLJCodec*v,KLJCodecBufferInfo*info,int64_t us){if(!v||!info)return kInvalid;auto until=deadline(us);std::unique_lock lock(v->mutex);if(v->listener)return kState;auto epoch=v->generation;for(;;){if(epoch!=v->generation)return kState;auto index=v->takeOutput(info);if(index!=AMEDIACODEC_INFO_TRY_AGAIN_LATER||us==0)return index;auto signal=v->signal;if(!v->changed.wait_until(lock,until,[&]{return signal!=v->signal;}))return AMEDIACODEC_INFO_TRY_AGAIN_LATER;}}
extern "C" uint8_t *klj_codec_output_buffer(KLJCodec*v,size_t index,size_t*size){
    if(size)*size=0;if(!v)return nullptr;std::lock_guard lock(v->mutex);if(v->sink||index>=v->outputs.size())return nullptr;auto&b=v->outputs[index];if(!b.leased||!b.pixels)return nullptr;
    if(!b.map()){v->error=kError;v->wake();return nullptr;}
    if(size)*size=b.linear.size();return b.linear.data();
}
namespace {
struct Completion {KLJCodec*owner;uint64_t epoch,listener;int64_t media;};
void consumed(void*context,int64_t system){
    std::unique_ptr<Completion>completion(static_cast<Completion*>(context));auto*v=completion->owner;
    {std::lock_guard lock(v->mutex);if(system>0&&v->generation==completion->epoch&&v->renderedGeneration==completion->listener&&v->rendered&&v->state==KLJCodec::Started){v->renderedEvents.push_back({completion->epoch,completion->media,system});v->wake();}}
    klj_codec_release(v);
}
}
extern "C" int klj_codec_release_output(KLJCodec*v,size_t index,bool render,int64_t time){
    if(!v)return kInvalid;CVPixelBufferRef pixels=nullptr;std::shared_ptr<Sink>sink;std::unique_ptr<Completion>completion;int64_t pts=0;
    {std::lock_guard lock(v->mutex);if(index>=v->outputs.size()||!v->outputs[index].leased)return kInvalid;auto&buffer=v->outputs[index];
        if(buffer.pixels){pixels=CVPixelBufferRetain(buffer.pixels);if(v->inFlight)v->inFlight--;}
        pts=buffer.info.presentationTimeUs;sink=v->sink;
        if(render&&pixels&&v->rendered)completion=std::make_unique<Completion>(Completion{v,v->generation,v->renderedGeneration,pts});
        buffer.clear();v->wake();}
    int result=0;
    if(render&&pixels){if(!sink||!sink->value.present)result=kState;
        else{auto*context=completion.release();if(context)klj_codec_retain(v);
            result=sink->value.present(sink->value.context,pixels,pts,time,context?consumed:nullptr,context);
            if(result&&context){delete context;klj_codec_release(v);}}}
    if(pixels)CVPixelBufferRelease(pixels);return result;
}
extern "C" AMediaFormat *klj_codec_format(KLJCodec*v,bool input){if(!v)return nullptr;std::lock_guard lock(v->mutex);if(!v->input)return nullptr;return copyFormat(input?v->input.get():v->output.get()).release();}
extern "C" AMediaFormat *klj_codec_buffer_format(KLJCodec*v,size_t index){if(!v)return nullptr;std::lock_guard lock(v->mutex);if(index>=v->outputs.size()||!v->outputs[index].leased)return nullptr;return copyFormat(v->output.get()).release();}
extern "C" const char *klj_codec_name(KLJCodec*v){return v?v->name.c_str():nullptr;}
extern "C" size_t klj_codec_input_count(KLJCodec*v){if(!v)return 0;std::lock_guard lock(v->mutex);return v->inputs.size();}
extern "C" size_t klj_codec_output_count(KLJCodec*v){if(!v)return 0;std::lock_guard lock(v->mutex);return v->outputs.size();}

void KLJCodec::dispatch(){
    kl_thread_init();{std::lock_guard lock(mutex);workerId=std::this_thread::get_id();}
    for(;;){std::shared_ptr<Listener>active;std::shared_ptr<RenderedListener>frameListener;RenderedEvent renderedEvent={};ssize_t index=-1;KLJCodecBufferInfo info={};Format format;int event=0,failure=0;uint64_t epoch=0;
        {std::unique_lock lock(mutex);for(;;){
            if(state==Closed){worker=false;workerId={};changed.notify_all();return;}
            if(rendered&&!renderedEvents.empty()){frameListener=rendered;renderedEvent=renderedEvents.front();renderedEvents.pop_front();event=5;inCallback=true;break;}
            active=listener;if(active&&state==Started){epoch=generation;
                if(error){failure=error;event=4;error=0;}
                else{index=takeOutput(&info);if(index==AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)continue;
                    if(index==AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED){format=copyFormat(output.get());event=3;}
                    else if(index>=0)event=2;
                    else if(index==AMEDIACODEC_INFO_TRY_AGAIN_LATER){index=takeInput();if(index>=0)event=1;}
                    else{failure=(int)index;event=4;}}
                if(event){inCallback=true;break;}}
            auto previous=signal;changed.wait(lock,[&]{return state==Closed||signal!=previous;});}}
        if(event==5){auto&callback=frameListener->value;callback.rendered(callback.context,renderedEvent.media,renderedEvent.system);}
        else{auto&cb=active->value;if(event==1&&cb.input)cb.input(cb.context,(size_t)index);else if(event==2&&cb.output)cb.output(cb.context,(size_t)index,&info);else if(event==3&&cb.format)cb.format(cb.context,format.get());else if(event==4&&cb.error)cb.error(cb.context,failure);}
        {std::lock_guard lock(mutex);inCallback=false;if(event==4&&generation==epoch&&state==Started)state=Configured;changed.notify_all();}
    }
}
static int startWorker(KLJCodec*v,bool spawn){
    if(!spawn)return 0;try{std::thread([v]{v->dispatch();klj_codec_release(v);}).detach();}
    catch(...){std::lock_guard lock(v->mutex);v->worker=false;v->listener.reset();v->rendered.reset();v->wake();klj_codec_release(v);return kError;}return 0;
}
extern "C" int klj_codec_callbacks(KLJCodec*v,const KLJCodecCallbacks*callbacks){
    if(!v)return kInvalid;auto next=callbacks?std::make_shared<Listener>(*callbacks):nullptr;bool spawn=false;
    {std::unique_lock lock(v->mutex);if(v->state==KLJCodec::Closed)return kState;
        // Wait before installing the replacement so the worker cannot begin a
        // new callback against it while the old registration is retiring.
        auto previous=std::move(v->listener);if(v->workerId!=std::this_thread::get_id())v->changed.wait(lock,[&]{return !v->inCallback;});
        v->listener=std::move(next);v->wake();if(callbacks&&!v->worker){v->worker=true;spawn=true;klj_codec_retain(v);}}
    return startWorker(v,spawn);
}
extern "C" int klj_codec_rendered_callbacks(KLJCodec*v,const KLJCodecRenderedCallbacks*callbacks){
    if(!v)return kInvalid;auto next=callbacks?std::make_shared<RenderedListener>(*callbacks):nullptr;bool spawn=false;
    {std::unique_lock lock(v->mutex);if(v->state==KLJCodec::Closed)return kState;
        auto previous=std::move(v->rendered);v->renderedGeneration++;v->renderedEvents.clear();
        if(v->workerId!=std::this_thread::get_id())v->changed.wait(lock,[&]{return !v->inCallback;});
        v->rendered=std::move(next);v->wake();if(callbacks&&!v->worker){v->worker=true;spawn=true;klj_codec_retain(v);}}
    return startWorker(v,spawn);
}

extern "C" int klj_codec_parameters(KLJCodec*v,const AMediaFormat*parameters){
    if(!v||!parameters)return kInvalid;std::lock_guard lock(v->mutex);if(v->state==KLJCodec::Closed)return kState;
    // Original MediaCodec permits unrecognized optional parameter hints to be
    // ignored. Decoder buffers/timestamps already determine actual output;
    // encoder-only bitrate/sync requests do not invent an encoder capability.
    return 0;
}
extern "C" uint8_t *klj_codec_array_buffer(KLJCodec*v,bool input,size_t index,size_t*size){
    if(size)*size=0;if(!v)return nullptr;std::lock_guard lock(v->mutex);
    if(v->state!=KLJCodec::Started&&v->state!=KLJCodec::Configured)return nullptr;
    if(input){if(index>=v->inputs.size())return nullptr;auto&buffer=v->inputs[index].bytes;if(buffer.empty())buffer.resize(v->inputCapacity);if(size)*size=buffer.size();return buffer.data();}
    if(v->sink||index>=v->outputs.size())return nullptr;v->legacyOutput=true;auto&output=v->outputs[index];if(output.leased&&output.pixels)output.map();auto&buffer=output.linear;
    if(size)*size=buffer.size();return buffer.empty()?nullptr:buffer.data();
}
