#pragma once
#import <Metal/Metal.h>
#include <cstdint>
#include <stdexcept>
extern "C" void *klj_graphics_symbol(const char*);
/** The existing ANGLE provider's device and GPU-only shared-event boundaries. */
namespace klj_angle {
using Resolver=void*(*)(const char*);
inline void* sharedSync(void*display,id<MTLSharedEvent> event,uint64_t value,bool external,Resolver resolve){
 const intptr_t attributes[]={0x34D9,(intptr_t)(__bridge void*)event,0x34DA,(intptr_t)(uint32_t)value,0x34DB,(intptr_t)(uint32_t)(value>>32),0x30F8,external?0x34DC:0x30F0,0x3038};
 auto create=reinterpret_cast<void*(*)(void*,unsigned,const intptr_t*)>(resolve("eglCreateSync"));
 return create?create(display,0x34D8,attributes):nullptr;
}
template<class T>T symbol(const char*name){auto result=klj_graphics_symbol(name);if(!result)throw std::runtime_error("ANGLE Metal interoperability function is unavailable");return reinterpret_cast<T>(result);}
inline id<MTLDevice> device(void*display){
 intptr_t eglDevice=0,metalDevice=0;
 if(!symbol<unsigned(*)(void*,int,intptr_t*)>("eglQueryDisplayAttribEXT")(display,0x322C,&eglDevice)||!symbol<unsigned(*)(void*,int,intptr_t*)>("eglQueryDeviceAttribEXT")((void*)eglDevice,0x34A6,&metalDevice)||!metalDevice)throw std::runtime_error("ANGLE Metal device is unavailable");
 return (__bridge id<MTLDevice>)(void*)metalDevice;
}
inline void* sharedSync(void*display,id<MTLSharedEvent> event,uint64_t value,bool external){
 return sharedSync(display,event,value,external,klj_graphics_symbol);
}
}
