#include "VideoDecoder.h"
#include <cstring>
extern "C" {
#include "kl_vtdec.h"
}

namespace {
class VideoToolboxDecoder final:public KLJVideoDecoder {
    kl_vtdec *decoder;
public:
    explicit VideoToolboxDecoder(kl_vtdec *value):decoder(value){}
    ~VideoToolboxDecoder()override{kl_vtdec_destroy(decoder);}
    int configuration(const uint8_t *data,size_t size)override{return submit(data,size,0);}
    int submit(const uint8_t *data,size_t size,int64_t pts)override{return kl_vtdec_submit(decoder,data,size,pts);}
    CVPixelBufferRef pull(int64_t *pts)override{return kl_vtdec_pull(decoder,pts);}
    int status()override{return klj_vtdec_status(decoder);}
    unsigned submitted()override{unsigned count=0;kl_vtdec_stats(decoder,&count,nullptr,nullptr,nullptr,nullptr);return count;}
    int drain()override{return klj_vtdec_drain(decoder);}
    void flush()override{kl_vtdec_flush(decoder);}
    void close()override{klj_vtdec_close(decoder);}
    void callback(void(*fn)(void*),void *context)override{klj_vtdec_set_callback(decoder,fn,context);}
};
}
std::shared_ptr<KLJVideoDecoder> klj_video_decoder(const char *mime){
    if(!mime)return {};
    if(!strcmp(mime,"video/x-vnd.on2.vp8")||!strcmp(mime,"video/x-vnd.on2.vp9"))return klj_vpx_decoder(mime);
    auto *decoder=kl_vtdec_create(mime);
    if(!decoder)return {};
    try{return std::make_shared<VideoToolboxDecoder>(decoder);}catch(...){kl_vtdec_destroy(decoder);throw;}
}
