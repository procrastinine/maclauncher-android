#include "pixel-copy.hpp"
#include <dispatch/dispatch.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace klj_copy {
namespace {
constexpr size_t maxRequests=8,maxBytes=128u*1024u*1024u;
std::mutex registryMutex;
std::unordered_map<uint64_t,std::shared_ptr<Request>> registry;
uint64_t nextId=1;
size_t allocatedBytes=0;
bool timerScheduled=false;
void armTimerLocked();
struct Environment {
    JavaVM *vm;JNIEnv *env=nullptr;bool attached=false;
    explicit Environment(JavaVM *value):vm(value){if(vm&&vm->GetEnv((void**)&env,JNI_VERSION_1_8)==JNI_EDETACHED)attached=vm->AttachCurrentThreadAsDaemon((void**)&env,nullptr)==JNI_OK;}
    ~Environment(){if(attached)vm->DetachCurrentThread();}
};
const char *shader=R"METAL(
#include <metal_stdlib>
using namespace metal;
kernel void copyPixels(texture2d<float,access::sample> source [[texture(0)]],
 device uint *output [[buffer(0)]],constant uint3 &extent [[buffer(1)]],constant float4 &region [[buffer(2)]],uint2 p [[thread_position_in_grid]]) {
 if(p.x>=extent.x||p.y>=extent.y)return;
 float2 uv=region.xy+(float2(p)+0.5f)/float2(extent.xy)*region.zw;if(extent.z)uv.y=1.0f-uv.y;
 constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);
 uint4 c=uint4(round(clamp(source.sample(s,uv),0.0f,1.0f)*255.0f));
 output[p.y*extent.x+p.x]=(c.a<<24)|(c.r<<16)|(c.g<<8)|c.b;
}
)METAL";
}
struct Request {
    std::mutex mutex;
    uint64_t token=0;int width=0,height=0;
    int left=0,top=0,right=0,bottom=0;
    JavaVM *vm=nullptr;jobject callback=nullptr;jmethodID method=nullptr;
    std::atomic<bool> done{false};
    std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    size_t bytes=0;
    ~Request(){if(callback){Environment thread(vm);if(thread.env)thread.env->DeleteGlobalRef(callback);}if(bytes){std::lock_guard lock(registryMutex);allocatedBytes-=bytes;}}
    void finish(int status,id<MTLBuffer> pixels=nil,bool notify=true){
        jobject target;{std::lock_guard lock(mutex);if(done.exchange(true))return;target=callback;callback=nullptr;}
        {std::lock_guard lock(registryMutex);registry.erase(token);}
        Environment thread(vm);auto env=thread.env;if(!env)return;
        jthrowable prior=env->ExceptionOccurred();if(prior)env->ExceptionClear();
        if(notify&&target){
            jintArray data=nullptr;
            if(status==Success){data=env->NewIntArray(width*height);if(data)env->SetIntArrayRegion(data,0,width*height,(const jint*)pixels.contents);}
            if(!env->ExceptionCheck())env->CallVoidMethod(target,method,status,data);
            // Owned completion only queues work; listener exceptions run on its
            // Handler. JNI allocation/queue failures remain visible diagnostics.
            if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();}
            if(data)env->DeleteLocalRef(data);
        }
        if(target)env->DeleteGlobalRef(target);
        if(prior){env->Throw(prior);env->DeleteLocalRef(prior);}
    }
};
namespace {
void armTimerLocked(){
    if(timerScheduled||registry.empty())return;
    auto earliest=registry.begin()->second->deadline;
    for(auto&entry:registry)earliest=std::min(earliest,entry.second->deadline);
    auto delay=std::chrono::duration_cast<std::chrono::nanoseconds>(earliest-std::chrono::steady_clock::now()).count();
    timerScheduled=true;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,std::max<int64_t>(1,delay)),dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        std::vector<std::shared_ptr<Request>> expired;
        {std::lock_guard lock(registryMutex);timerScheduled=false;auto now=std::chrono::steady_clock::now();for(auto&entry:registry)if(entry.second->deadline<=now)expired.push_back(entry.second);}
        for(auto&request:expired)request->finish(Timeout);
        std::lock_guard lock(registryMutex);armTimerLocked();
    });
}
}
uint64_t Queue::add(JNIEnv *env,jobject callback,int width,int height,int immediateStatus,int left,int top,int right,int bottom){
    auto request=std::make_shared<Request>();request->width=width;request->height=height;
    request->left=left;request->top=top;request->right=right;request->bottom=bottom;
    if(env->GetJavaVM(&request->vm)!=JNI_OK)return 0;
    auto type=env->GetObjectClass(callback);if(!type)return 0;
    request->method=env->GetMethodID(type,"complete","(I[I)V");env->DeleteLocalRef(type);if(!request->method)return 0;
    request->callback=env->NewGlobalRef(callback);if(!request->callback)return 0;
    {std::lock_guard lock(registryMutex);request->token=nextId++;if(registry.size()>=maxRequests)immediateStatus=Timeout;else if(immediateStatus<0){registry.emplace(request->token,request);armTimerLocked();}}
    if(immediateStatus>=0){request->finish(immediateStatus);return 0;}
    pending.erase(std::remove_if(pending.begin(),pending.end(),[](auto&w){auto r=w.lock();return !r||r->done.load();}),pending.end());
    pending.push_back(request);
    // At most one process-wide timeout callback, idle when requests are absent.
    return request->token;
}
bool Queue::waiting(){pending.erase(std::remove_if(pending.begin(),pending.end(),[](auto&w){auto r=w.lock();return !r||r->done.load();}),pending.end());return !pending.empty();}
void Queue::encode(id<MTLCommandBuffer> commands,id<MTLTexture> source,bool bottomLeft){
    if(!waiting())return;
    if(!commands||!source||source.device!=commands.device){retire(SourceInvalid);return;}
    if(!pipeline){NSError *error=nil;auto library=[commands.device newLibraryWithSource:@(shader) options:nil error:&error];if(library)pipeline=[commands.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"copyPixels"] error:&error];if(!pipeline){retire(Unknown);return;}}
    auto batch=std::move(pending);pending.clear();
    for(auto &weak:batch){auto request=weak.lock();if(!request||request->done.load())continue;
        int left=std::clamp(request->left,0,(int)source.width),top=std::clamp(request->top,0,(int)source.height);
        int right=request->right?std::clamp(request->right,0,(int)source.width):(int)source.width,bottom=request->bottom?std::clamp(request->bottom,0,(int)source.height):(int)source.height;
        if(right<=left||bottom<=top){request->finish(SourceInvalid);continue;}
        size_t size=size_t(request->width)*request->height*4;
        size_t charge=[commands.device heapBufferSizeAndAlignWithLength:size options:MTLResourceStorageModeShared].size;
        bool affordable;{std::lock_guard lock(registryMutex);affordable=charge&&charge<=maxBytes-allocatedBytes;if(affordable){allocatedBytes+=charge;request->bytes=charge;}}
        if(!affordable){request->finish(DestinationInvalid);continue;}
        auto buffer=[commands.device newBufferWithLength:size options:MTLResourceStorageModeShared];
        if(!buffer){request->finish(DestinationInvalid);continue;}
        {std::lock_guard lock(registryMutex);size_t actual=buffer.allocatedSize;affordable=actual<=maxBytes-(allocatedBytes-request->bytes);if(affordable){allocatedBytes=allocatedBytes-request->bytes+actual;request->bytes=actual;}}
        if(!affordable){request->finish(DestinationInvalid);continue;}
        auto encoder=[commands computeCommandEncoder];if(!encoder){request->finish(Unknown);continue;}
        uint32_t extent[4]={uint32_t(request->width),uint32_t(request->height),bottomLeft?1u:0u,0};
        float region[4]={float(left)/source.width,float(top)/source.height,float(right-left)/source.width,float(bottom-top)/source.height};
        [encoder setComputePipelineState:pipeline];[encoder setTexture:source atIndex:0];[encoder setBuffer:buffer offset:0 atIndex:0];[encoder setBytes:extent length:sizeof(extent) atIndex:1];[encoder setBytes:region length:sizeof(region) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(request->width,request->height,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];[encoder endEncoding];
        [commands addCompletedHandler:^(id<MTLCommandBuffer> completed){request->finish(completed.status==MTLCommandBufferStatusCompleted?Success:Unknown,buffer);}];
    }
}
void Queue::retire(int status){auto batch=std::move(pending);pending.clear();for(auto &weak:batch)if(auto request=weak.lock())request->finish(status);}
Queue::~Queue(){retire();}
void cancel(uint64_t id){std::shared_ptr<Request> request;{std::lock_guard lock(registryMutex);auto found=registry.find(id);if(found!=registry.end())request=found->second;}if(request)request->finish(SourceInvalid,nil,false);}
}
