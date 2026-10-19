#pragma once
#include <cstring>
#include <stdexcept>
#include <string>

// Initialize the external drawable without changing the guest context state.
// GLES 1 exposes framebuffer objects through OES; the core entrypoints require
// GLES 2 even when the driver exports both sets of symbols.
namespace klj_framebuffer {
inline bool separateRead(const char *version){return version&&std::strstr(version,"OpenGL ES 3");}
inline void initialize(void *(*resolve)(const char *)){
    auto function=[&]<class T>(const char *name){
        auto address=resolve(name);
        if(!address)throw std::runtime_error(std::string("Missing GLES function: ")+name);
        return reinterpret_cast<T>(address);
    };
    auto getInteger=function.operator()<void(*)(unsigned,int*)>("glGetIntegerv");
    auto getFloat=function.operator()<void(*)(unsigned,float*)>("glGetFloatv");
    auto getBoolean=function.operator()<void(*)(unsigned,unsigned char*)>("glGetBooleanv");
    auto getString=function.operator()<const unsigned char*(*)(unsigned)>("glGetString");
    const char *version=reinterpret_cast<const char*>(getString(0x1F02));
    const bool es1=version&&(std::strstr(version,"OpenGL ES 1")||std::strstr(version,"OpenGL ES-CM 1")||std::strstr(version,"OpenGL ES-CL 1"));
    auto bind=function.operator()<void(*)(unsigned,unsigned)>(es1?"glBindFramebufferOES":"glBindFramebuffer");
    auto status=function.operator()<unsigned(*)(unsigned)>(es1?"glCheckFramebufferStatusOES":"glCheckFramebufferStatus");
    auto enabled=function.operator()<unsigned char(*)(unsigned)>("glIsEnabled");
    auto enable=function.operator()<void(*)(unsigned)>("glEnable");
    auto disable=function.operator()<void(*)(unsigned)>("glDisable");
    auto colorMask=function.operator()<void(*)(unsigned char,unsigned char,unsigned char,unsigned char)>("glColorMask");
    auto clearColor=function.operator()<void(*)(float,float,float,float)>("glClearColor");
    auto clear=function.operator()<void(*)(unsigned)>("glClear");
    int framebuffer=0,readbuffer=0;float color[4]={};unsigned char mask[4]={};
    getInteger(0x8CA6,&framebuffer);
    if(separateRead(version))getInteger(0x8CAA,&readbuffer);
    getFloat(0x0C22,color);getBoolean(0x0C23,mask);
    const bool scissor=enabled(0x0C11);
    bind(0x8D40,0);const unsigned completeness=status(0x8D40);
    disable(0x0C11);colorMask(1,1,1,1);clearColor(0,0,0,0);clear(0x4000);
    clearColor(color[0],color[1],color[2],color[3]);colorMask(mask[0],mask[1],mask[2],mask[3]);
    if(scissor)enable(0x0C11);
    bind(0x8D40,framebuffer);
    if(separateRead(version))bind(0x8CA8,readbuffer);
    // Do not consume guest errors with glGetError.
    if(completeness!=0x8CD5)throw std::runtime_error("Android IOSurface framebuffer is incomplete");
}
}
