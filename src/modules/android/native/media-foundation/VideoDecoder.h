#pragma once
#include <CoreVideo/CoreVideo.h>
#include <cstddef>
#include <cstdint>
#include <memory>

// Shared packet/output ownership beneath the original Java and NDK MediaCodec
// clients. pull() transfers one retained CVPixelBuffer to the existing owner.
class KLJVideoDecoder {
public:
    virtual ~KLJVideoDecoder()=default;
    virtual int configuration(const uint8_t*,size_t)=0;
    virtual int submit(const uint8_t*,size_t,int64_t)=0;
    virtual CVPixelBufferRef pull(int64_t*)=0;
    virtual int status()=0;
    virtual unsigned submitted()=0;
    virtual int drain()=0;
    virtual void flush()=0;
    virtual void close()=0;
    virtual void callback(void(*)(void*),void*)=0;
};

constexpr unsigned KLJ_VPX_MAX_WIDTH=4096,KLJ_VPX_MAX_HEIGHT=2304;
std::shared_ptr<KLJVideoDecoder> klj_video_decoder(const char*);
std::shared_ptr<KLJVideoDecoder> klj_vpx_decoder(const char*);
