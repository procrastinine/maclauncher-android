import fs from'node:fs';import path from'node:path';import{fileURLToPath}from'node:url';import{execFileSync}from'node:child_process';import{createHash}from'node:crypto';
export function buildNativeHal({prepared,wrapper}) {
const root=prepared.output;
const generated=prepared.modules;const out=root+'/native';fs.mkdirSync(out,{recursive:true});
const flags=[...wrapper.flags,'-I'+root+'/overlay',...generated.map(g=>'-I'+g.output+'/include')];
const units=['apc_compat.cpp','km_compat.cpp','km_compat_service.cpp','PlatformHal.cpp'].map(f=>root+'/overlay/'+f).concat(generated.flatMap(g=>fs.globSync('**/*.cpp',{cwd:g.output}).map(f=>g.output+'/'+f)));
const objects=[];
for(const file of units){const object=out+'/'+path.relative(root,file).replaceAll('/','_')+'.o';execFileSync('clang++',[...flags,'-c',file,'-o',object],{stdio:'inherit'});objects.push(object);console.log(path.relative(root,file));}
const archive=out+'/libkeystore_native_hal.a';execFileSync('ar',['rcs',archive,...objects]);
return {archive,objects,flags,source:prepared.overlay};
}
