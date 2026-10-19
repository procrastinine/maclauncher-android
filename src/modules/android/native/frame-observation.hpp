#pragma once
#include "frame-timing.hpp"
#include "shared-presentation.h"
#include "android-log.h"
#include <atomic>
#include <cerrno>
#include <cstdio>

namespace klj_timing {
struct Presentation {
    void *presenter=nullptr;
    int (*status)(void *,RSPresenterStatus *)=nullptr;
    uint64_t generation=0,accepted=0,busy=0;
    unsigned width=0,height=0,outputWidth=0,outputHeight=0;
};
// Both execution bindings observe their actual producer and common presenter.
// Composition observations remain distinct from application-frame delivery.
class Reporter {
    static inline std::atomic<uint64_t> nextId{0};
    const uint64_t id=++nextId;
    Window timing;
    uint64_t accepted=0,busy=0,real=0,generated=0,generation=0;
    uint64_t observedFrames=0,firstFrame=0;
public:
    void record(uint64_t start,uint64_t end,const char *path,const char *stage,const Presentation &p) noexcept {
        if(!observedFrames)firstFrame=start;
        ++observedFrames;timing.record(start,end);
        // Retain the first real source observation even if it never produces
        // another frame. The second supplies the first interval; subsequent
        // reports keep the existing bounded cadence and require no idle timer.
        if(observedFrames>2&&!timing.due(end))return;
        const double seconds=double(end-timing.began)/1e9;
        char sourceFps[32]="null";
        if(timing.intervals)snprintf(sourceFps,sizeof(sourceFps),"%.3f",timing.fps());
        const auto rate=[&](uint64_t count){return timing.intervals&&seconds>0?double(count)/seconds:0;};
        RSPresenterStatus status{};status.size=sizeof(status);status.version=RS_PRESENTER_ABI_VERSION;
        const bool available=p.presenter&&p.status&&p.status(p.presenter,&status)==RS_FRAME_OK;
        if(generation!=p.generation){real=generated=0;generation=p.generation;}
        const auto currentReal=available?status.presentedReal:0,currentGenerated=available?status.presentedGenerated:0;
        const auto delta=[](uint64_t current,uint64_t previous){return current>=previous?current-previous:current;};
        const int savedError=errno;
        char message[2048];
        const int length=snprintf(message,sizeof message,"[android-frame] {\"surface\":%llu,\"path\":\"%s\",\"stage\":\"%s\",\"counterScope\":\"source\",\"hostSeconds\":%.6f,\"seconds\":%.6f,\"observedFrames\":%llu,\"intervals\":%llu,\"firstFrameHostSeconds\":%.6f,\"lastFrameHostSeconds\":%.6f,\"sourceFps\":%s,\"acceptedFps\":%.3f,\"presentedFps\":%.3f,\"generatedFps\":%.3f,\"presentedAvailable\":%s,\"intervalMeanMs\":%.3f,\"intervalP95Ms\":%.3f,\"intervalMaxMs\":%.3f,\"submitMeanMs\":%.3f,\"submitMaxMs\":%.3f,\"attempts\":%llu,\"busy\":%llu,\"pending\":%u,\"sourceWidth\":%u,\"sourceHeight\":%u,\"outputWidth\":%u,\"outputHeight\":%u}",
            (unsigned long long)id,path,stage,double(end)/1e9,seconds,(unsigned long long)observedFrames,(unsigned long long)timing.intervals,double(firstFrame)/1e9,double(start)/1e9,sourceFps,rate(delta(p.accepted,accepted)),
            available?rate(delta(currentReal,real)):0,available?rate(delta(currentGenerated,generated)):0,
            available?"true":"false",timing.intervals?double(timing.intervalNs)/timing.intervals/1e6:0,timing.percentile(.95),double(timing.maxIntervalNs)/1e6,
            timing.frames?double(timing.submitNs)/timing.frames/1e6:0,double(timing.maxSubmitNs)/1e6,
            (unsigned long long)timing.frames,(unsigned long long)delta(p.busy,busy),status.pending,p.width,p.height,p.outputWidth,p.outputHeight);
        // Application stdio retains its original lifetime. Native diagnostics
        // use the existing process log owner even after a guest closes stderr.
        if(length>0&&size_t(length)<sizeof message)klj_android_log_write(4,"AndroidFrame",message);
        errno=savedError;
        accepted=p.accepted;busy=p.busy;real=currentReal;generated=currentGenerated;timing.reset(end);
    }
};
}
