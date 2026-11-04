import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';

function source(checkout,revision,file){
  const actual=execFileSync('git',['-C',checkout,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
  if(actual!==revision)throw Error('KeyCharacterMap source does not match source-pins.json: '+checkout);
  return execFileSync('git',['-C',checkout,'show',revision+':'+file],{encoding:'utf8',maxBuffer:4<<20});
}
function method(text,signature){
  const start=text.indexOf(signature);
  if(start<0||text.indexOf(signature,start+signature.length)>=0)throw Error('AOSP KeyCharacterMap method changed: '+signature);
  const open=text.indexOf('{',start);let depth=1,end=open+1;
  // These pinned methods contain no braces in string literals or comments.
  for(;end<text.length&&depth;end++){if(text[end]==='{')depth++;else if(text[end]==='}')depth--;}
  if(open<0||depth)throw Error('AOSP KeyCharacterMap body is incomplete: '+signature);
  return text.slice(start,end);
}

/** Compile original serializers against the actual Parcel owner. The publisher
 * KeyCharacterMap remains the owner of parsing, lookup, events and disposal. */
export function prepareKeyMapParcel({framework,native,pins,generated}){
  const input=source(native,pins.aospNative,'libs/input/KeyCharacterMap.cpp');
  const jni=source(framework,pins.aospFramework,'core/jni/android_view_KeyCharacterMap.cpp');
  const preamble=input.slice(0,input.indexOf('*/')+2);
  const bodies=[
    method(input,'KeyCharacterMap::KeyCharacterMap(const std::string& filename)'),
    method(input,'std::unique_ptr<KeyCharacterMap> KeyCharacterMap::readFromParcel(Parcel* parcel)'),
    method(input,'void KeyCharacterMap::writeToParcel(Parcel* parcel) const'),
    method(jni,'static jlong nativeReadFromParcel(JNIEnv *env, jobject clazz, jobject parcelObj)'),
    method(jni,'static void nativeWriteToParcel(JNIEnv* env, jobject clazz, jlong ptr, jobject parcelObj)'),
  ];
  const output=path.join(generated,'KeyCharacterMapParcel.cpp');
  fs.mkdirSync(generated,{recursive:true});
  fs.writeFileSync(output,`${preamble}
// Original pinned native bodies, with the complete existing KeyCharacterMap
// layout. Parcel operations bind to this library's real Binder implementation.
#include "hotspot-jni.h"
#include "android_os_Parcel.h"
#include <binder/Parcel.h>
#include <utils/String8.h>
#include <log/log.h>
#include "NativeKeyCharacterMap.h"

namespace android {
${bodies.join('\n\n')}
}

extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostKeyCharacterMap_nativeReadFromParcel(JNIEnv* env,jclass type,jobject parcel) {
    return android::nativeReadFromParcel(env,type,parcel);
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostKeyCharacterMap_nativeWriteToParcel(JNIEnv* env,jclass type,jlong pointer,jobject parcel) {
    android::nativeWriteToParcel(env,type,pointer,parcel);
}
`);
  return output;
}
