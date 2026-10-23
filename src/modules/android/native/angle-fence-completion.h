#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <cstdint>

namespace maclauncher {
// Only the owned native fence producer installs this request, while its
// synchronous eglCreateSync call owns the request's storage. ANGLE copies the
// callback contract and retains its context before returning from that call.
struct FenceCompletionRequest {
    uint32_t size, version, attached, reserved;
    uint64_t value;
    void *context;
    void (*retain)(void *);
    void (*completed)(void *, int32_t);
    void (*release)(void *);
};
static_assert(sizeof(FenceCompletionRequest) == 56);

inline void observeNativeFenceCompletion(id<MTLCommandBuffer> commands,
                                         id<MTLEvent> event, uint64_t value) {
    if (!commands || !event) return;
    const void *key = reinterpret_cast<const void *>(
        sel_registerName("maclauncher.angle.fence.completion.v1"));
    NSValue *boxed = (NSValue *)objc_getAssociatedObject(event, key);
    if (![boxed isKindOfClass:NSValue.class]) return;
    auto *request = static_cast<FenceCompletionRequest *>(boxed.pointerValue);
    if (!request || request->size != sizeof(*request) || request->version != 1 ||
        request->attached || request->value != value || !request->context ||
        !request->retain || !request->completed || !request->release) return;
    const FenceCompletionRequest observer = *request;
    observer.retain(observer.context);
    request->attached = 1;
    [commands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        // A real failed completion retires the Linux fence with EIO. A
        // successful buffer leaves signaling to the real shared-event listener.
        observer.completed(observer.context,
            completed.status == MTLCommandBufferStatusError ? -5 : 0);
        observer.release(observer.context);
    }];
}
}
