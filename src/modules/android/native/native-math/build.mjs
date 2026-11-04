import fs from 'node:fs';
import path from 'node:path';
import {execFileSync,spawnSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {parseBlueprint} from '../upstream/blueprint.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';

const root=import.meta.dirname;
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));
const baseFlags=['-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-fno-builtin'];
const annotations='#define __USE_GNU 1\n#define __USE_BSD 1\n#define __BIONIC_AVAILABILITY_GUARD(x) 1\n#define __INTRODUCED_IN(x)\n#define __attribute_const__ __attribute__((const))\n';
function run(command,args){const result=spawnSync(command,args,{encoding:'utf8',maxBuffer:32*1024*1024});if(result.status!==0)throw Error(result.stderr||result.error?.message||command+' failed');return result.stdout;}

/** The original architecture map and public declarations define the complete
 * namespace. Platform providers implement that namespace; game imports never
 * determine which APIs are included. */
export function prepareNativeMath({bionicSource,klepton,build,python,shim}){
  const output=path.join(build,'native-math'),source=path.join(output,'source');
  const cflags=[...baseFlags,...nativePathFlags({source,build})];
  exportSources(bionicSource,pins.aospBionic,['libm','libc/include/math.h','libc/include/complex.h','libc/include/fenv.h','libc/include/bits/fenv_arm.h','tools/generate-version-script.py'],source);
  const include=path.join(source,'libc/include'),libm=path.join(source,'libm');
  const map=path.join(output,'libm.map');
  execFileSync(python,[path.join(source,'tools/generate-version-script.py'),'arm64',path.join(libm,'libm.map.txt'),map]);
  const names=[...fs.readFileSync(map,'utf8').matchAll(/^\s*(\w+);/gm)].map(match=>match[1]);
  const signbit=fs.readFileSync(path.join(libm,'signbit.cpp'),'utf8').replaceAll('extern "C" ','');
  const api=path.join(output,'api.c');
  fs.writeFileSync(api,annotations+'#include <math.h>\n#include <complex.h>\n#include <fenv.h>\n'+signbit);
  const declarations={};
  function visit(node){if(node.kind==='FunctionDecl'||node.kind==='VarDecl')declarations[node.name]={type:node.type.qualType,kind:node.kind};for(const child of node.inner||[])visit(child);}
  visit(JSON.parse(run('clang',['-std=c11','-I'+include,'-fsyntax-only','-Xclang','-ast-dump=json',api])));
  const probe=path.join(output,'host-symbols.c'),probeBinary=path.join(output,'host-symbols');
  fs.writeFileSync(probe,'#include <stdio.h>\n#include <dlfcn.h>\nint main(void){\n'+names.map(name=>`printf("${name} %d\\n",dlsym(RTLD_DEFAULT,"${name}")!=0);`).join('\n')+'\n}\n');
  run('clang',[...cflags,probe,'-o',probeBinary]);
  const available=new Set(run(probeBinary,[]).trim().split('\n').filter(line=>line.endsWith(' 1')).map(line=>line.split(' ')[0]));
  const fenv=fs.readFileSync(path.join(libm,'fenv-arm64.c'),'utf8');
  const environment=new Set([...fenv.matchAll(/^int\s+(\w+)\s*\(/gm)].map(match=>match[1]));environment.add('__fe_dfl_env');
  const supplied=new Map([...fs.readFileSync(path.join(klepton,'runtime/libc/kl_shim.c'),'utf8').matchAll(/E\("(\w+)",\s*(\w+)\)/g)].map(match=>[match[1],match[2]]));
  const entries=names.map(name=>{
    const declaration=declarations[name];if(!declaration)throw Error('Original libm declaration missing: '+name);
    const owner=declaration.type.includes('long double')?'binary128':environment.has(name)?'bionic-fenv':supplied.has(name)?'klepton':available.has(name)?'darwin':'bionic';
    return {name,...declaration,owner};
  });
  const blueprint=path.join(libm,'Android.bp');
  const module=parseBlueprint(fs.readFileSync(blueprint,'utf8'),{file:blueprint}).modules.find(module=>module.type==='cc_library'&&module.properties.name==='libm').properties;
  const excluded=new Set(module.arch.arm64.exclude_srcs);
  const units=[...module.srcs,...module.multilib.lib64.srcs,...module.arch.arm64.srcs].filter(file=>/\.(c|cpp)$/.test(file)&&!excluded.has(file));
  const definitions=new Map();
  for(const file of units){const text=fs.readFileSync(path.join(libm,file),'utf8').replaceAll('extern "C" ','').replace(/\/\*[\s\S]*?\*\//g,'').replace(/^\s*#.*$/gm,'');for(const entry of entries){
    if(!new RegExp('(?:^|\\n)[ \\t]*(?:[\\w*]+[ \\t]+)*'+entry.name+'\\s*\\([^;{}]*\\)\\s*\\{','m').test(text))continue;
    const found=definitions.get(entry.name)||[];found.push(file);definitions.set(entry.name,found);
  }}
  const selected=new Set(['fenv-arm64.c']);
  for(const entry of entries)if(entry.owner==='bionic'){
    const found=definitions.get(entry.name)||[];
    if(found.length!==1)throw Error('Original libm definition is ambiguous: '+entry.name+' '+found);
    selected.add(found[0]);
  }
  // Original implementation units own every function they define together.
  for(const entry of entries)if(entry.owner!=='binary128'&&entry.owner!=='bionic-fenv'&&(definitions.get(entry.name)||[]).some(file=>selected.has(file)))entry.owner='bionic';
  const namespaces=path.join(output,'namespace.h');
  fs.writeFileSync(namespaces,annotations+entries.filter(entry=>entry.owner==='bionic'||entry.owner==='bionic-fenv'||entry.owner==='klepton'||(definitions.get(entry.name)||[]).some(file=>selected.has(file))).map(entry=>`#define ${entry.name} ${entry.owner==='klepton'?supplied.get(entry.name):'klj_math_'+entry.name}`).join('\n')+'\n');
  const compatibility=path.join(output,'compatibility.h');
  // Bionic's BSD source calls the legacy libc function rather than its macro.
  // Keep that dependency with Klepton's existing libc implementation.
  fs.writeFileSync(compatibility,'#include <stdint.h>\n#include <sys/types.h>\n#include <math.h>\nextern int klb_isnanf(float);\n#define isnanf klb_isnanf\n');
  const objects=[];
  for(const file of selected){
    let input=path.join(libm,file);
    if(file.endsWith('.cpp')){input=path.join(output,path.basename(file)+'.c');fs.writeFileSync(input,fs.readFileSync(path.join(libm,file),'utf8').replaceAll('extern "C" ',''));}
    const object=path.join(output,path.basename(file)+'.o');
    run('clang',[...cflags,'-std=c11','-Wno-static-in-inline','-I'+include,'-I'+libm,'-I'+path.join(libm,'upstream-freebsd/lib/msun/src'),'-include',namespaces,'-include',compatibility,'-c',input,'-o',object]);
    objects.push(object);
  }
  const lookup=path.join(output,'lookup.c');
  const bound=entries.filter(entry=>entry.owner!=='binary128');
  fs.writeFileSync(lookup,'#include <stddef.h>\n#include <string.h>\n'+bound.map(entry=>{
    const symbol=entry.owner==='darwin'?entry.name:entry.owner==='klepton'?supplied.get(entry.name):'klj_math_'+entry.name;
    return `extern ${entry.kind==='VarDecl'?'char':'void'} klj_math_binding_${entry.name}${entry.kind==='VarDecl'?'':'(void)'} __asm__("_${symbol}");`;
  }).join('\n')+'\nvoid *klj_math_lookup(const char *name){\n'+bound.map(entry=>` if(!strcmp(name,"${entry.name}"))return (void *)&klj_math_binding_${entry.name};`).join('\n')+'\n return NULL;\n}\n');
  const object=path.join(output,'lookup.o');run('clang',[...cflags,'-c',lookup,'-o',object]);objects.push(object);
  fs.writeFileSync(path.join(output,'exports.json'),JSON.stringify(entries,null,2)+'\n');
  if(shim)adaptNativeMathShim({shim,klepton,entries,output});
  return {
    objects,
    entries,
    output
  };
}

/** Move the implemented family out of sample-derived tables into its one owner. */
export function adaptNativeMathShim({shim,klepton,entries,output}){
  const names=new Set(entries.filter(entry=>entry.owner!=='binary128').map(entry=>entry.name));
  const table=fs.readFileSync(path.join(klepton,'runtime/libc/kl_libc_table.h'),'utf8').replace(/^KL_FWD\((\w+)\).*\n/gm,(line,name)=>names.has(name)?'':line);
  const target=path.join(output,'libc-table.h');fs.writeFileSync(target,table);
  let text=fs.readFileSync(shim,'utf8');
  text=text.replace('#include "kl_libc_table.h"','#include '+JSON.stringify(path.resolve(target)));
  text=text.replace(/E\("(\w+)",\s*\w+\),?/g,(entry,name)=>names.has(name)?'':entry);
  fs.writeFileSync(shim,text);
}
