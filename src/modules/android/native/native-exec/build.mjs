import {copyOptional,writeChanged} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from '../binder-rpc/build.mjs';
import {compileHostObject} from '../host-compile.mjs';
const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));

/** Original Bionic PATH/script/argv handling above the shared native process
 * boundary. No executable names or applications determine its namespace. */
export function prepareNativeExec({bionicSource,klepton,build,ninja,python}){
 build=path.resolve(build);const output=path.join(build,'native-exec'),source=path.join(output,'source');
 const units=['libc/bionic/exec.cpp','libc/bionic/__bionic_get_shell_path.cpp'];
 exportSources(bionicSource,pins.aospBionic,[...units,'libc/private/FdPath.h','libc/private/__bionic_get_shell_path.h','libc/NOTICE'],source);
 const original=fs.readFileSync(path.join(source,units[0]),'utf8');
 const declarations=[...original.matchAll(/^int\s+(\w+)\s*(\([^;{]*\))\s*\{/gm)].map(m=>({name:m[1],signature:'int '+m[1]+m[2]+';'}));
 const names=[...declarations.map(d=>d.name),'execve'];
 const namespace=path.join(output,'namespace.h');
 writeChanged(namespace,'#include <stddef.h>\n#include <string.h>\n'+names.map(name=>`#define ${name} klj_exec_${name}`).join('\n')+'\n#define access klj_exec_access\n#define __bionic_get_shell_path klj_exec_shell_path\n#include <unistd.h>\nextern "C" {\n'+declarations.map(d=>d.signature).join('\n')+'\n}\nstatic inline void *klj_exec_mempcpy(void *to,const void *from,size_t size){return static_cast<char *>(memcpy(to,from,size))+size;}\n#define mempcpy klj_exec_mempcpy\n');
 const include=path.join(output,'include'),fd=path.join(source,'libc/private/FdPath.h');writeChanged(path.join(include,'private/FdPath.h'),fs.readFileSync(fd,'utf8').replace('"/proc/self/fd/%d"','"/dev/fd/%d"'));
 const variadic=declarations.filter(d=>d.signature.includes('...')).map(d=>{
  const start=original.indexOf('int '+d.name+'('),end=original.indexOf('\n}',start),body=original.slice(start,end),variant=body.match(/__execl<(\w+)>/)?.[1];
  if(!variant)throw Error('Original exec argument variant changed: '+d.name);
  return {...d,variant};
 });
 const entry=path.join(output,'exec.cpp');
 // Only va_list representation changes. Bionic retains argument collection,
 // environment selection, PATH search and script fallback in __execl.
 writeChanged(entry,original+'\n#include <vector>\nextern "C" {\n#include <kl_va.h>\n}\n'+variadic.map(d=>`extern "C" int klj_exec_${d.name}_handler(const char *name,const char *arg,kl_va *arguments){\nstd::vector<uint64_t> words;do{words.push_back(kl_va_gp(arguments));}while(words.back());\n${d.variant==='ExecLE'?'words.push_back(kl_va_gp(arguments));':''}\nstatic_assert(sizeof(va_list)==sizeof(char *));va_list native=reinterpret_cast<char *>(words.data());return __execl<${d.variant}>(name,arg,native);\n}`).join('\n'));
 const flags=['-std=c++20','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-fno-builtin','-Wno-vla-cxx-extension','-I'+include,'-I'+path.join(source,'libc'),'-I'+path.join(klepton,'runtime/libc')];
 const compile=(name,input,extra=[])=>compileHostObject({name,inputs:[input],flags:[...flags,...extra],directory:path.join(build,'host-objects'),cwd:source,ninja,python,compiler:'clang++'}).output;
 const objects=units.map((unit,index)=>compile('bionic-'+path.basename(unit)+'.o',index===0?entry:path.join(source,unit),['-include',namespace]));
 const thunks=path.join(output,'thunks.S'),macro=fs.readFileSync(path.join(klepton,'runtime/libc/kl_va_thunks.S'),'utf8').match(/\.macro KL_VTHUNK[\s\S]*?\.endm/)?.[0];
 if(!macro)throw Error('Original native variadic thunk changed');
 writeChanged(thunks,macro+'\n'+variadic.map(d=>`KL_VTHUNK klj_exec_${d.name}_thunk, klj_exec_${d.name}_handler, 2`).join('\n')+'\n');
 objects.push(compileHostObject({name:'bionic-exec-thunks.o',inputs:[thunks],flags:['-arch','arm64','-mmacosx-version-min=13.0'],directory:path.join(build,'host-objects'),cwd:source,ninja,python}).output);
 const boundary=path.join(output,'boundary.cpp');
 writeChanged(boundary,'#include <unistd.h>\n#include <limits.h>\nextern "C" const char *kl_guest_path(const char *,char *,size_t);\nextern "C" void *kl_shim_lookup(const char *);\nextern "C" int klj_exec_execve(const char *file,char *const *argv,char *const *envp){char mapped[PATH_MAX];return execve(kl_guest_path(file,mapped,sizeof mapped),argv,envp);}\nextern "C" int klj_exec_access(const char *file,int mode){using Access=int(*)(const char*,int);return reinterpret_cast<Access>(kl_shim_lookup("access"))(file,mode);}\n');
 objects.push(compile('bionic-exec-boundary.o',boundary));
 const symbol=name=>'klj_exec_'+name+(variadic.some(d=>d.name===name)?'_thunk':'');
 const lookup=path.join(output,'lookup.cpp');writeChanged(lookup,'#include <cstring>\n'+names.map(name=>`extern "C" void ${symbol(name)}(void);`).join('\n')+'\nextern "C" void *klj_exec_lookup(const char *name){\n'+names.map(name=>`if(!std::strcmp(name,"${name}"))return reinterpret_cast<void *>(&${symbol(name)});`).join('\n')+'\nreturn nullptr;\n}\n');
 objects.push(compile('bionic-exec-lookup.o',lookup));
 const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});copyOptional(path.join(source,'libc/NOTICE'),path.join(notices,'NOTICE'));
 return {output,objects,names,notices,thunks};
}
