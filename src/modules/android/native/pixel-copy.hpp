#pragma once
#import <Metal/Metal.h>
#include <jni.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace klj_copy {
enum Status {Success=0,Unknown=1,Timeout=2,NoData=3,SourceInvalid=4,DestinationInvalid=5};
struct Request;
// Caller owns the presenter's frame mutex. Requests retain neither presenter
// nor native window. Only an explicitly requested copy allocates GPU storage.
class Queue {
    std::vector<std::weak_ptr<Request>> pending;
    id<MTLComputePipelineState> pipeline=nil;
public:
    ~Queue();
    uint64_t add(JNIEnv*,jobject callback,int width,int height,int immediateStatus=-1,int left=0,int top=0,int right=0,int bottom=0);
    bool waiting();
    void encode(id<MTLCommandBuffer>,id<MTLTexture>,bool bottomLeft);
    void retire(int status=SourceInvalid);
};
void cancel(uint64_t requestId);
}
