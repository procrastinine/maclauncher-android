import {copyOptional} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';

const root=import.meta.dirname;
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));

/** All implementation units behind the original public search.h family. The
 * same portable sources can execute natively or in the guest-width ARM ELF. */
export function prepareSearchSources({bionicSource,output}){
  const source=path.join(output,'source');
  const units=[...['insque','remque','lsearch','tfind','tsearch'].map(name=>'libc/upstream-openbsd/lib/libc/stdlib/'+name+'.c'),
    ...['hcreate','hcreate_r','hdestroy_r','hsearch_r'].map(name=>'libc/upstream-freebsd/lib/libc/stdlib/'+name+'.c'),'libc/bionic/tdestroy.cpp'];
  exportSources(bionicSource,pins.aospBionic,[...units,'libc/upstream-freebsd/lib/libc/stdlib/hsearch.h','libc/include/search.h','libc/NOTICE'],source);
  const header=fs.readFileSync(path.join(source,'libc/include/search.h'),'utf8');
  const names=[...header.replace(/\/\*[\s\S]*?\*\//g,'').matchAll(/^(?:void|int|ENTRY)[^()\n]*?\b(\w+)\(/gm)].map(match=>match[1]);
  const include=path.join(output,'include');fs.mkdirSync(include,{recursive:true});
  fs.writeFileSync(path.join(include,'search.h'),header.replace(/__BIONIC_AVAILABILITY_GUARD\(\d+\)/g,'1').replace(/__INTRODUCED_IN\(\d+\)/g,''));
  const annotations=path.join(output,'annotations.h');fs.writeFileSync(annotations,'#define __USE_GNU 1\n#define __FBSDID(x)\n#define DEF_WEAK(symbol)\n');
  return {source,include,annotations,names,units:units.map(unit=>path.join(source,unit)),relativeUnits:units};
}

export function prepareNativeSearch({bionicSource,build}){
  const output=path.join(build,'native-search');
  const prepared=prepareSearchSources({bionicSource,output});
  const namespace=path.join(output,'namespace.h');
  fs.writeFileSync(namespace,prepared.names.map(name=>`#define ${name} klj_search_${name}`).join('\n')+'\n');
  const objects=[],flags=[...nativePathFlags({source:prepared.source,build}),'-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-fno-builtin','-I'+prepared.include,'-include',prepared.annotations,'-include',namespace];
  for(const file of prepared.units){
    const object=path.join(output,path.basename(file)+'.o');
    execFileSync(file.endsWith('.cpp')?'clang++':'clang',[...flags,'-Wno-deprecated-non-prototype','-c',file,'-o',object],{stdio:'inherit'});objects.push(object);
  }
  const lookup=path.join(output,'lookup.c');
  fs.writeFileSync(lookup,'#include <string.h>\n#include <stddef.h>\n'+prepared.names.map(name=>`extern void klj_search_${name}(void);`).join('\n')+'\nvoid *klj_search_lookup(const char *name){\n'+prepared.names.map(name=>` if(!strcmp(name,"${name}"))return (void *)&klj_search_${name};`).join('\n')+'\n return NULL;\n}\n');
  const object=path.join(output,'lookup.o');execFileSync('clang',[...flags,'-c',lookup,'-o',object],{stdio:'inherit'});objects.push(object);
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});copyOptional(path.join(prepared.source,'libc/NOTICE'),path.join(notices,'NOTICE'));
  return {...prepared,output,objects,notices};
}
