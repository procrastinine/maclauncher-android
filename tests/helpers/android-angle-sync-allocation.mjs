import fs from 'node:fs';
import path from 'node:path';
import {execFileSync,spawnSync} from 'node:child_process';
import {pathToFileURL} from 'node:url';
const [sourceRoot, adapterFile, outputRoot] = process.argv.slice(2);
if (!sourceRoot || !adapterFile || !outputRoot) throw Error('source, adapter and output paths required');
const adapter = await import(pathToFileURL(path.resolve(adapterFile)));
const directory=path.join(sourceRoot,'src/libANGLE/renderer/metal');
const read=name=>fs.readFileSync(path.join(directory,name),'utf8');
const original={sync:read('SyncMtl.mm'),context:read('ContextMtl.mm'),queue:read('mtl_command_buffer.mm')};
function method(text, marker) {
 const start=text.indexOf(marker); if(start<0)throw Error('missing source method '+marker);
 const open=text.indexOf('{',start);let depth=1,end=open+1;
 while(depth&&end<text.length){if(text[end]==='{')depth++;if(text[end]==='}')depth--;end++;}
 if(depth)throw Error('unterminated source method');return text.slice(start,end);
}
const support=`#import <Foundation/Foundation.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#define ANGLE_MTL_OBJC_SCOPE if (true)
#define ANGLE_MTL_LOG(...) ((void)0)
#define ASSERT(x) ((void)0)
@protocol MTLEvent @end
@protocol MTLSharedEvent <MTLEvent> @end
@protocol MTLCommandBuffer
- (void)addCompletedHandler:(void (^)(id<MTLCommandBuffer>))handler;
@end
@interface CpuBuffer : NSObject <MTLCommandBuffer> @end
@implementation CpuBuffer
- (void)addCompletedHandler:(void (^)(id<MTLCommandBuffer>))handler {}
@end
@interface CpuEvent : NSObject <MTLSharedEvent> @end
@implementation CpuEvent @end
@interface CpuQueue : NSObject
@property bool failAllocation;
- (id<MTLCommandBuffer>)commandBuffer;
@end
@implementation CpuQueue
- (id<MTLCommandBuffer>)commandBuffer { return _failAllocation ? nil : [CpuBuffer new]; }
@end
namespace angle { enum class Result { Continue, Stop }; template<class T> struct ObjCPtr {
 T value=nil; ObjCPtr()=default; ObjCPtr(T v):value(v){} T get()const{return value;}
 operator T()const{return value;} explicit operator bool()const{return value!=nil;}
 ObjCPtr& operator=(T v){value=v;return *this;}
}; }
namespace maclauncher { inline void observeNativeFenceCompletion(id<MTLCommandBuffer>, id<MTLEvent>,uint64_t) {} }
const int MTLCommandBufferErrorOutOfMemory=8;
namespace rx { namespace mtl {
constexpr int NoWait=0;
struct CommandQueue {
 CpuQueue* queue; explicit CommandQueue(CpuQueue*q):queue(q){} CpuQueue*get(){return queue;}
 std::mutex mLock; uint64_t mQueueSerialCounter=1,mActiveTimeElapsedId=0;
 std::atomic<int>mCmdBufferError{0};
 struct Entry{angle::ObjCPtr<id<MTLCommandBuffer>> buffer;uint64_t serial;};
 std::vector<Entry>mMetalCmdBuffers;
 void addCommandBufferToTimeElapsedEntry(std::lock_guard<std::mutex>&,uint64_t){}
 void onCommandBufferCompleted(id<MTLCommandBuffer>,uint64_t,uint64_t){}
 angle::ObjCPtr<id<MTLCommandBuffer>>makeMetalCommandBuffer(uint64_t*);
};
struct CommandEncoder{};
struct WrappedBuffer { id<MTLCommandBuffer>buffer=nil; void set(id<MTLCommandBuffer>b){buffer=b;} id<MTLCommandBuffer>get(){return buffer;} };
struct CommandBuffer:WrappedBuffer {
 using ParentClass=WrappedBuffer;CommandQueue&mCmdQueue; explicit CommandBuffer(CommandQueue&q):mCmdQueue(q){}
 std::mutex mLock;uint64_t mQueueSerial=0;bool mCommitted=false;int mLastWaitOp=0;
 std::vector<std::string>mDebugGroups;size_t encoded=0;
 struct PendingEvent{id<MTLEvent>event;uint64_t signalValue;};std::vector<PendingEvent>mPendingSignalEvents;
 bool ready()const{return buffer&&!mCommitted;} bool readyImpl()const{return ready();}
 CommandEncoder*getPendingCommandEncoder(){return nullptr;}
 void setEventImpl(id<MTLEvent>,uint64_t){if(buffer)encoded++;}
 void pushDebugGroupImpl(const std::string&){}void clearResourceListAndSize(){}
 void restart();uint64_t queueEventSignal(id<MTLEvent>,uint64_t);
};
} // mtl
struct Device { bool failEvent=false;id<MTLEvent>newEvent(){return failEvent?nil:[CpuEvent new];} };
struct Display { id getOrCreateSharedEventListener(){return [NSObject new];} };
struct ContextMtl { mtl::CommandBuffer&mCmdBuffer;Device device;Display display;
 explicit ContextMtl(mtl::CommandBuffer&b):mCmdBuffer(b){}
 Device&getMetalDevice(){return device;}Display*getDisplay(){return &display;}
 void flushCommandBufferIfNeeded(){}void endBlitAndComputeEncoding(){}
 void ensureCommandBufferReady(){flushCommandBufferIfNeeded();if(!mCmdBuffer.ready())mCmdBuffer.restart();}
 uint64_t queueEventSignal(id<MTLEvent>,uint64_t);
};
namespace mtl {
struct SharedSync { angle::ObjCPtr<id<MTLSharedEvent>>mMetalSharedEvent;uint64_t mSignalValue=0;id mEventListener=nil;
 angle::Result set(ContextMtl*,id<MTLSharedEvent>,uint64_t,bool); };
struct EventSync { static constexpr uint64_t kEventSignalValue=1;angle::ObjCPtr<id<MTLEvent>>mMetalEvent;
 uint64_t mEncodedCommandBufferSerial=0;angle::Result set(ContextMtl*); };
`;
function program(source) {
 const queue=method(source.queue,'angle::ObjCPtr<id<MTLCommandBuffer>> CommandQueue::makeMetalCommandBuffer');
 const restart=method(source.queue,'void CommandBuffer::restart()');
 const signal=method(source.queue,'uint64_t CommandBuffer::queueEventSignal');
 const shared=method(source.sync,'    angle::Result set(ContextMtl *contextMtl,').replace('angle::Result set(', 'angle::Result SharedSync::set(');
 const event=method(source.sync,'    angle::Result set(ContextMtl *contextMtl)') .replace('angle::Result set(', 'angle::Result EventSync::set(');
 const context=method(source.context,'uint64_t ContextMtl::queueEventSignal');
 return support+[queue,restart,signal,shared,event].join('\n')+'\n} // mtl\n'+context+'\n} // rx\n'+String.raw`
int main(){@autoreleasepool{
 CpuQueue*q=[CpuQueue new];rx::mtl::CommandQueue queue(q);rx::mtl::CommandBuffer buffer(queue);rx::ContextMtl context(buffer);
 id<MTLSharedEvent>event=[CpuEvent new];rx::mtl::SharedSync positive;
 if(positive.set(&context,event,1,true)!=angle::Result::Continue||buffer.encoded!=1||queue.mMetalCmdBuffers.size()!=1)return 10;
 q.failAllocation=true;buffer.buffer=nil;size_t before=queue.mMetalCmdBuffers.size();rx::mtl::SharedSync failed;
 bool stopped=failed.set(&context,event,1,true)==angle::Result::Stop;
 bool noUnbackedSerial=queue.mMetalCmdBuffers.size()==before;
 rx::mtl::SharedSync imported;bool importedOk=imported.set(&context,event,7,false)==angle::Result::Continue;
 rx::mtl::SharedSync nilEvent;bool nilStopped=nilEvent.set(&context,nil,1,false)==angle::Result::Stop;
 rx::mtl::EventSync ordinary;bool ordinaryStopped=ordinary.set(&context)==angle::Result::Stop;
 context.device.failEvent=true;rx::mtl::EventSync noEvent;bool noEventStopped=noEvent.set(&context)==angle::Result::Stop;
 std::printf("valid=1 nilBufferRejected=%d noUnbackedSerial=%d importedPreserved=%d nilSharedRejected=%d ordinaryFenceRejected=%d nilEventRejected=%d\n",stopped,noUnbackedSerial,importedOk,nilStopped,ordinaryStopped,noEventStopped);
 return stopped&&noUnbackedSerial&&importedOk&&nilStopped&&ordinaryStopped&&noEventStopped?0:1;
}}
`;
}
fs.mkdirSync(outputRoot,{recursive:true});
const patched={sync:adapter.adaptAngleMetalSyncAllocation(original.sync),context:adapter.adaptAngleMetalSyncContext(original.context),queue:adapter.adaptAngleMetalCommandBuffers(original.queue)};
const results=[];
for(const [name,source]of [['baseline',original],['fixed',patched]]){
 const input=path.join(outputRoot,name+'.mm'),binary=path.join(outputRoot,name);fs.writeFileSync(input,program(source));
 execFileSync('xcrun',['clang++','-std=c++17','-fobjc-arc','-fblocks','-framework','Foundation',input,'-o',binary],{stdio:'inherit'});
 const result=spawnSync(binary,{encoding:'utf8'});results.push({name,status:result.status,stdout:result.stdout,stderr:result.stderr});
 console.log(name+': '+result.stdout.trim());
}
fs.writeFileSync(path.join(outputRoot,'result.json'),JSON.stringify({sourceRoot:path.resolve(sourceRoot),adapter:path.resolve(adapterFile),results},null,2)+'\n');
if(results[0].status!==1||results[1].status!==0)throw Error('expected original RED and fixed GREEN');
