/* Reuse Klepton's real video/graphics AHardwareBuffer owner. The Java framework
 * and decoded guest video share the same CVPixelBuffer representation. */
#include KLEPTON_MEDIANDK_SOURCE

void *klj_hardwarebuffer_create(CVPixelBufferRef pixels) { return klm_hwbuf_new(pixels); }
void klj_hardwarebuffer_acquire(void *buffer) { klm_AHardwareBuffer_acquire(buffer); }
void klj_hardwarebuffer_release(void *buffer) { klm_AHardwareBuffer_release(buffer); }
