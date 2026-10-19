import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';

const git=(repo,args)=>execFileSync('git',['-C',repo,...args],{encoding:'utf8',maxBuffer:32<<20});
function body(source,signature){
  const start=source.indexOf(signature);if(start<0)throw Error('Original input definition is absent: '+signature);
  const brace=source.indexOf('{',start);let depth=1,end=brace+1;
  while(depth&&end<source.length){const c=source[end++];if(c==='{')depth++;else if(c==='}')depth--;}
  if(depth)throw Error('Original input definition is incomplete: '+signature);
  return source.slice(start,end);
}

/** Compile the original native MotionEvent wire format with the real Parcel owner. */
export function prepareMotionParcel({framework,native,pins,generated}){
  const include=path.join(generated,'motion-include');fs.mkdirSync(include,{recursive:true});
  const revision=pins.aospNative;
  const files=new Set(git(native,['ls-tree','-r','--name-only',revision]).trim().split('\n'));
  const roots=['include','libs/ui/include','libs/math/include','libs/ftl/include','libs/nativewindow/include'];
  const seen=new Set();
  function header(name){
    if(seen.has(name))return;seen.add(name);
    let input=roots.map(root=>root+'/'+name).find(file=>files.has(file))||[...files].find(file=>file.endsWith('/include/'+name));if(!input)return;
    let text=git(native,['show',revision+':'+input]);
    while(/^\.\.?\//.test(text.trim())&&!text.includes('\n')){input=path.posix.normalize(path.posix.join(path.posix.dirname(input),text));text=git(native,['show',revision+':'+input]);}
    if(name==='input/Input.h'){
      // The complete declarations through MotionEvent retain their original
      // layout, inline methods and constants. Later unrelated event types are
      // not part of this native translation unit.
      const end=text.indexOf('\nstd::ostream& operator<<(std::ostream& out, const MotionEvent& event);');
      if(end<0)throw Error('Original MotionEvent declaration is incomplete');
      text=text.slice(0,end)+'\n}\n';
      text=text.replace('#ifdef __linux__\n#include <android/os/IInputConstants.h>\n#include <android/os/MotionEventFlag.h>\n#endif','#include <android/os/IInputConstants.h>\n#include <android/os/MotionEventFlag.h>');
      text=text.replace('#include <android/os/PointerIconType.h>','');
    }
    const target=path.join(include,name);fs.mkdirSync(path.dirname(target),{recursive:true});fs.writeFileSync(target,text);
    for(const match of text.matchAll(/^\s*#\s*include\s*[<"]([^>"\n]+)[>"]/gm))header(match[1]);
  }
  header('input/Input.h');
  for(const name of ['IInputConstants','MotionEventFlag']){
    const text=git(native,['show',revision+':libs/input/android/os/'+name+'.aidl']).replace(/\/\*[\s\S]*?\*\//g,'').replace(/\/\/[^\n]*/g,'');
    const values=name==='IInputConstants'?[...text.matchAll(/const int (\w+)\s*=\s*([^;]+);/g)].map(m=>'static constexpr int32_t '+m[1]+' = static_cast<int32_t>('+m[2]+');').join('\n'):
      text.slice(text.indexOf('{')+1,text.lastIndexOf('}')).replace(/IInputConstants\./g,'IInputConstants::');
    const declaration=name==='IInputConstants'?'struct '+name+' {\n'+values+'\n};':'enum class '+name+' : int32_t {\n'+values+'\n};';
    const file=path.join(include,'android/os',name+'.h');fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,'#pragma once\n#include <cstdint>\nnamespace android::os {\n'+declaration+'\n}\n');
  }
  const input=git(native,['show',revision+':libs/input/Input.cpp']);
  const transform=git(native,['show',revision+':libs/ui/Transform.cpp']);
  const jni=git(framework,['show',pins.aospFramework+':core/jni/android_view_MotionEvent.cpp']);
  const methods=['status_t PointerCoords::readFromParcel(','status_t PointerCoords::writeToParcel(','static status_t readFromParcel(ui::Transform&','static status_t writeToParcel(const ui::Transform&','status_t MotionEvent::readFromParcel(','status_t MotionEvent::writeToParcel('].map(name=>body(input,name));
  const transforms=['Transform::Transform()','void Transform::reset()','bool Transform::isZero(','bool Transform::absIsOne(','uint32_t Transform::type()','void Transform::set(const std::array<float, 9>&','float Transform::tx()','float Transform::ty()','float Transform::dsdx()','float Transform::dtdx()','float Transform::dtdy()','float Transform::dsdy()'].map(name=>body(transform,name));
  const file=path.join(generated,'MotionEventParcel.cpp');
  // Preserve the original JNI body, changing only its exported native owner.
  const read=body(jni,'static jlong android_view_MotionEvent_nativeReadFromParcel').replace('static jlong android_view_MotionEvent_nativeReadFromParcel','extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostMotionEvent_nativeReadFromParcel');
  const write=body(jni,'static void android_view_MotionEvent_nativeWriteToParcel').replace('static void android_view_MotionEvent_nativeWriteToParcel','extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostMotionEvent_nativeWriteToParcel');
  fs.writeFileSync(file,input.slice(0,input.indexOf('*/')+2)+'\n#include "hotspot-jni.h"\n#include "core_jni_helpers.h"\n#include "android_os_Parcel.h"\n#include <binder/Parcel.h>\n#include <optional>\n#include <input/Input.h>\nnamespace android {\n'+methods.join('\n')+'\n'+read+'\n'+write+'\n}\nnamespace android::ui {\nstatic const float EPSILON = 0.0f;\nTransform::~Transform() = default;\n'+transforms.join('\n')+'\n}\n');
  return {file,include};
}
