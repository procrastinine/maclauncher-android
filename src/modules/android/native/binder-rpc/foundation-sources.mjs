import fs from 'node:fs';
import path from 'node:path';

/** Binder, JNI, Rust and native platform services link the same libbase/liblog. */
export function useSharedFoundation({root,output,sources,foundation,flags}) {
  for(let index=sources.length-1;index>=0;--index)
    if(sources[index].includes('/source/libbase/'))sources.splice(index,1);
  const runtime=sources.find(file=>file===path.join(root,'JniRuntime.cpp'));
  let text=fs.readFileSync(runtime,'utf8');
  const start=text.indexOf('extern "C" int __android_log_write(');
  const end=text.indexOf('// The complete AOSP JNI helpers',start);
  if(start<0||end<start)throw Error('Binder JNI log supplier boundary changed');
  const generated=path.join(output,'JniRuntime-shared.cpp');
  fs.writeFileSync(generated,text.slice(0,start)+text.slice(end));
  sources[sources.indexOf(runtime)]=generated;
  text=fs.readFileSync(path.join(root,'OS_darwin.cpp'),'utf8');
  const log=text.indexOf('LIBBINDER_EXPORTED int __android_log_print(');
  if(log<0)throw Error('Binder OS log supplier boundary changed');
  const os=path.join(output,'OS_darwin-shared.cpp');
  fs.writeFileSync(os,text.slice(0,log));
  flags.unshift('-I'+foundation.property.include);
  sources.push(...foundation.shared.map(unit=>unit.object));
  return os;
}
