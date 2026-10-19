import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../upstream/source-files.mjs';
import {readModuleList} from '../upstream/blueprint.mjs';

/** Original fence ownership, serialization, waits and JNI, beside Parcel's
 * existing descriptor owner. Linux sync-file ioctls retain their original ABI. */
export function prepareSyncFence({pins,source,output,inputs,exportSources,sources,flags,ioctlTokens}) {
  const framework=path.join(source,'framework'),native=path.join(source,'native'),core=path.join(source,'core');
  const jni='core/jni/android_hardware_SyncFence.cpp';
  exportSources(inputs.framework,pins.aospFramework,[jni],framework);
  exportSources(inputs.native,pins.aospNative,['libs/ui/Fence.cpp','libs/ui/include/ui/Fence.h'],native);
  exportSources(inputs.core,pins.aospSystemCore,['libsync'],core);
  const original=fs.readFileSync(path.join(framework,jni),'utf8');
  const methods=[...original.matchAll(/\{\s*"(\w+)"\s*,\s*"([^"]+)"\s*,\s*\(void\*\)\s*(\w+)\s*\}/g)];
  if(!methods.length)throw Error('Original SyncFence native registration is absent');
  let text=original;
  // Export every original table target without changing its arguments/body or
  // initializing SyncFence while its native library is still being loaded.
  for(const [,name,,target] of methods){
    const entry='Java_android_hardware_SyncFence_'+name;
    text=text.replace(new RegExp('\\b'+target+'\\b','g'),entry);
    const definition=new RegExp('\\bstatic (\\w+) '+entry+'\\(');
    if(!definition.test(text))throw Error('Original SyncFence native target changed: '+target);
    text=text.replace(definition,'extern "C" JNIEXPORT $1 JNICALL '+entry+'(');
  }
  const generated=path.join(output,'jni/android_hardware_SyncFence.cpp');
  writeChanged(generated,'#include "hotspot-jni.h"\n'+text);
  sources.push(generated,path.join(native,'libs/ui/Fence.cpp'));
  flags.push('-I'+path.join(native,'libs/ui/include'),'-I'+path.join(core,'libsync/include'));
  const header=path.join(core,'libsync/include/ndk/sync.h');
  writeChanged(header,fs.readFileSync(header,'utf8').replace(/\s*__INTRODUCED_IN\(\d+\)/g,''));
  const units=readModuleList(path.join(core,'libsync'),'libsync_defaults','srcs');
  const cSources=units.map(name=>{
    const original=fs.readFileSync(path.join(core,'libsync',name),'utf8');
    // Darwin exposes the standard allocator declarations through stdlib.h.
    const file=path.join(output,'sync',name);
    writeChanged(file,original.replace('#include <malloc.h>','#include <stdlib.h>').replace(ioctlTokens,name=>'KLJ_LINUX_'+name));
    return file;
  });
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  writeChanged(path.join(notices,'sync-fence.json'),JSON.stringify({
    framework:{revision:pins.aospFramework,unit:jni,methods:methods.map(([,name,descriptor])=>({name,descriptor}))},
    native:{revision:pins.aospNative,units:['libs/ui/Fence.cpp','libs/ui/include/ui/Fence.h']},
    core:{revision:pins.aospSystemCore,module:'libsync_defaults',units},
    adaptations:['Ordinary JNI exports from the complete original registration table','Darwin allocator header and availability annotations','Shared Linux ioctl declaration namespace']
  },null,2)+'\n');
  return cSources;
}
