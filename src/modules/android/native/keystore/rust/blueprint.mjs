import fs from 'node:fs';
import path from 'node:path';
import {parseBlueprint} from '../../upstream/blueprint.mjs';
import {writeChanged} from '../../upstream/source-files.mjs';

// Cargo replaces Soong only as the build driver. Original module dependencies,
// defaults, source entry points and features continue to define the closure.
export function readModules(roots) {
  const modules=new Map();
  for(const root of new Set(roots))for(const relative of fs.globSync('**/Android.bp',{cwd:root})) {
    const file=path.join(root,relative);
    for(const module of parseBlueprint(fs.readFileSync(file,'utf8'),{file}).modules) {
      const name=module.properties.name;
      if(name&&!modules.has(name))modules.set(name,{...module,directory:path.dirname(file),file});
    }
  }
  return modules;
}

export function properties(modules,name,seen=new Set()) {
  const module=modules.get(name);
  if(!module)throw Error('Missing upstream build declaration: '+name);
  if(seen.has(name))throw Error('Circular upstream defaults: '+name);
  const next=new Set([...seen,name]),result={};
  const merge=value=>{for(const [key,v] of Object.entries(value)){
    if(key==='defaults')continue;
    result[key]=Array.isArray(v)?[...(result[key]||[]),...v]:v;
  }};
  for(const base of module.properties.defaults||[])merge(properties(modules,base,next));
  merge(module.properties);
  // These are Android libraries running on the host boundary, not Soong host
  // utility variants. Preserve every dependency of the original Android target.
  merge(module.properties.target?.android||{});
  return result;
}

export const aidlCrate=name=>name.replace(/(?:-V\d+)?-rust$/,'').replace(/-V\d+$/,'').replaceAll('.','_');
export const write=writeChanged;
export function replace(file,before,after) {
  const text=fs.readFileSync(file,'utf8');
  if(!text.includes(before))throw Error('Upstream native adaptation changed: '+file+' '+before);
  write(file,text.replace(before,after));
}
export function manifest({name,entry='lib.rs',dependencies=[],features=[],kind,buildDependencies=[]}) {
  return `[package]\nname=${JSON.stringify(name)}\nversion="0.1.0"\nedition="2021"\n[lib]\npath=${JSON.stringify(entry)}\n${kind?'crate-type='+JSON.stringify([kind])+'\n':''}`+
    (features.length?'[features]\ndefault='+JSON.stringify(features)+'\n'+features.map(f=>f+'=[]').join('\n')+'\n':'')+
    '[dependencies]\n'+dependencies.join('\n')+'\n'+(buildDependencies.length?'[build-dependencies]\n'+buildDependencies.join('\n')+'\n':'');
}

export function bindgenMethods(flags) {
  let result='';
  const booleans=new Set(['derive-default','derive-debug','generate-inline-functions','enable-function-attribute-detection','use-core','wrap-static-fns']);
  for(let i=0;i<flags.length;i++) {
    const flag=flags[i],equal=flag.indexOf('='),key=(equal<0?flag:flag.slice(0,equal)).replace(/^--/,'');
    if(booleans.has(key)){result+='.'+key.replaceAll('-','_')+'('+(['use-core','enable-function-attribute-detection'].includes(key)?'':'true')+')';continue;}
    const value=equal<0?flags[++i]:flag.slice(equal+1);
    if(value===undefined)throw Error('Missing bindgen option value: '+flag);
    result+='.'+key.replaceAll('-','_')+'('+JSON.stringify(value)+')';
  }
  return result;
}
