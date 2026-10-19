#import <AppKit/AppKit.h>
#include "application-sleep.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <new>

namespace {
std::once_flag registered;
Uint32 eventType;
struct Subscription {SDL_WindowID window;std::atomic<bool> active{true};};
void deliver(const std::shared_ptr<Subscription>& state,bool asleep){
 if(!state->active.load(std::memory_order_acquire))return;
 SDL_Event event{};event.type=eventType;event.user.windowID=state->window;event.user.code=asleep;
 SDL_PushEvent(&event);
}
}
struct KLApplicationSleep {
 std::shared_ptr<Subscription> state;
 __strong id sleep,wake;
};
extern "C" KLApplicationSleep *kl_application_sleep_observe(SDL_WindowID window){@autoreleasepool{
 if(!window)return nullptr;
 std::call_once(registered,[]{eventType=SDL_RegisterEvents(1);});if(!eventType)return nullptr;
 auto observer=std::unique_ptr<KLApplicationSleep>(new(std::nothrow)KLApplicationSleep);if(!observer)return nullptr;
 observer->state=std::make_shared<Subscription>();observer->state->window=window;auto state=observer->state;
 NSNotificationCenter *center=NSWorkspace.sharedWorkspace.notificationCenter;
 observer->sleep=[center addObserverForName:NSWorkspaceWillSleepNotification object:nil queue:nil usingBlock:^(NSNotification*){deliver(state,true);}];
 observer->wake=[center addObserverForName:NSWorkspaceDidWakeNotification object:nil queue:nil usingBlock:^(NSNotification*){deliver(state,false);}];
 return observer.release();
}}
extern "C" void kl_application_sleep_retire(KLApplicationSleep *observer){@autoreleasepool{
 if(!observer)return;observer->state->active.store(false,std::memory_order_release);
 NSNotificationCenter *center=NSWorkspace.sharedWorkspace.notificationCenter;
 [center removeObserver:observer->sleep];[center removeObserver:observer->wake];delete observer;
}}
extern "C" int kl_application_sleep_event(const SDL_Event *event,SDL_WindowID window,int *sleeping){
 if(!event||!eventType||event->type!=eventType||event->user.windowID!=window)return 0;
 if(sleeping)*sleeping=event->user.code!=0;return 1;
}
