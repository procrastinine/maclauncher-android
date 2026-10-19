#include "display-clock.h"
#include "tick-mailbox.hpp"
#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <mutex>

struct KLDisplayClock {
    std::mutex control;
    CVDisplayLinkRef link=nullptr;
    CGDirectDisplayID display=0;
    std::atomic<CGDirectDisplayID> publishedDisplay{0};
    mach_timebase_info_data_t timebase{};
    klclock::Mailbox mailbox;
    uint64_t consumed=0,sequence=0;
    bool running=false;
    void (*wake)(void*)=nullptr;
    void* wakeOwner=nullptr;
    // Control-locked aggregates. The CoreVideo callback remains lock-free.
    struct {
        bool enabled=false;
        uint64_t began=0,raw=0,delivered=0,starts=0,stops=0,skipped=0,discarded=0;
        uint64_t previousCallback=0,nominal=0,maxCallbackGap=0,maxDeliveryLag=0;
    } trace;
};
static void reportClock(KLDisplayClock* clock,uint64_t now){
    auto& trace=clock->trace;
    if(!trace.enabled||now<trace.began||now-trace.began<2'000'000'000)return;
    uint64_t raw=clock->mailbox.version.load(std::memory_order_relaxed)/2;
    double seconds=double(now-trace.began)/1'000'000'000;
    std::fprintf(stderr,"[android-display] display=%u running=%d seconds=%.3f starts=%llu stops=%llu raw=%llu delivered=%llu skipped=%llu discarded=%llu nominal-hz=%.3f callback-hz=%.3f delivered-hz=%.3f max-callback-gap-ms=%.3f max-delivery-lag-ms=%.3f\n",
        clock->display,clock->running,seconds,(unsigned long long)trace.starts,(unsigned long long)trace.stops,
        (unsigned long long)(raw-trace.raw),(unsigned long long)trace.delivered,
        (unsigned long long)trace.skipped,(unsigned long long)trace.discarded,
        trace.nominal?1'000'000'000.0/double(trace.nominal):0.0,double(raw-trace.raw)/seconds,double(trace.delivered)/seconds,
        double(trace.maxCallbackGap)/1'000'000,double(trace.maxDeliveryLag)/1'000'000);
    trace.began=now;trace.raw=raw;trace.delivered=trace.starts=trace.stops=trace.skipped=trace.discarded=0;
    trace.maxCallbackGap=trace.maxDeliveryLag=0;
}
static bool ui(char* error,size_t cap){if(pthread_main_np())return true;if(error&&cap)snprintf(error,cap,"Display clock requires the native UI thread");return false;}
static int fail(char* error,size_t cap,const char* operation,int code){if(error&&cap)snprintf(error,cap,"%s failed (%d)",operation,code);return 0;}
static CGDirectDisplayID screen(void* cocoaWindow){
    NSWindow* window=(__bridge NSWindow*)cocoaWindow;
    if(!window||![window isKindOfClass:NSWindow.class])return 0;
    return [window.screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}
static CVReturn callback(CVDisplayLinkRef,const CVTimeStamp* now,const CVTimeStamp* output,CVOptionFlags,CVOptionFlags*,void* user){
    auto& clock=*static_cast<KLDisplayClock*>(user);
    if(!now||!output||!(now->flags&kCVTimeStampHostTimeValid)||!(output->flags&kCVTimeStampHostTimeValid)||!(output->flags&kCVTimeStampVideoRefreshPeriodValid)||output->videoTimeScale<=0||output->videoRefreshPeriod<=0)return kCVReturnSuccess;
    uint64_t n=klclock::scale(now->hostTime,clock.timebase.numer,clock.timebase.denom);
    uint64_t p=klclock::scale(output->hostTime,clock.timebase.numer,clock.timebase.denom);
    uint64_t interval=klclock::scale(static_cast<uint64_t>(output->videoRefreshPeriod),1'000'000'000,static_cast<uint32_t>(output->videoTimeScale));
    if(n&&p&&interval){clock.mailbox.publish({n,p,interval});if(clock.wake)clock.wake(clock.wakeOwner);}
    return kCVReturnSuccess;
}
static bool createLink(KLDisplayClock* clock,CGDirectDisplayID display,char* error,size_t cap){
    if(!display){fail(error,cap,"Window display lookup",0);return false;}
    CVReturn code=CVDisplayLinkCreateWithCGDisplay(display,&clock->link);
    if(code!=kCVReturnSuccess||!clock->link){fail(error,cap,"CVDisplayLinkCreateWithCGDisplay",code);return false;}
    if(CVDisplayLinkGetCurrentCGDisplay(clock->link)!=display){CVDisplayLinkRelease(clock->link);clock->link=nullptr;fail(error,cap,"CVDisplayLink display identity",0);return false;}
    code=CVDisplayLinkSetOutputCallback(clock->link,callback,clock);
    if(code!=kCVReturnSuccess){CVDisplayLinkRelease(clock->link);clock->link=nullptr;fail(error,cap,"CVDisplayLinkSetOutputCallback",code);return false;}
    clock->display=display;clock->publishedDisplay.store(display,std::memory_order_release);return true;
}
extern "C" KLDisplayClock* kl_display_clock_create(void* window,char* error,size_t cap){
    if(!ui(error,cap))return nullptr;
    auto* clock=new(std::nothrow) KLDisplayClock;if(!clock){fail(error,cap,"Display clock allocation",0);return nullptr;}
    if(mach_timebase_info(&clock->timebase)!=KERN_SUCCESS||!clock->timebase.denom||!createLink(clock,screen(window),error,cap)){delete clock;return nullptr;}
    const char* trace=std::getenv("KL_DISPLAY_TRACE");clock->trace.enabled=trace&&*trace&&std::strcmp(trace,"0");
    if(clock->trace.enabled)clock->trace.began=kl_display_clock_monotonic_now();
    return clock;
}
static int setDemandLocked(KLDisplayClock* clock,int wanted,char* error,size_t cap){
    if(!clock->link)return fail(error,cap,"Display clock ownership",0);
    if(clock->trace.enabled)reportClock(clock,kl_display_clock_monotonic_now());
    if(clock->running==!!wanted)return 1;
    CVReturn code=wanted?CVDisplayLinkStart(clock->link):CVDisplayLinkStop(clock->link);
    bool running=CVDisplayLinkIsRunning(clock->link);
    clock->running=running;
    if(running!=!!wanted)return fail(error,cap,wanted?"CVDisplayLinkStart":"CVDisplayLinkStop",code);
    if(clock->trace.enabled){if(running)++clock->trace.starts;else ++clock->trace.stops;clock->trace.previousCallback=0;}
    if(!running){uint64_t previous=clock->consumed;clock->mailbox.discard(clock->consumed);if(clock->trace.enabled)clock->trace.discarded+=(clock->consumed-previous)/2;}
    return 1;
}
extern "C" int kl_display_clock_set_demand(KLDisplayClock* clock,int wanted,char* error,size_t cap){
    if(!clock)return fail(error,cap,"Display clock ownership",0);
    std::lock_guard lock(clock->control);return setDemandLocked(clock,wanted,error,cap);
}
extern "C" int kl_display_clock_set_wake(KLDisplayClock* clock,void (*signal)(void*),void* owner,char* error,size_t cap){
    if(!ui(error,cap))return 0;if(!clock)return fail(error,cap,"Display clock ownership",0);std::lock_guard lock(clock->control);
    if(clock->running)return fail(error,cap,"Display wake installation requires a stopped source",0);
    clock->wake=signal;clock->wakeOwner=owner;return 1;
}
extern "C" int kl_display_clock_update_display(KLDisplayClock* clock,void* window,char* error,size_t cap){
    if(!ui(error,cap))return 0;if(!clock)return fail(error,cap,"Display clock ownership",0);
    std::lock_guard lock(clock->control);
    CGDirectDisplayID display=screen(window);if(!display)return fail(error,cap,"Window display lookup",0);if(display==clock->display&&clock->link)return 1;
    bool resume=clock->running;if(clock->link&&!setDemandLocked(clock,0,error,cap))return 0;
    if(clock->link)CVDisplayLinkRelease(clock->link);clock->link=nullptr;clock->display=0;
    if(!createLink(clock,display,error,cap))return 0;
    return !resume||setDemandLocked(clock,1,error,cap);
}
extern "C" int kl_display_clock_poll(KLDisplayClock* clock,KLDisplayTick* result){
    if(!pthread_main_np()||!clock||!result)return 0;std::lock_guard lock(clock->control);if(!clock->running)return 0;
    uint64_t now=clock->trace.enabled?kl_display_clock_monotonic_now():0;
    klclock::Tick tick{};uint64_t skipped=0;if(!clock->mailbox.read(clock->consumed,tick,skipped)){if(clock->trace.enabled)reportClock(clock,now);return 0;}
    if(clock->trace.enabled){
        auto& trace=clock->trace;++trace.delivered;trace.skipped+=skipped;trace.nominal=tick.interval;
        if(trace.previousCallback&&tick.callback>=trace.previousCallback){uint64_t gap=tick.callback-trace.previousCallback;if(gap>trace.maxCallbackGap)trace.maxCallbackGap=gap;}
        if(now>=tick.callback&&now-tick.callback>trace.maxDeliveryLag)trace.maxDeliveryLag=now-tick.callback;
        trace.previousCallback=tick.callback;reportClock(clock,now);
    }
    *result={++clock->sequence,tick.callback,tick.output,tick.interval,skipped,clock->display};return 1;
}
extern "C" int kl_display_clock_sample(KLDisplayClock* clock,uint64_t* cursor,KLDisplayTick* result){
    if(!clock||!cursor||!result)return 0;
    klclock::Tick tick{};uint64_t skipped=0;if(!clock->mailbox.read(*cursor,tick,skipped))return 0;
    *result={*cursor/2,tick.callback,tick.output,tick.interval,skipped,clock->publishedDisplay.load(std::memory_order_acquire)};return 1;
}
extern "C" int kl_display_clock_destroy(KLDisplayClock* clock,char* error,size_t cap){
    if(!ui(error,cap))return 0;if(!clock)return 1;
    {std::lock_guard lock(clock->control);if(clock->link){if(!setDemandLocked(clock,0,error,cap))return 0;CVDisplayLinkRelease(clock->link);clock->link=nullptr;}}
    delete clock;return 1;
}
extern "C" uint64_t kl_display_clock_monotonic_now(void){
    mach_timebase_info_data_t timebase{};if(mach_timebase_info(&timebase)!=KERN_SUCCESS)return 0;
    return klclock::scale(mach_absolute_time(),timebase.numer,timebase.denom);
}
