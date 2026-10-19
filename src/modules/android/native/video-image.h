#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Plane offsets/strides come from the original buffer allocator. FourCC uses
 * the allocator's DRM byte order; no native_handle or VM object crosses here. */
typedef struct {uint64_t offset,stride,width,height;} KljVideoPlane;
typedef struct {
 uint64_t address,length;
 uint32_t width,height,fourcc,plane_count;
 KljVideoPlane planes[3];
} KljVideoBuffer;
typedef void*(*KljVideoResolve)(const char*);
/* The image lease owns this per-image interoperability state. All callbacks
 * use the selected ANGLE Vulkan device/queue, never another graphics device. */
typedef struct {
 void *owner,*device,*ready,*consumed;
 void *(*import_texture)(void*,void*,uint32_t,uint32_t,uint32_t);
 int (*begin)(void*,uint64_t);
 int (*end)(void*,uint64_t);
 void (*destroy)(void*);
} KljVideoInterop;
void *klj_video_image_create(const KljVideoBuffer*,void *device,void *display,KljVideoResolve,void **image);
/* Consumes interop.owner even when creation fails. */
void *klj_video_image_create_interop(const KljVideoBuffer*,void *display,KljVideoResolve,const KljVideoInterop*,void **image);
int klj_video_image_refresh(void*,int32_t dataspace);
void klj_video_image_destroy(void*);
#ifdef __cplusplus
}
#endif
