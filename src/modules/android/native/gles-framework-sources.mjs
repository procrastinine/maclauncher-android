import fs from 'node:fs';import path from 'node:path';import{execFileSync}from'node:child_process';
import {writeChanged} from './upstream/source-files.mjs';
const git=(repo,args)=>execFileSync('git',['-C',repo,...args],{encoding:'utf8',maxBuffer:32*1024*1024});
export function prepareFrameworkGles(repo,revision,build,owned,nativeRepo,nativeRevision){
 if(git(repo,['rev-parse','HEAD']).trim()!==revision)throw Error('AOSP graphics source does not match its pin');
 const files=git(repo,['ls-tree','-r','--name-only',revision,'core/jni']).trim().split('\n').filter(file=>/\/(android_opengl_\w+|com_google_android_gles_jni_\w+)\.cpp$/.test(file));
 const destination=path.join(build,'framework-gles');fs.mkdirSync(destination,{recursive:true});const units=[],registrations=[],pending=new Map();
 if(!nativeRepo||git(nativeRepo,['rev-parse','HEAD']).trim()!==nativeRevision)throw Error('AOSP native graphics source does not match its pin');
 const utilityFiles=git(repo,['ls-tree','-r','--name-only',revision,'core/jni/android/opengl']).trim().split('\n').filter(file=>/\.(cpp|h)$/.test(file));
 for(const file of utilityFiles){
  const target=path.join(destination,path.basename(file));let source=git(repo,['show',revision+':'+file]);
  if(file.endsWith('.cpp')){
   for(const match of source.matchAll(/\{\s*"(android\/opengl\/[^"]+)"\s*,\s*(\w+)\s*,\s*NELEM\(\2\)\s*\}/g)){
    const registration='register_host_'+match[2];registrations.push({className:match[1],registration,namespace:'android',direct:true});
    source+='\nnamespace android { int '+registration+'(JNIEnv* env,jclass type){return env->RegisterNatives(type,'+match[2]+',NELEM('+match[2]+'));}}\n';
   }
   source=source.replace('#include <android/graphics/bitmap.h>','#include "'+path.join(owned,'framework-gles/bitmap.h')+'"').replaceAll('graphics::Bitmap','HostGraphicsBitmap')
    .replace('#include "core_jni_helpers.h"','#include <memory>\n#include <android_runtime/AndroidRuntime.h>\n#define RegisterMethodsOrDie AndroidRuntime::registerNativeMethods')
    .replace('#include <utils/Log.h>','');
   units.push(target);
  }
  pending.set(target,source);
 }
 const nativeFiles=git(nativeRepo,['ls-tree','-r','--name-only',nativeRevision,'opengl/libs/ETC1','opengl/include/ETC1','include/android/bitmap.h']).trim().split('\n').filter(file=>/\.(cpp|h)$/.test(file));
 for(const file of nativeFiles){
  const relative=file.startsWith('opengl/include/')?file.slice('opengl/include/'.length):file.startsWith('include/')?file.slice('include/'.length):path.basename(file);
  const target=path.join(destination,relative);fs.mkdirSync(path.dirname(target),{recursive:true});pending.set(target,git(nativeRepo,['show',nativeRevision+':'+file]));if(file.endsWith('.cpp'))units.push(target);
 }
 for(const file of files){
  let source=git(repo,['show',revision+':'+file]),name=path.basename(file),registration=source.match(/int\s+(register_\w+)\s*\(JNIEnv\s*\*/)?.[1];
  const className=source.match(/classPathName\s*=\s*"([^"]+)"/)?.[1];
  if(!registration||!className)throw Error('AOSP JNI registration could not be located: '+file);
  registrations.push({className,registration,namespace:''});
  source=source.replace(/^#include <(?:android_runtime\/android_(view_Surface|graphics_SurfaceTexture)\.h|gui\/[^>]+|ui\/ANativeObjectBase\.h|private\/EGL\/display\.h)>\n/gm,'');
  source=source.replace(/(?:android::)?sp<ANativeWindow>/g,'android::HostNativeWindow');
  if(name.includes('EGL14'))source=source.replace('    android::sp<android::IGraphicBufferProducer> producer;','').replace(/    producer = android::SurfaceTexture_getProducer\(_env, win\);[\s\S]*?    window = new android::Surface\(producer, true\);/,'    window = android::android_graphics_SurfaceTexture_getNativeWindow(_env, win);');
  if(name.includes('EGLImpl'))source=source.replace(/    sp<IGraphicBufferProducer> producer\(SurfaceTexture_getProducer\(_env, native_window\)\);\n    window = new Surface\(producer, true\);/,'    window = android_graphics_SurfaceTexture_getNativeWindow(_env, native_window);');
  source=source.replaceAll('reinterpret_cast<EGLNativeDisplayType>(display_id)','(EGLNativeDisplayType)(uintptr_t)display_id').replaceAll('reinterpret_cast<uintptr_t>(EGL_DEFAULT_DISPLAY)','(uintptr_t)EGL_DEFAULT_DISPLAY');
  source=source.replace(/_env->Get(Byte|Boolean|Char|Short|Int|Long|Float|Double)ArrayElements\(/g,'HostArrayAccess::get$1(_env,').replace(/_env->Release(Byte|Boolean|Char|Short|Int|Long|Float|Double)ArrayElements\(/g,'HostArrayAccess::release$1(_env,');
  source=source.replace(/jlong handle = _env->CallLongMethod\(([^;]+)\);/g,'jlong handle = _env->CallLongMethod($1);\n    if (_env->ExceptionCheck()) return nullptr;');
  const target=path.join(destination,name);pending.set(target,source);units.push(target);
 }
 for(const target of units){
  let source=pending.get(target);
  const includes=source.match(/^#include [<"][^\n]+/gm)||[];
  for(const alias of [...source.matchAll(/^#define (gl\w+) (gl\w+)$/gm)]){source=source.replace(alias[0],'').replace(new RegExp('(?<![\"\\w])'+alias[1]+'(?![\"\\w])','g'),alias[2]);}
  const executable=source.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*|"(?:\\.|[^"\\])*"/g,'');
  const symbols=[...new Set([...executable.matchAll(/\b((?:gl|egl)[A-Z]\w*)\s*(?:\(|>)/g)].map(match=>match[1]))].filter(symbol=>!symbol.endsWith('Bounds'));
  const bindings=symbols.map(symbol=>`static constexpr char name_${symbol}[]="${symbol}";using host_${symbol}=HostGraphicsEntry<decltype(&::${symbol}),name_${symbol}>;\n#define ${symbol} host_${symbol}::call`).join('\n');
  pending.set(target,`#define GL_GLEXT_PROTOTYPES 1\n#define EGL_EGLEXT_PROTOTYPES 1\n#include "${path.join(owned,'framework-gles/dispatch.h')}"\n#include "${path.join(owned,'framework-gles/window.h')}"\n#include "${path.join(owned,'framework-gles/array-storage.h')}"\n${includes.join('\n')}\n${bindings}\n${source}`);
 }
 const declarations=registrations.map(({registration,namespace,direct})=>namespace?`namespace ${namespace}{int ${registration}(JNIEnv*${direct?",jclass":""});}`:`int ${registration}(JNIEnv*);`).join('\n');
 const cases=registrations.map(({className,registration,namespace,direct})=>`if(name=="${className.replaceAll('/','.')}" ){${namespace?namespace+'::':''}${registration}(env${direct?",type":""});return;}`).join('\n');
 const registry=path.join(destination,'registry.cpp');pending.set(registry,`#include <jni.h>\n#include <string>\n#include <nativehelper/JNIPlatformHelp.h>\n${declarations}\nextern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostGles_register(JNIEnv*env,jclass,jclass type){jclass clazz=env->FindClass("java/lang/Class");jmethodID method=env->GetMethodID(clazz,"getName","()Ljava/lang/String;");auto value=(jstring)env->CallObjectMethod(type,method);if(env->ExceptionCheck())return;const char*chars=env->GetStringUTFChars(value,nullptr);if(!chars)return;std::string name(chars);env->ReleaseStringUTFChars(value,chars);env->DeleteLocalRef(value);env->DeleteLocalRef(clazz);${cases}\njniThrowException(env,"java/lang/UnsatisfiedLinkError","Graphics JNI class has no upstream registration");}\n`);units.push(registry);
 pending.set(path.join(destination,'registrations.txt'),registrations.map(entry=>entry.className).sort().join('\n')+'\n');
 for(const [target,bytes] of pending)writeChanged(target,bytes);
 return units;
}
