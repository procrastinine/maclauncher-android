import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';

/** Read actual publisher registry entries through its retained Mach-O symbols.
 * The helper runs only during builds. No JNI registration or game code runs.
 */
export function frameworkNativeCatalog(library) {
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'android-native-catalog-'));
  try {
    const helper=path.join(directory,'catalog');
    execFileSync('clang++',['-std=c++20','-O1','-Wall','-Wextra','-Werror',fileURLToPath(new URL('./framework-native-catalog.cpp',import.meta.url)),'-o',helper]);
    const output=execFileSync(helper,[path.resolve(library)],{encoding:'utf8',timeout:30000,maxBuffer:1<<20});
    const catalog={core:[],graphics:[]};
    for(const line of output.trim().split('\n')){
      const [group,key,...extra]=line.split('\t');
      if(!Object.hasOwn(catalog,group)||!key||extra.length)throw Error('Invalid publisher registration entry');
      catalog[group].push(key);
    }
    for(const keys of Object.values(catalog))keys.sort();
    return catalog;
  } finally {fs.rmSync(directory,{recursive:true,force:true});}
}

export function writeFrameworkNativeCatalog(library,destination,declarations){
  const catalog=frameworkNativeCatalog(library);
  const methods=new Map(fs.readFileSync(declarations,'utf8').trim().split('\n').filter(Boolean).map(line=>{
    const [name,original,adapted,...extra]=line.split('\t');
    if(!name||extra.length||!/^\d+$/.test(original)||!/^(-1|\d+)$/.test(adapted))throw Error('Invalid framework native declaration record');
    return [name,{original:Number(original),adapted:Number(adapted)}];
  }));
  const lines=Object.entries(catalog).map(([group,classes])=>group+'='+classes.filter(name=>{
    const supplied=methods.get(name.replaceAll('.','/'));
    // Unknown registry groups still run. Skip only complete native families
    // whose declarations have all become actual platform Java implementations.
    return !supplied||supplied.original===0||supplied.adapted!==0;
  }).join(','));
  fs.writeFileSync(destination,lines.join('\n')+'\n');
  return catalog;
}
