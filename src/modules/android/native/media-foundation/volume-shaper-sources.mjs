import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';

/** Original envelope/interpolation and JNI conversion share the existing media
 * and Binder owners. Serializer inputs follow the headers' actual AIDL closure. */
export function prepareVolumeShaper({avSource,frameworkSource,pins,source,output,aidl}){
  exportSources(avSource,pins.aospAv,['media/aidl'],source);
  exportSources(frameworkSource,pins.aospFramework,['media/jni/android_media_VolumeShaper.h'],source);
  const generated=path.join(output,'volume-shaper');fs.mkdirSync(generated,{recursive:true});
  const pending=['include/media/VolumeShaper.h','include/media/Interpolator.h'].flatMap(name=>[...fs.readFileSync(path.join(source,name),'utf8').matchAll(/#include <(android\/media\/[^>]+)\.h>/g)].map(match=>match[1]));
  const inputs=new Set();
  for(let index=0;index<pending.length;index++){
    const name=pending[index];if(inputs.has(name))continue;inputs.add(name);
    const text=fs.readFileSync(path.join(source,'media/aidl',name+'.aidl'),'utf8');
    for(const match of text.matchAll(/\bimport\s+(android\.media\.[\w.]+)\s*;/g))pending.push(match[1].replaceAll('.','/'));
  }
  execFileSync(aidl,['--lang=cpp','-I'+path.join(source,'media/aidl'),'--out='+generated,'--header_out='+generated,...[...inputs].sort().map(name=>path.join(source,'media/aidl',name+'.aidl'))],{stdio:'inherit'});
  return {include:generated,jni:path.join(source,'media/jni'),units:[...inputs].sort().map(name=>path.join(generated,name+'.cpp')).filter(file=>fs.existsSync(file))};
}
