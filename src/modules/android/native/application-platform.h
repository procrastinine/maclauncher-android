#pragma once
#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include <pthread.h>
#include <string>

/* One process application lifecycle for both execution engines. VM adapters
 * supply only the prepared Android label, icon bytes and process role. */
static inline void kl_application_show(){
 [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
}
static inline bool kl_application_start(const char*name,const void*png,size_t bytes,bool headless){@autoreleasepool{
 if(!pthread_main_np())return SDL_SetError("Application services require their main thread");
 [[NSProcessInfo processInfo]setProcessName:[NSString stringWithUTF8String:name]];
 if(!SDL_SetAppMetadata(name,nullptr,nullptr))return false;
 // SDL uses this same policy after didFinishLaunching. Establish it before
 // AppKit's initial open event can ask the OS to restore unrelated Mac state.
 [[NSUserDefaults standardUserDefaults]registerDefaults:@{@"ApplePersistenceIgnoreState":@YES}];
 const char*prior=SDL_GetHint(SDL_HINT_MAC_BACKGROUND_APP);
 std::string background=prior?prior:"";bool hadBackground=prior!=nullptr;
 if(headless)SDL_SetHintWithPriority(SDL_HINT_MAC_BACKGROUND_APP,"1",SDL_HINT_OVERRIDE);
 bool initialized=SDL_InitSubSystem(SDL_INIT_VIDEO);
 std::string error=initialized?"":SDL_GetError();
 if(initialized){SDL_PumpEvents();if(!headless)kl_application_show();}
 if(headless){SDL_ResetHint(SDL_HINT_MAC_BACKGROUND_APP);if(hadBackground)SDL_SetHintWithPriority(SDL_HINT_MAC_BACKGROUND_APP,background.c_str(),SDL_HINT_OVERRIDE);}
 if(!initialized)return SDL_SetError("%s",error.c_str());
 if(png&&bytes){NSImage*image=[[NSImage alloc]initWithData:[NSData dataWithBytes:png length:bytes]];if(image)NSApp.applicationIconImage=image;}
 return true;
}}
static inline void kl_application_stop(){SDL_QuitSubSystem(SDL_INIT_VIDEO);}
