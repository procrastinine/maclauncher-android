#pragma once
#import <Metal/Metal.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int klj_xr_compositor_start(void *layer);
int klj_xr_compositor_present(int width,int height);
void klj_xr_compositor_stop(void);
id<MTLTexture> klj_xr_output(id<MTLCommandBuffer> commands,int width,int height);
void klj_xr_submit(id<MTLCommandBuffer> commands,id<MTLTexture> target,uint64_t value);
void klj_xr_discard(id<MTLCommandBuffer> commands,uint64_t value);
void klj_xr_begin(id<MTLCommandBuffer> commands,uint64_t value);
id<MTLCommandQueue> klj_xr_queue(id<MTLDevice> device);
void klj_xr_fences(id<MTLSharedEvent> produced,int gl);
#ifdef __cplusplus
}
#endif
