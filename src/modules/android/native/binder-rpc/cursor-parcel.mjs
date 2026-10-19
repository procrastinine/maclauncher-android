import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {publisherFunctions} from '../ndk-graphics/publisher-symbols.mjs';

const definition=JSON.parse(fs.readFileSync(new URL('../framework-binary.json',import.meta.url)));
function method(source,signature){
  const start=source.indexOf(signature),open=source.indexOf('{',start);
  if(start<0||open<0)throw Error('AOSP cursor parcel definition changed: '+signature);
  let depth=1,end=open+1;
  while(depth&&end<source.length){const c=source[end++];if(c==='{')depth++;else if(c==='}')depth--;}
  if(depth)throw Error('AOSP cursor parcel definition is incomplete: '+signature);
  return source.slice(start,end);
}

/** Add the original wire operations to the supplied native CursorWindow.
 * SQLite and Java keep their existing object, allocator and disposal owner.
 * Only the supplied host object's MappedFile layout differs from the device
 * header; the serializers bind to the same real Parcel as Binder RPC. */
export function prepareCursorParcel({framework,publisher,generated}){
  if(crypto.createHash('sha256').update(fs.readFileSync(publisher)).digest('hex')!==definition.nativeRuntime.sha256)
    throw Error('Cursor transport requires the declared native framework image');
  const original=file=>execFileSync('git',['-C',framework,'show',definition.declarationSource.revision+':'+file],{encoding:'utf8',maxBuffer:4<<20});
  const source=original('libs/androidfw/CursorWindow.cpp');
  const jni=original('core/jni/android_database_CursorWindow.cpp');
  let header=original('libs/androidfw/include/androidfw/CursorWindow.h');
  const field='    void* mData = nullptr;';
  if(header.split(field).length!==2)throw Error('AOSP cursor storage declaration changed');
  header=header.replace(field,'    std::unique_ptr<android::base::MappedFile> mMappedFile;\n'+field);
  const directory=path.join(generated,'cursor');fs.mkdirSync(path.join(directory,'androidfw'),{recursive:true});
  fs.writeFileSync(path.join(directory,'androidfw/CursorWindow.h'),'#include <android-base/mapped_file.h>\n'+header);
  let read=method(source,'status_t CursorWindow::createFromParcel(');
  const mapping='        window->mData = ::mmap(nullptr, window->mSize, PROT_READ, MAP_SHARED, tempFd, 0);\n        if (window->mData == MAP_FAILED) {';
  if(read.split(mapping).length!==2)throw Error('AOSP cursor mapping boundary changed');
  read=read.replace(mapping,'        window->mMappedFile = android::base::MappedFile::FromFd(tempFd, 0, window->mSize, PROT_READ);\n        if (!window->mMappedFile) {')
    .replace('        window->mAshmemFd = tempFd;','        window->mData = window->mMappedFile->data();\n        window->mAshmemFd = tempFd;');
  // Reuse the publisher's actual constructor, destructor and slot maintenance.
  // No second cursor registry or second native object implementation is added.
  const available=publisherFunctions(publisher);
  const names=['__ZN7android12CursorWindowC1Ev','__ZN7android12CursorWindowC2Ev','__ZN7android12CursorWindowD1Ev','__ZN7android12CursorWindowD2Ev','__ZN7android12CursorWindow15updateSlotsDataEv'];
  const offsets=names.map(name=>{const offset=available.get(name);if(!Number.isSafeInteger(offset))throw Error('Native cursor dependency is unavailable: '+name);return offset;});
  const assembly=['.section __DATA,__data','.p2align 3','.private_extern _kljCursorTargets','_kljCursorTargets:',`.space ${names.length*8}`,'.text'];
  names.forEach((name,i)=>assembly.push('.p2align 2',`.private_extern ${name}`,`${name}:`,'adrp x16, _kljCursorTargets@PAGE',`add x16, x16, _kljCursorTargets@PAGEOFF`,`ldr x16, [x16, #${i*8}]`,'br x16'));
  const unit=path.join(directory,'CursorWindowParcel.cpp');
  const exports=[['nativeCreateFromParcel','jlong'],['nativeWriteToParcel','void']].map(([name,type])=>method(jni,'static '+type+' '+name+'(').replace('static '+type+' '+name,'extern "C" JNIEXPORT '+type+' JNICALL Java_android_database_CursorWindow_'+name));
  fs.writeFileSync(unit,source.slice(0,source.indexOf('*/')+2)+`
#include "hotspot-jni.h"
#include "core_jni_helpers.h"
#include "android_os_Parcel.h"
#include <androidfw/CursorWindow.h>
#include <android-base/logging.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
extern "C" uintptr_t kljCursorTargets[];
asm(${JSON.stringify(assembly.join('\n')+'\n')});
static void __attribute__((constructor)) bindCursorOwner() {
    Dl_info own{};
    if (!dladdr(reinterpret_cast<void*>(&bindCursorOwner), &own)) return;
    std::string file(own.dli_fname); file.resize(file.find_last_of('/')+1);
    void* publisher=dlopen((file+"libandroid_runtime.dylib").c_str(),RTLD_LAZY|RTLD_LOCAL);
    Dl_info image{};
    if (!publisher || !dladdr(dlsym(publisher,"JNI_OnLoad"),&image)) return;
    constexpr uintptr_t offsets[]={${offsets.join(',')}};
    for (size_t i=0;i<std::size(offsets);i++) kljCursorTargets[i]=reinterpret_cast<uintptr_t>(image.dli_fbase)+offsets[i];
}
namespace android {
${source.slice(source.indexOf('static constexpr const size_t kInlineSize'),source.indexOf('CursorWindow::CursorWindow()'))}
${read}
${method(source,'status_t CursorWindow::writeToParcel(')}
${method(jni,'static int getFdCount()')}
${exports.join('\n')}
}
`);
  return {sources:[unit],include:directory};
}
