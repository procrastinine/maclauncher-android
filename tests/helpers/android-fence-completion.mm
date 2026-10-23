#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <cassert>
#include <cstdio>
#include "angle-fence-completion.h"

@interface CpuCommands : NSObject
@property(copy) void (^completion)(id<MTLCommandBuffer>);
@property MTLCommandBufferStatus status;
- (void)addCompletedHandler:(void (^)(id<MTLCommandBuffer>))handler;
@end
@implementation CpuCommands
- (void)addCompletedHandler:(void (^)(id<MTLCommandBuffer>))handler { self.completion=handler; }
@end
struct Context { unsigned retained=0,released=0,completed=0;int status=99; };
void retain(void *pointer) { ++static_cast<Context *>(pointer)->retained; }
void completed(void *pointer,int32_t status) { auto *c=static_cast<Context *>(pointer);++c->completed;c->status=status; }
void release(void *pointer) { ++static_cast<Context *>(pointer)->released; }
int main() { @autoreleasepool {
    const void *key=reinterpret_cast<const void *>(sel_registerName("maclauncher.angle.fence.completion.v1"));
    NSObject *event=[NSObject new];CpuCommands *commands=[CpuCommands new];Context context;
    maclauncher::FenceCompletionRequest request={sizeof(request),1,0,0,7,&context,retain,completed,release};
    objc_setAssociatedObject(event,key,[NSValue valueWithPointer:&request],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    // A different event value, absent metadata and a nil buffer add no observer.
    maclauncher::observeNativeFenceCompletion((id)commands,(id)event,6);assert(!commands.completion);
    maclauncher::observeNativeFenceCompletion(nil,(id)event,7);assert(!request.attached);
    maclauncher::observeNativeFenceCompletion((id)commands,(id)[NSObject new],7);assert(!commands.completion);
    maclauncher::observeNativeFenceCompletion((id)commands,(id)event,7);assert(request.attached==1&&context.retained==1);
    maclauncher::observeNativeFenceCompletion((id)commands,(id)event,7);assert(context.retained==1);
    // Native creation can drop its metadata and stack request before the real
    // command buffer later invokes the copied completion callback.
    objc_setAssociatedObject(event,key,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    commands.status=MTLCommandBufferStatusError;commands.completion((id)commands);
    assert(context.completed==1&&context.status==-5&&context.released==1);
    Context success;request={sizeof(request),1,0,0,8,&success,retain,completed,release};
    objc_setAssociatedObject(event,key,[NSValue valueWithPointer:&request],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    CpuCommands *good=[CpuCommands new];maclauncher::observeNativeFenceCompletion((id)good,(id)event,8);
    good.status=MTLCommandBufferStatusCompleted;good.completion((id)good);
    assert(success.completed==1&&success.status==0&&success.released==1);
    objc_setAssociatedObject(event,key,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    puts("PASS copied completion lifetime, exact event/value, duplicate registration, actual Error=-EIO, success delegated; zero Metal allocations/submissions");
} }
