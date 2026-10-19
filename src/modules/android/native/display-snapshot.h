#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KLDisplaySnapshot {
    uint32_t screenId;
    uint32_t modeId;int32_t physicalWidth,physicalHeight;
    float refreshRate;
    /* 1 = current CGDisplayMode; 2 = current screen's nominal CVDisplayLink period. */
    int32_t refreshSource;
    float windowWidth,windowHeight; /* Native content points, for Android density. */
} KLDisplaySnapshot;
/* Main/UI thread only; borrows the actual NSWindow during this call. No clock
 * starts, display mode changes, retained window, default monitor, or 60Hz fallback.
 * Returns 1 on success; leaves output untouched on failure. Call on window bind
 * and actual SDL screen/mode events, never once per frame. */
int kl_display_snapshot_read(void* cocoaWindow,KLDisplaySnapshot*,char* error,size_t capacity);
/* Read an actual display for the profile compositor without creating a window. */
int kl_display_snapshot_screen(uint32_t screen,KLDisplaySnapshot*,char* error,size_t capacity);
#ifdef __cplusplus
}
#endif
