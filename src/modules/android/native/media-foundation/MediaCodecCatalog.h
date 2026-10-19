#pragma once
#include <cstdint>
#include <span>
#include <vector>

// The native codec owner's construction and discovery use the same descriptors.
// Android's original CodecCapabilities owns format/profile matching above this boundary.
struct KLJCodecProfileLevel {int32_t profile,level;};
struct KLJCodecDescriptor {
    const char *mime,*name;
    bool hardware;
    std::vector<KLJCodecProfileLevel> profiles;
    std::vector<int32_t> colors;
    unsigned maxWidth=0,maxHeight=0;
};
std::span<const KLJCodecDescriptor> klj_codec_catalog();
const KLJCodecDescriptor *klj_codec_descriptor(const char *name,bool nameIsType);
