#pragma once
#import <Metal/Metal.h>
#include <jni.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace klj_copy {
enum Status {Success=0,Unknown=1,Timeout=2,NoData=3,SourceInvalid=4,DestinationInvalid=5};
struct Request;
struct SubmittedSource;
struct SourcePool;
// Caller owns the presenter's frame mutex. Requests retain neither presenter
// nor native window. Only an explicitly requested copy allocates GPU storage.
class Queue {
    friend struct SubmittedSource;
    std::vector<std::weak_ptr<Request>> pending;
    id<MTLComputePipelineState> pipeline=nil;
    std::shared_ptr<SubmittedSource> submitted;
    std::shared_ptr<SourcePool> sourcePool;
    int sourceStatus=-1;
public:
    ~Queue();
    uint64_t add(JNIEnv*,jobject callback,int width,int height,int immediateStatus=-1,int left=0,int top=0,int right=0,int bottom=0);
    bool waiting();
    void encode(id<MTLCommandBuffer>,id<MTLTexture>,bool bottomLeft);
    // A producer lease prevents recycling this exact raw image after its
    // command buffer completes. Mutable texture ownership alone is insufficient.
    void source(id<MTLCommandBuffer>,id<MTLTexture>,bool bottomLeft,std::shared_ptr<void> producerLease);
    // Mutable single-buffer producers snapshot inside their existing GPU fence.
    // Retainable producers use source() without an additional image copy.
    void snapshot(id<MTLCommandBuffer>,id<MTLTexture>,bool bottomLeft);
    void clearSource();
    void retire(int status=SourceInvalid);
};
void cancel(uint64_t requestId);
}
