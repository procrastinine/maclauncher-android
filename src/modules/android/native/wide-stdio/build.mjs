import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {parseBlueprint} from '../upstream/blueprint.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';

const root=import.meta.dirname;
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));
const compilerRtRevision=pins.aospCompilerRt;
const run=(program,args,options={})=>execFileSync(program,args,{encoding:'utf8',maxBuffer:32*1024*1024,...options});
const baseHostFlags=['-target','arm64-apple-macos13','-O2','-fPIC'];
function bodyRange(text,signature){
  const start=text.indexOf(signature);if(start<0)throw Error('Original formatter boundary changed: '+signature);
  const open=text.indexOf('{',start);let depth=1,index=open+1;
  for(;depth&&index<text.length;index++){if(text[index]==='{')depth++;if(text[index]==='}')depth--;}
  return {start,open,end:index};
}
function replaceBody(text,signature,replacement){const {open,end}=bodyRange(text,signature);return text.slice(0,open)+replacement+text.slice(end);}
function removeFunction(text,signature){const {start,end}=bodyRange(text,signature);return text.slice(0,start)+text.slice(end);}
function definitions(ir){return [...ir.matchAll(/^(?:define\b[^\n]*?@|@)([\w.$]+)(?=\(|\s*=)/gm)].map(match=>match[1]).filter(name=>!name.startsWith('llvm.'));}
function rename(ir,names){return ir.replace(/@([\w.$]+)/g,(token,name)=>names.has(name)?'@'+names.get(name):token);}

/** Preserve the whole original Android wide formatter and gdtoa module. LLVM's
 * Android frontend owns AAPCS64 va_list and binary128; its Darwin backend owns
 * Mach-O code generation. Only FILE, locale, locks and errno cross the host ABI. */
export function prepareWideStdio({bionicSource,compilerRtSource,klepton,build,shim,libcSource,table}){
  const output=path.join(build,'wide-stdio'),source=path.join(output,'source');
  const portable=nativePathFlags({source,build,roots:[[klepton,'/src/klepton']]}),hostFlags=[...baseHostFlags,...portable];
  exportSources(bionicSource,pins.aospBionic,[
    'libc/include',
    'libc/kernel',
    'libc/stdio',
    'libc/upstream-openbsd',
    'libc/private',
    'libc/platform',
    'libc/Android.bp',
    'libc/libc.map.txt'
  ],source);
  const libc=path.join(source,'libc'),bsd=path.join(libc,'upstream-openbsd'),gdtoa=path.join(bsd,'lib/libc/gdtoa');
  const include=['-I'+libc,'-I'+path.join(libc,'include'),'-I'+path.join(libc,'kernel/uapi'),'-I'+path.join(libc,'kernel/uapi/asm-arm64'),'-I'+path.join(libc,'kernel/android/uapi'),'-I'+path.join(libc,'private'),'-I'+path.join(bsd,'android/include'),'-I'+path.join(bsd,'lib/libc/include'),'-I'+path.join(bsd,'lib/libc/stdio'),'-I'+gdtoa,'-I'+root,'-I'+output];
  const declarations=path.join(output,'boundary.h');
  fs.writeFileSync(declarations,'#include "bridge.h"\n#include <stdarg.h>\n#undef va_copy\n#define va_copy(destination,source) __builtin_memcpy(&(destination),&(source),sizeof(va_list))\n#undef va_end\n#define va_end(arguments) ((void)(arguments))\n#define __errno klj_wide_errno\n#define mmap klj_wide_mmap\n#define __fortify_fatal klj_wide_fatal\n#define strerrorname_np klj_wide_error_name\n#define pthread_mutex_lock klb_pthread_mutex_lock\n#define pthread_mutex_unlock klb_pthread_mutex_unlock\n');
  const guestFlags=[...portable,'-target','aarch64-linux-android36','-D_GNU_SOURCE','-ffreestanding','-fno-stack-protector','-fno-builtin','-fPIC','-O2','-Wno-macro-redefined','-Wno-nullability-completeness',...include,'-include',declarations];
  const compiled=[];
  function compileIR(input,key,extra=[]){
    const ir=path.join(output,key+'.ll');run('clang',[...guestFlags,...extra,'-S','-emit-llvm',input,'-o',ir]);compiled.push({input,key,ir});return ir;
  }
  let common=fs.readFileSync(path.join(libc,'stdio/printf_common.h'),'utf8');
  const conversion=bodyRange(common,'static wchar_t* mbsconv(');
  const hostConversion=common.slice(conversion.start,conversion.end).replace('static wchar_t* mbsconv','extern "C" wchar_t* klj_wide_mbsconv');
  const conversionFile=path.join(output,'conversion.cpp');
  fs.writeFileSync(conversionFile,'#include <wchar.h>\n#include <stdlib.h>\n#include <string.h>\n#include <limits.h>\n#include <errno.h>\n#include "bridge.h"\nextern "C" int kl_errno_to_linux(int);\n'+hostConversion.replaceAll('return nullptr;', '{ *klj_wide_errno()=kl_errno_to_linux(errno); return nullptr; }').replaceAll('return (nullptr);','{ *klj_wide_errno()=kl_errno_to_linux(errno); return nullptr; }'));
  common=common.replace('#include "local.h"','#include "bridge.h"\n#define MAXEXPDIG 6\n#define ORIENT_CHARS 1\n#define ORIENT_BYTES -1\n#define __SERR 1\n#define __sferror(fp) ((fp)->error)\n');
  common=common.replace('__attribute__((__noinline__))\n','');
  for(const name of ['static int __sbprintf(','static int sprint(','static char* wcsconv('])common=removeFunction(common,name);
  common=replaceBody(common,'static wint_t xfputwc(','{ return klj_wide_put(wc, fp); }');
  common=replaceBody(common,'static wchar_t* mbsconv(','{ return klj_wide_mbsconv(mbsarg, prec); }');
  common=common.replace(/\bFILE\b/g,'KLJWideOutput');
  fs.writeFileSync(path.join(output,'printf_common.h'),common);
  let formatter=fs.readFileSync(path.join(libc,'stdio/vfwprintf.cpp'),'utf8');
  formatter=formatter.replace('#define FUNCTION_NAME __vfwprintf','#define FUNCTION_NAME klj_wide_format');
  formatter=formatter.replace('int FUNCTION_NAME(','extern "C" int FUNCTION_NAME(').replace(/\bFILE\b/g,'KLJWideOutput').replaceAll('fp->_flags','fp->error');
  formatter=formatter.replace('_SET_ORIENTATION(fp, CHAR_TYPE_ORIENTATION);','').replace('cantwrite(fp)','klj_wide_can_write(fp)');
  formatter=formatter.replace(/  \/\/ Optimize writes to stderr[\s\S]*?\n  }\n/,'');
  formatter=formatter.replaceAll('nl_langinfo(RADIXCHAR)','klj_wide_radix()').replaceAll('strerror_r(', 'klj_wide_strerror(');
  formatter=formatter.replace(/^  (?:struct __suio |struct __siov|uio\.).*\n/gm,'');
  const formatterFile=path.join(output,'formatter.cpp');fs.writeFileSync(formatterFile,formatter);
  compileIR(formatterFile,'formatter',['-std=c++17','-include',path.join(bsd,'android/include/openbsd-compat.h')]);
  const blueprint=path.join(libc,'Android.bp');
  const module=parseBlueprint(fs.readFileSync(blueprint,'utf8'),{file:blueprint}).modules.find(item=>item.properties.name==='libc_gdtoa').properties;
  const units=[...module.srcs,...module.multilib.lib64.srcs];
  for(const unit of units)compileIR(path.join(libc,unit),path.basename(unit),['-include',path.join(bsd,'android/include/openbsd-compat.h')]);
  const names=new Map();for(const item of compiled)for(const name of definitions(fs.readFileSync(item.ir,'utf8')))if(name!=='klj_wide_format')names.set(name,'klj_wide_original_'+name);
  const objects=[];
  for(const item of compiled){const prepared=path.join(output,item.key+'.prepared.ll'),object=path.join(output,item.key+'.o');fs.writeFileSync(prepared,rename(fs.readFileSync(item.ir,'utf8'),names));run('clang',[...hostFlags,'-Wno-override-module','-x','ir','-c',prepared,'-o',object]);objects.push(object);}
  // Compiler support is selected from original build declarations by the actual
  // undefined-symbol dependency closure, including every alias a unit defines.
  const rtSource=path.join(output,'compiler-rt');
  exportSources(compilerRtSource,compilerRtRevision,['lib/builtins'],rtSource);
  const rt=path.join(rtSource,'lib/builtins');
  const cmake=fs.readFileSync(path.join(rt,'CMakeLists.txt'),'utf8');
  const rtUnits=[...new Set(['GENERIC_SOURCES','aarch64_SOURCES'].flatMap(name=>{
    const declaration=cmake.match(new RegExp('set\\('+name+'\\s+([\\s\\S]*?)\\)'));
    if(!declaration)throw Error('Original compiler-rt module missing: '+name);
    return [...declaration[1].replace(/#[^\n]*/g,'').matchAll(/\b([\w/]+\.c)\b/g)].map(match=>match[1]);
  }))];
  const rtDefinitions=new Map(),rtIR=[];
  for(const file of rtUnits){
    // The compiler determines which units actually supply binary128 helpers.
    // Preprocessing excludes target-inapplicable implementation bodies.
    const target=path.join(rt,file);if(!fs.existsSync(target))continue;
    const ir=path.join(output,'rt-'+file+'.ll');
    try{run('clang',[...guestFlags,'-Wno-error=implicit-function-declaration','-S','-emit-llvm',target,'-o',ir]);}catch(error){throw Error('Original compiler-rt source failed: '+file+'\n'+error.message);}
    const text=fs.readFileSync(ir,'utf8');for(const name of definitions(text))rtDefinitions.set(name,{file,ir});rtIR.push(ir);
  }
  const provided=new Set();
  function unresolved(){const symbols=run('nm',['-j','-u',...objects]);return [...new Set([...symbols.matchAll(/^_([^\s]+)$/gm)].map(match=>match[1]))].filter(name=>rtDefinitions.has(name)&&!provided.has(name));}
  for(let missing=unresolved();missing.length;missing=unresolved())for(const name of missing){
    if(provided.has(name))continue;const unit=rtDefinitions.get(name),object=path.join(output,'rt-'+unit.file+'.o');
    for(const symbol of definitions(fs.readFileSync(unit.ir,'utf8')))provided.add(symbol);
    run('clang',[...hostFlags,'-Wno-override-module','-x','ir','-c',unit.ir,'-o',object]);objects.push(object);
  }
  const errorNames=path.join(output,'error-names.c');
  const macros=run('clang',[...guestFlags,'-dM','-E','-x','c','-include','errno.h','/dev/null']);
  const errors=new Map();for(const match of macros.matchAll(/^#define (E[A-Z0-9]+) (\d+)$/gm))if(!errors.has(Number(match[2])))errors.set(Number(match[2]),match[1]);
  fs.writeFileSync(errorNames,'#include <stddef.h>\nconst char *klj_wide_error_name(int error){switch(error){\n'+[...errors].map(([value,name])=>`case ${value}:return "${name}";`).join('\n')+'\ndefault:return NULL;}}\n');
  const exports=[...fs.readFileSync(path.join(libc,'libc.map.txt'),'utf8').matchAll(/^\s*(\w*wprintf);/gm)].map(match=>match[1]).sort();
  const lookup=path.join(output,'lookup.c');fs.writeFileSync(lookup,'#include <stddef.h>\n#include <string.h>\n'+exports.map(name=>`extern void klj_wide_${name}(void);`).join('\n')+'\nvoid *klj_wide_stdio_lookup(const char *name){\n'+exports.map(name=>`if(!strcmp(name,"${name}"))return (void *)klj_wide_${name};`).join('\n')+'\nreturn NULL;}\n');
  for(const input of [conversionFile,errorNames,lookup,path.join(root,'bridge.c')]){const object=path.join(output,path.basename(input)+'.o');run('clang',[...hostFlags,'-I'+root,'-I'+path.join(klepton,'runtime/libc'),'-c',input,'-o',object]);objects.push(object);}
  const macro=fs.readFileSync(path.join(klepton,'runtime/libc/kl_va_thunks.S'),'utf8').match(/\.macro KL_VTHUNK[\s\S]*?\.endm/);
  if(!macro)throw Error('Original AAPCS64 argument thunk is missing');
  const thunk=path.join(output,'thunks.S');fs.writeFileSync(thunk,'.text\n'+macro[0]+'\nKL_VTHUNK klj_wide_wprintf, klj_wide_vwprintf, 1\nKL_VTHUNK klj_wide_fwprintf, klj_wide_vfwprintf, 2\nKL_VTHUNK klj_wide_swprintf, klj_wide_vswprintf, 3\n');
  const thunkObject=thunk+'.o';run('clang',[...hostFlags,'-c',thunk,'-o',thunkObject]);objects.push(thunkObject);
  if(shim){
    let text=fs.readFileSync(shim,'utf8');
    text=text.replace(/E\("swprintf",\s*klb_swprintf\),?/g,'').replace('X(klb_swprintf)','');
    const layout=run('clang',[...guestFlags,'-x','c','-S','-emit-llvm','-o','-','-'],{input:'#include <stdio.h>\n#include <bits/struct_file.h>\nconst unsigned long klj_stdio_stride=sizeof(FILE);\n'});
    const stride=layout.match(/@klj_stdio_stride\s*=.*?constant i64 (\d+)/)?.[1];
    if(!stride)throw Error('Original Bionic FILE layout was not emitted');
    const old='if ((char *)guest >= g_sF && (char *)guest < g_sF + sizeof g_sF) return stderr;';
    if(text.split(old).length!==2)throw Error('Shared standard stream owner changed');
    text=text.replace(old,`if ((char *)guest >= g_sF && (char *)guest < g_sF + sizeof g_sF) {\n        size_t slot=((char *)guest-g_sF)/${stride};\n        return slot==0?stdin:slot==1?stdout:stderr;\n    }`);
    fs.writeFileSync(shim,text);
  }
  if(table){const text=fs.readFileSync(table,'utf8');fs.writeFileSync(table,text.replace(/^KL_FWD\((\w+)\).*\n/gm,(line,name)=>exports.includes(name)?'':line));}
  if(libcSource){const text=fs.readFileSync(libcSource,'utf8');fs.writeFileSync(libcSource,removeFunction(text,'int    klb_swprintf('));}
  return {
    objects,
    output,
    exports
  };
}
