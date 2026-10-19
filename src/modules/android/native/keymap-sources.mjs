import fs from'node:fs';import path from'node:path';import{execFileSync}from'node:child_process';
const git=(repo,args)=>execFileSync('git',['-C',repo,...args],{encoding:'utf8',maxBuffer:16<<20});
export function prepareKeyMapHeaders({framework,native,base,fmt,pins,build}){
 for(const[key,repo]of[['aospFramework',framework],['aospNative',native],['aospLibbase',base],['aospFmt',fmt]])if(git(repo,['rev-parse','HEAD']).trim()!==pins[key])throw Error(key+' does not match source-pins.json');
 const out=path.join(build,'keymap');fs.mkdirSync(path.join(out,'android-base'),{recursive:true});fs.mkdirSync(path.join(out,'fmt'),{recursive:true});
 for(const name of ['result.h','expected.h','errors.h','format.h'])fs.writeFileSync(path.join(out,'android-base',name),git(base,['show',pins.aospLibbase+':include/android-base/'+name]));
 for(const name of git(fmt,['ls-tree','-r','--name-only',pins.aospFmt,'include/fmt']).trim().split('\n'))if(name.endsWith('.h'))fs.writeFileSync(path.join(out,name.slice(8)),git(fmt,['show',pins.aospFmt+':'+name]));
 let header=git(native,['show',pins.aospNative+':include/input/KeyCharacterMap.h']);
 // JNI needs the unmodified native object layout and inline copy constructor;
 // Android-only parameter types are declarations, not linked implementations.
 header=header.replace(/^#include <(?:binder\/IBinder.h|input\/Input.h|utils\/(?:Errors|Tokenizer|Unicode).h)>\n/gm,'');
 fs.writeFileSync(path.join(out,'KeyCharacterMap.h'),'#include <memory>\n#include <vector>\n#include <cstdint>\nnamespace android {class Parcel;class Tokenizer;class KeyEvent;template<class T>class Vector;using status_t=int32_t;using nsecs_t=int64_t;}\n'+header);
 const source=git(framework,['show',pins.aospFramework+':core/jni/android_view_KeyCharacterMap.cpp']);
 const first=source.indexOf('class NativeKeyCharacterMap {'),end=source.indexOf('\n};',first);if(first<0||end<0)throw Error('Native key-map storage definition is absent');
 fs.writeFileSync(path.join(out,'NativeKeyCharacterMap.h'),'#include "KeyCharacterMap.h"\nnamespace android {\n'+source.slice(first,end+3)+'\n}\n');return out;
}
