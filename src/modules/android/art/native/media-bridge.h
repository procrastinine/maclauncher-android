#pragma once
#include <stdint.h>
enum KljArtMediaOperation {
    KLJ_ART_MEDIA_DEVICES, KLJ_ART_MEDIA_CONFIGURE, KLJ_ART_MEDIA_AUTHORIZED,
    KLJ_ART_MEDIA_SHUTDOWN, KLJ_ART_MEDIA_AUDIO_OPEN, KLJ_ART_MEDIA_AUDIO_START,
    KLJ_ART_MEDIA_AUDIO_READ, KLJ_ART_MEDIA_AUDIO_STOP, KLJ_ART_MEDIA_AUDIO_CLOSE,
    KLJ_ART_MEDIA_CAMERA_SIZES, KLJ_ART_MEDIA_CAMERA_OPEN, KLJ_ART_MEDIA_CAMERA_START,
    KLJ_ART_MEDIA_CAMERA_READ, KLJ_ART_MEDIA_CAMERA_STOP, KLJ_ART_MEDIA_CAMERA_CLOSE
};
/* All buffers belong to the guest caller and remain valid for this call only.
 * Sizes are bytes except AUDIO_READ, whose capacity/result are PCM samples. */
struct KljArtMedia {
    uint32_t operation;
    int32_t first, second, reserved;
    uint64_t token, data, other, capacity, length;
    int64_t result;
};
