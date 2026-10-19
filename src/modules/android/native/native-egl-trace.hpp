#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// Opt-in metadata only. Disabled lookup returns the original ANGLE pointer;
// no wrapper, state tracking, pixel readback or extra GL query enters rendering.
namespace klj_egl_trace {
using Resolver=void*(*)(const char*);
inline std::atomic<Resolver> resolve;
inline std::atomic<unsigned> records{0};
inline std::atomic<unsigned> shaderFailures{0};
inline std::mutex output;
inline bool enabled(){static const bool value=[] {const char*v=std::getenv("KL_GL_TEXTURE_TRACE");return v&&*v&&std::strcmp(v,"0");}();return value;}
inline bool shadersEnabled(){static const bool value=[] {const char*v=std::getenv("KL_GL_SHADER_TRACE");return v&&*v&&std::strcmp(v,"0");}();return value;}
template<class T>T function(const char*name){return reinterpret_cast<T>(resolve.load(std::memory_order_relaxed)(name));}
inline void record(const char*name,unsigned target,const char*format,...){
 unsigned sequence=records.fetch_add(1,std::memory_order_relaxed);if(sequence>=8192)return;
 unsigned binding=target==0x0DE1?0x8069:target==0x806F?0x806A:target==0x8C1A?0x8C1D:target==0x8513||(target>=0x8515&&target<=0x851A)?0x8514:0;
 int texture=0;if(binding)function<void(*)(unsigned,int*)>("glGetIntegerv")(binding,&texture);
 auto context=function<void*(*)()>("eglGetCurrentContext")();
 std::lock_guard lock(output);std::fprintf(stderr,"[android-gl-texture] event=%u context=%p op=%s target=0x%x texture=%d ",sequence,context,name,target,texture);
 va_list values;va_start(values,format);std::vfprintf(stderr,format,values);va_end(values);std::fputc('\n',stderr);
 if(sequence==8191)std::fputs("[android-gl-texture] metadata limit reached\n",stderr);
}
inline void image2(unsigned target,int level,int internal,int width,int height,int border,unsigned format,unsigned type,const void*data){
 record("glTexImage2D",target,"level=%d format=0x%x pixels=%dx%d source=0x%x type=0x%x data=%d",level,internal,width,height,format,type,data!=nullptr);
 function<decltype(&image2)>("glTexImage2D")(target,level,internal,width,height,border,format,type,data);
}
inline void sub2(unsigned target,int level,int x,int y,int width,int height,unsigned format,unsigned type,const void*data){
 record("glTexSubImage2D",target,"level=%d offset=%d,%d pixels=%dx%d source=0x%x type=0x%x",level,x,y,width,height,format,type);
 function<decltype(&sub2)>("glTexSubImage2D")(target,level,x,y,width,height,format,type,data);
}
inline void compressed2(unsigned target,int level,unsigned internal,int width,int height,int border,int size,const void*data){
 record("glCompressedTexImage2D",target,"level=%d format=0x%x pixels=%dx%d bytes=%d",level,internal,width,height,size);
 function<decltype(&compressed2)>("glCompressedTexImage2D")(target,level,internal,width,height,border,size,data);
}
inline void compressedSub2(unsigned target,int level,int x,int y,int width,int height,unsigned format,int size,const void*data){
 record("glCompressedTexSubImage2D",target,"level=%d offset=%d,%d pixels=%dx%d format=0x%x bytes=%d",level,x,y,width,height,format,size);
 function<decltype(&compressedSub2)>("glCompressedTexSubImage2D")(target,level,x,y,width,height,format,size,data);
}
inline void storage2(unsigned target,int levels,unsigned internal,int width,int height){
 record("glTexStorage2D",target,"levels=%d format=0x%x pixels=%dx%d",levels,internal,width,height);
 function<decltype(&storage2)>("glTexStorage2D")(target,levels,internal,width,height);
}
inline void parameteri(unsigned target,unsigned name,int value){record("glTexParameteri",target,"parameter=0x%x value=%d",name,value);function<decltype(&parameteri)>("glTexParameteri")(target,name,value);}
inline void parameterf(unsigned target,unsigned name,float value){record("glTexParameterf",target,"parameter=0x%x value=%g",name,(double)value);function<decltype(&parameterf)>("glTexParameterf")(target,name,value);}
inline void parameteriv(unsigned target,unsigned name,const int*value){record("glTexParameteriv",target,"parameter=0x%x first=%d",name,value?*value:0);function<decltype(&parameteriv)>("glTexParameteriv")(target,name,value);}
inline void parameterfv(unsigned target,unsigned name,const float*value){record("glTexParameterfv",target,"parameter=0x%x first=%g",name,value?(double)*value:0);function<decltype(&parameterfv)>("glTexParameterfv")(target,name,value);}
inline void sampleri(unsigned sampler,unsigned name,int value){record("glSamplerParameteri",0,"sampler=%u parameter=0x%x value=%d",sampler,name,value);function<decltype(&sampleri)>("glSamplerParameteri")(sampler,name,value);}
inline void samplerf(unsigned sampler,unsigned name,float value){record("glSamplerParameterf",0,"sampler=%u parameter=0x%x value=%g",sampler,name,(double)value);function<decltype(&samplerf)>("glSamplerParameterf")(sampler,name,value);}
inline void mipmap(unsigned target){record("glGenerateMipmap",target,"");function<decltype(&mipmap)>("glGenerateMipmap")(target);}
inline void integer(unsigned name,int*value){function<decltype(&integer)>("glGetIntegerv")(name,value);record("glGetIntegerv",0,"parameter=0x%x first=%d",name,value?*value:0);}
// Inspect the original compiler result without consuming GL errors or replacing
// an application's debug callback. Sources stay in ANGLE; only failed objects
// produce bounded diagnostics, including shaders compiled entirely by NDK code.
inline void shaderFailure(unsigned object,bool program){
 auto valid=function<unsigned char(*)(unsigned)>(program?"glIsProgram":"glIsShader");
 if(!valid(object))return;
 auto query=function<void(*)(unsigned,unsigned,int*)>(program?"glGetProgramiv":"glGetShaderiv");
 int status=0;query(object,program?0x8B82:0x8B81,&status);if(status)return;
 if(shaderFailures.fetch_add(1,std::memory_order_relaxed)>=16)return;
 char log[8192]={0},source[4096]={0};
 function<void(*)(unsigned,int,int*,char*)>(program?"glGetProgramInfoLog":"glGetShaderInfoLog")(object,sizeof log,nullptr,log);
 if(!program)function<void(*)(unsigned,int,int*,char*)>("glGetShaderSource")(object,sizeof source,nullptr,source);
 auto context=function<void*(*)()>("eglGetCurrentContext")();
 std::lock_guard lock(output);
 std::fprintf(stderr,"[android-gl-shader] context=%p op=%s object=%u log=%s source=%s\n",context,program?"glLinkProgram":"glCompileShader",object,log,source);
}
inline void compile(unsigned shader){function<void(*)(unsigned)>("glCompileShader")(shader);shaderFailure(shader,false);}
inline void link(unsigned program){function<void(*)(unsigned)>("glLinkProgram")(program);shaderFailure(program,true);}
inline void *lookup(const char*name,Resolver provider){
 const bool textures=enabled(),shaders=shadersEnabled();if(!textures&&!shaders)return nullptr;resolve.store(provider,std::memory_order_relaxed);
 if(shaders&&(!std::strcmp(name,"glCompileShader")||!std::strcmp(name,"glLinkProgram")))return provider(name)?(!std::strcmp(name,"glCompileShader")?(void*)compile:(void*)link):nullptr;
 if(!textures)return nullptr;
 struct Entry{const char*name;void*function;};const Entry entries[]={
 {"glTexImage2D",(void*)image2},{"glTexSubImage2D",(void*)sub2},{"glCompressedTexImage2D",(void*)compressed2},{"glCompressedTexSubImage2D",(void*)compressedSub2},
 {"glTexStorage2D",(void*)storage2},{"glTexParameteri",(void*)parameteri},{"glTexParameterf",(void*)parameterf},{"glTexParameteriv",(void*)parameteriv},{"glTexParameterfv",(void*)parameterfv},
 {"glSamplerParameteri",(void*)sampleri},{"glSamplerParameterf",(void*)samplerf},{"glGenerateMipmap",(void*)mipmap},{"glGetIntegerv",(void*)integer}};
 for(const auto&entry:entries)if(!std::strcmp(name,entry.name))return provider(name)?entry.function:nullptr;
 return nullptr;
}
}
