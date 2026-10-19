#include "MediaCodecCatalog.h"
#include "VideoDecoder.h"
#include <VideoToolbox/VideoToolbox.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <string>

namespace {
CFStringRef symbol(const char *name){auto value=static_cast<CFStringRef*>(dlsym(RTLD_DEFAULT,name));return value?*value:nullptr;}
CFTypeRef get(CFDictionaryRef dictionary,const char *key){auto name=symbol(key);return dictionary&&name?CFDictionaryGetValue(dictionary,name):nullptr;}
int number(CFTypeRef value){int out=0;if(value&&CFGetTypeID(value)==CFNumberGetTypeID())CFNumberGetValue(static_cast<CFNumberRef>(value),kCFNumberIntType,&out);return out;}
int hevcLevel(int idc){
    // ISO/IEC 23008-2 level_idc to the original Android profile-level encoding.
    constexpr int levels[]={30,60,63,90,93,120,123,150,153,156,180,183,186};
    for(size_t i=std::size(levels);i-->0;)if(idc>=levels[i])return 1u<<(2*i);
    return HEVCMainTierLevel1;
}
void hevcProfiles(KLJCodecDescriptor &codec){
    // This is the same optional VideoToolbox capability query used by WebKit.
    // It supplies actual host decode levels rather than encoder limits.
    using Copy=CFDictionaryRef(*)();
    auto copy=reinterpret_cast<Copy>(dlsym(RTLD_DEFAULT,"VTCopyHEVCDecoderCapabilitiesDictionary"));
    CFDictionaryRef capabilities=copy?copy():nullptr;
    auto profiles=get(capabilities,"kVTHEVCDecoderCapability_PerProfileSupport");
    if(profiles&&CFGetTypeID(profiles)==CFDictionaryGetTypeID()){
        for(auto [id,profile]:{std::pair{1,HEVCProfileMain},std::pair{2,HEVCProfileMain10}}){
            const auto text=std::to_string(id);
            CFStringRef key=CFStringCreateWithCString(nullptr,text.c_str(),kCFStringEncodingUTF8);
            auto entry=CFDictionaryGetValue(static_cast<CFDictionaryRef>(profiles),key);CFRelease(key);
            if(entry&&CFGetTypeID(entry)==CFDictionaryGetTypeID()){
                const int level=number(get(static_cast<CFDictionaryRef>(entry),"kVTHEVCDecoderProfileCapability_MaxDecodeLevel"));
                if(level)codec.profiles.push_back({profile,hevcLevel(level)});
            }
        }
    }
    if(capabilities)CFRelease(capabilities);
    // The public VideoToolbox contract predates the optional capability dictionary.
    // Retain its baseline support when that query is absent on an older macOS.
    if(codec.profiles.empty())codec.profiles={{HEVCProfileMain,HEVCMainTierLevel51},{HEVCProfileMain10,HEVCMainTierLevel51}};
}
}
std::span<const KLJCodecDescriptor> klj_codec_catalog(){
    static const std::vector<KLJCodecDescriptor> descriptors=[] {
        // Discovery and construction share the same native decoder boundary.
        std::vector<KLJCodecDescriptor> out={
            {"video/avc","maclauncher.videotoolbox.avc.decoder",bool(VTIsHardwareDecodeSupported(kCMVideoCodecType_H264)),
             {{AVCProfileBaseline,AVCLevel52},{AVCProfileConstrainedBaseline,AVCLevel52},{AVCProfileMain,AVCLevel52},{AVCProfileHigh,AVCLevel52},{AVCProfileConstrainedHigh,AVCLevel52}},
             {COLOR_FormatYUV420SemiPlanar,COLOR_Format32bitARGB8888,COLOR_FormatSurface}},
            {"video/hevc","maclauncher.videotoolbox.hevc.decoder",bool(VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC)),{},
             {COLOR_FormatYUV420SemiPlanar,COLOR_FormatYUVP010,COLOR_Format32bitARGB8888,COLOR_FormatSurface}},
            {"video/x-vnd.on2.vp8","maclauncher.libvpx.vp8.decoder",false,
             {{VP8ProfileMain,VP8Level_Version0},{VP8ProfileMain,VP8Level_Version1},{VP8ProfileMain,VP8Level_Version2},{VP8ProfileMain,VP8Level_Version3}},
             {COLOR_FormatYUV420SemiPlanar,COLOR_FormatSurface},KLJ_VPX_MAX_WIDTH,KLJ_VPX_MAX_HEIGHT},
            {"video/x-vnd.on2.vp9","maclauncher.libvpx.vp9.decoder",false,{{VP9Profile0,VP9Level51}},
             {COLOR_FormatYUV420SemiPlanar,COLOR_FormatSurface},KLJ_VPX_MAX_WIDTH,KLJ_VPX_MAX_HEIGHT}
        };
        hevcProfiles(out[1]);return out;
    }();
    return descriptors;
}
const KLJCodecDescriptor *klj_codec_descriptor(const char *name,bool type){
    if(!name)return nullptr;
    if(type&&!strcasecmp(name,"video/x-hevc"))name="video/hevc";
    for(const auto&entry:klj_codec_catalog())if(type?!strcasecmp(name,entry.mime):!strcmp(name,entry.name))return &entry;
    return nullptr;
}
