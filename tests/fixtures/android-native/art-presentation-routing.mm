// Compile actual ART loader/clock/dispatch code with deterministic ABI peers.
// No device, window, renderer or scheduling implementation is simulated.
#import <Foundation/Foundation.h>
@protocol MTLCommandQueue;
#include "shared-presentation.h"
#include "target-time.h"
#include "frame-observation.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <new>
#include <cmath>
#include <thread>
#include <atomic>
static int libraryMarker,presenterMarker,tokenMarker,observerMarker;
static unsigned opens,closes,observedCalls,plainCalls,releases;
static bool missingObserved,synchronous;
static int outcome=RS_FRAME_OK;
static const RSPresenterSubmission*expectedSubmission;
static const RSPresentationObserver*expectedObserver;
struct Receipt {uint64_t frame;double shown,time;uint32_t outcome;};
struct Accepted {RSPresenterSubmission submission;RSPresentationObserver observer;};
static std::vector<Accepted> accepted;
static std::vector<Receipt> forwarded;
static std::vector<std::string> requested;
static void(*reenter)();
static void releaseToken(void*token){assert(token==&tokenMarker);++releases;}
static void retired(void*context,uint64_t frame,double shown,double time,uint32_t status){assert(context==&observerMarker);forwarded.push_back({frame,shown,time,status});if(reenter)reenter();}
static void*create(void*,void*,const RSPresenterOptions*,RSPresenterError*){assert(false);return nullptr;}
static int status(void*,RSPresenterStatus*){assert(false);return 0;}
static int timing(void*,RSPresentationTiming*){assert(false);return 0;}
static void destroy(void*){assert(false);}
static int observed(void*presenter,const RSPresenterSubmission*submission,const RSPresentationObserver*observer,RSFrameResult*result){
 assert(presenter==&presenterMarker&&submission==expectedSubmission);
 assert(submission->sourceTime==12.125&&submission->lifetimeToken==&tokenMarker&&submission->releaseToken==releaseToken);
 assert(observer->size==expectedObserver->size&&observer->version==expectedObserver->version);++observedCalls;
 if(!expectedObserver->retired){assert(observer==expectedObserver);return RS_FRAME_INVALID_ARGUMENT;}
 assert(observer->context!=expectedObserver->context&&observer->retired!=expectedObserver->retired);
 if(outcome==RS_FRAME_OK){if(synchronous)observer->retired(observer->context,submission->frameId,90.25,90.5,RS_PRESENTED);else accepted.push_back({*submission,*observer});}
 result->encoded=outcome==RS_FRAME_OK;result->frameId=submission->frameId;std::strcpy(result->reason,"original dispatch result");return outcome;
}
static int plain(void*presenter,const RSPresenterSubmission*submission,RSFrameResult*result){assert(presenter==&presenterMarker&&submission==expectedSubmission);++plainCalls;result->frameId=submission->frameId;return outcome;}
static void*fixtureOpen(const char*file,int flags){assert(!std::strcmp(file,"fixture-core")&&flags==(RTLD_NOW|RTLD_LOCAL));++opens;return &libraryMarker;}
static int fixtureClose(void*library){assert(library==&libraryMarker);++closes;return 0;}
static const char*fixtureError(){return "fixture loader failure";}
static void*fixtureSymbol(void*library,const char*name){assert(library==&libraryMarker);requested.emplace_back(name);
 if(!std::strcmp(name,"rsCreateFramePresenter"))return reinterpret_cast<void*>(create);
 if(!std::strcmp(name,"rsSubmitFramePresenter"))return reinterpret_cast<void*>(plain);
 if(!std::strcmp(name,"rsSubmitFramePresenterObserved"))return missingObserved?nullptr:reinterpret_cast<void*>(observed);
 if(!std::strcmp(name,"rsGetFramePresenterStatus"))return reinterpret_cast<void*>(status);
 if(!std::strcmp(name,"rsGetFramePresenterTiming"))return reinterpret_cast<void*>(timing);
 if(!std::strcmp(name,"rsDestroyFramePresenter"))return reinterpret_cast<void*>(destroy);
 assert(false&&"ART must not resolve a targeted fallback");return nullptr;
}
#define dlopen fixtureOpen
#define dlsym fixtureSymbol
#define dlclose fixtureClose
#define dlerror fixtureError
/* ACTUAL_ART_PRESENTATION_DISPATCH */
#undef dlopen
#undef dlsym
#undef dlclose
#undef dlerror
static void finish(size_t index,uint64_t frame,double shown,double time,uint32_t status){auto observer=accepted.at(index).observer;assert(observer.retired);accepted[index].observer.retired=nullptr;const auto before=forwarded.size();observer.retired(observer.context,frame,shown,time,status);assert(forwarded.size()==before+1);const auto&last=forwarded.back();assert(last.frame==frame&&(last.shown==shown||(std::isnan(last.shown)&&std::isnan(shown)))&&last.time==time&&last.outcome==status);}
static void clockIs(uint64_t frame,uint64_t nanos){auto actual=displayedPresentation();assert(actual.frame==frame&&actual.nanos==nanos);}
int main(){
 unsetenv("KLEPTON_FRAME_LIBRARY");assert(prepareFrameLibrary()&&opens==0&&!frames.library);
 setenv("KLEPTON_FRAME_LIBRARY","fixture-core",1);missingObserved=true;
 assert(std::strstr(prepareFrameLibrary(),"observed frame presentation")&&opens==1&&closes==1&&!frames.library);
 assert(observedCalls==0&&plainCalls==0&&releases==0&&forwarded.empty());
 missingObserved=false;assert(!prepareFrameLibrary()&&frames.library==&libraryMarker&&opens==2&&closes==1);
 assert(!prepareFrameLibrary()&&opens==2);frames.presenter=&presenterMarker;frames.clock=std::make_shared<PresentationClock>();
 RSPresenterSubmission submission{sizeof(submission),RS_PRESENTER_ABI_VERSION,reinterpret_cast<void*>(0x11),reinterpret_cast<void*>(0x22),73,12.125,&tokenMarker,releaseToken};
 RSPresentationObserver observer{sizeof(observer),RS_TARGET_ABI_VERSION,&observerMarker,retired};
 RSFrameResult result{};result.size=sizeof(result);result.version=RS_FRAME_ABI_VERSION;expectedSubmission=&submission;expectedObserver=&observer;
 auto submit=[&](uint64_t frame){submission.frameId=frame;assert(submitPresentationFrame(submission,&observer,result)==RS_FRAME_OK);return accepted.size()-1;};
 const size_t first=submit(73);assert(result.encoded&&result.frameId==73&&frames.clock.use_count()==2);clockIs(0,0);
 accepted[first].submission.releaseToken(accepted[first].submission.lifetimeToken);assert(releases==1&&forwarded.empty());clockIs(0,0);
 finish(first,73,12.5,13.25,RS_PRESENTED);clockIs(73,12500000000);assert(frames.clock.use_count()==1);
 // Only a real, finite, positive, representable presentation of this source
 // updates the pair. Original callbacks retain all values and outcomes.
 for(const auto test:std::vector<Receipt>{{74,14,15,RS_PRESENT_DROPPED},{75,14,15,RS_PRESENT_CANCELLED},{76,14,15,RS_PRESENT_GPU_ERROR},{77,NAN,15,RS_PRESENTED},{78,INFINITY,15,RS_PRESENTED},{79,-1,15,RS_PRESENTED},{80,0,15,RS_PRESENTED},{81,1e100,15,RS_PRESENTED},{82,11,15,RS_PRESENTED}}){const auto i=submit(test.frame);finish(i,test.frame,test.shown,test.time,test.outcome);clockIs(73,12500000000);}
 auto wrong=submit(83);finish(wrong,999,16,17,RS_PRESENTED);clockIs(73,12500000000);
 auto fresh=submit(84);finish(fresh,84,18,19,RS_PRESENTED);clockIs(84,18000000000);
 const auto callbacks=forwarded.size(),acceptedCount=accepted.size();
 for(int code:{RS_FRAME_BUSY,RS_FRAME_ENCODE_FAILED,RS_FRAME_INVALID_ARGUMENT}){outcome=code;assert(submitPresentationFrame(submission,&observer,result)==code&&!std::strcmp(result.reason,"original dispatch result"));assert(forwarded.size()==callbacks&&accepted.size()==acceptedCount&&frames.clock.use_count()==1&&releases==1);clockIs(84,18000000000);}
 outcome=RS_FRAME_OK;assert(submitPresentationFrame(submission,nullptr,result)==RS_FRAME_OK&&plainCalls==1);clockIs(84,18000000000);
 observer.retired=nullptr;assert(submitPresentationFrame(submission,&observer,result)==RS_FRAME_INVALID_ARGUMENT&&frames.clock.use_count()==1);observer.retired=retired;
 // Poll and callback threads share one coherent pair, not independently read
 // frame/time atomics. Every intermediate pair must have the exact relation.
 std::vector<size_t> pending;for(uint64_t i=0;i<64;++i)pending.push_back(submit(100+i));
 std::atomic<bool> done=false;std::thread writer([&]{for(size_t i=0;i<pending.size();++i)finish(pending[i],100+i,20+double(i),84+double(i),RS_PRESENTED);done=true;});
 do{const auto p=displayedPresentation();assert((p.frame==84&&p.nanos==18000000000)||(p.frame>=100&&p.frame<164&&p.nanos==(p.frame-80)*1000000000));}while(!done.load());writer.join();clockIs(163,83000000000);
 // Stop releases the presenter reference, but accepted callbacks retain the
 // old generation. Late presented/cancelled events cannot touch replacement.
 auto late=submit(200),cancelled=submit(201);std::weak_ptr<PresentationClock> old=frames.clock;retirePresentationClock();assert(!frames.clock&&!old.expired());clockIs(0,0);
 frames.clock=std::make_shared<PresentationClock>();finish(late,200,100,101,RS_PRESENTED);clockIs(0,0);assert(!old.expired());finish(cancelled,201,0,102,RS_PRESENT_CANCELLED);assert(old.expired());clockIs(0,0);
 // The original owner can poll and replace from its callback without a held
 // lock, and the bridge remains valid until that reentrant callback returns.
 auto reentrant=submit(300);std::weak_ptr<PresentationClock> replaced=frames.clock;
 reenter=[](){clockIs(300,110000000000);retirePresentationClock();frames.clock=std::make_shared<PresentationClock>();};finish(reentrant,300,110,111,RS_PRESENTED);reenter=nullptr;assert(replaced.expired());clockIs(0,0);
 synchronous=true;submission.frameId=301;assert(submitPresentationFrame(submission,&observer,result)==RS_FRAME_OK);clockIs(301,90250000000);assert(frames.clock.use_count()==1);synchronous=false;
 assert(submission.commandBuffer==reinterpret_cast<void*>(0x11)&&submission.sourceTexture==reinterpret_cast<void*>(0x22)&&submission.sourceTime==12.125&&submission.lifetimeToken==&tokenMarker&&submission.releaseToken==releaseToken&&releases==1);
 for(const auto&entry:accepted)assert(!entry.observer.retired);
 retirePresentationClock();clockIs(0,0);
 std::puts("PASS actual ART untargeted observed dispatch, real coherent display clock, callback ownership, rejection, stale generation, Stop/reentry and proof path");
}
