import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {compileHostObject} from '../host-compile.mjs';
import {writeChanged} from '../upstream/source-files.mjs';

const run=(command,args)=>execFileSync(command,args,{encoding:'utf8',maxBuffer:32*1024*1024});
const definitions=text=>[...text.matchAll(/^(?:define\b[^\n]*?@|@)([\w.$]+)(?=\(|\s*=)/gm)].map(match=>match[1]).filter(name=>!name.startsWith('llvm.'));

/** Keep Android's original multibyte state and complete implementation units.
 * LLVM's Android frontend supplies the ABI; the Darwin backend supplies Mach-O.
 * No host wchar/mbstate record is passed into original Android code. */
export function prepareNativeMultibyte({bionicSource,revision,build,ninja,python}){
  const output=path.join(build,'native-ctype','multibyte'),source=path.join(output,'source');
  exportSources(bionicSource,revision,['libc/Android.bp','libc/libc.map.txt','libc/include','libc/kernel','libc/private/bionic_mbstate.h','libc/bionic','libc/upstream-openbsd/lib/libc/locale','libc/NOTICE'],source);
  const libc=path.join(source,'libc'),manifest=fs.readFileSync(path.join(libc,'Android.bp'),'utf8');
  const candidates=[...new Set([...manifest.matchAll(/"((?:bionic\/|upstream-openbsd\/lib\/libc\/locale\/)[^"*]+\.(?:c|cpp))"/g)].map(match=>match[1]))];
  const units=candidates.filter(file=>/\bmbstate_t\b/.test(fs.readFileSync(path.join(libc,file),'utf8'))).sort();
  if(!units.length)throw Error('Original Bionic multibyte implementation units are absent');
  const macros=run('clang',['-dM','-E','-x','c','-include','errno.h','/dev/null']);
  const errors=[...macros.matchAll(/^#define (E[A-Z0-9_]+) (\d+)$/gm)];
  const boundary=path.join(output,'boundary.h');
  writeChanged(boundary,'#define __errno __error\n#include <errno.h>\n#define DEF_STRONG(symbol)\n#define DEF_WEAK(symbol)\n'+errors.map(match=>`#undef ${match[1]}\n#define ${match[1]} ${match[2]}`).join('\n')+'\n');
  const android=['-target','aarch64-linux-android36','-D_GNU_SOURCE','-ffreestanding','-fno-stack-protector','-fno-builtin','-fPIC','-O2','-Wno-nullability-completeness','-Wno-macro-redefined','-I'+libc,'-I'+path.join(libc,'include'),'-I'+path.join(libc,'kernel/uapi'),'-I'+path.join(libc,'kernel/uapi/asm-arm64'),'-I'+path.join(libc,'kernel/android/uapi'),'-include',boundary];
  const compiled=units.map(file=>{
    const input=path.join(libc,file),name='bionic-multibyte-'+path.basename(file)+'.bc';
    const bitcode=compileHostObject({name,inputs:[input],flags:[...android,'-emit-llvm'],directory:path.join(build,'host-objects'),cwd:source,ninja,python,compiler:file.endsWith('.cpp')?'clang++':'clang'}).output;
    return {file,text:run('clang',['-Wno-override-module','-x','ir','-S','-emit-llvm',bitcode,'-o','-'])};
  });
  const names=new Map(),aliases=new Map();for(const item of compiled){
    for(const name of definitions(item.text))names.set(name,'klj_ctype_original_'+name);
    for(const match of item.text.matchAll(/^module asm "(\w+) = (\w+)"$/gm)){aliases.set(match[1],match[2]);names.set(match[1],'klj_ctype_original_'+match[1]);}
  }
  const exports=new Set([...fs.readFileSync(path.join(libc,'libc.map.txt'),'utf8').matchAll(/^\s*([\w]+);/gm)].map(match=>match[1]));
  const bound=[...names.keys()].filter(name=>exports.has(name)||aliases.has(name)).sort(),objects=[];
  for(const item of compiled){
    const prepared=path.join(output,path.basename(item.file)+'.ll');
    const text=item.text.replace(/^\$[^\n]* = comdat[^\n]*\n/gm,'').replace(/ comdat(?:\([^)]*\))?/g,'').replace(/@([\w.$]+)/g,(token,name)=>names.has(name)?'@'+names.get(name):token)
      .replace(/^module asm "\.global (\w+)"$/gm,(line,name)=>names.has(name)?`module asm ".globl _${names.get(name)}"`:line)
      .replace(/^module asm "(\w+) = (\w+)"$/gm,(line,alias,target)=>aliases.has(alias)?`module asm "_${names.get(alias)} = _${names.get(target)}"`:line);
    writeChanged(prepared,text);
    objects.push(compileHostObject({name:'bionic-multibyte-'+path.basename(item.file)+'.o',inputs:[prepared],flags:['-target','arm64-apple-macos13','-O2','-fPIC','-Wno-override-module','-x','ir'],directory:path.join(build,'host-objects'),cwd:source,ninja,python}).output);
  }
  writeChanged(path.join(output,'exports.json'),JSON.stringify({revision,units,exports:bound},null,2)+'\n');
  return {objects,names:bound,symbols:names,output,units};
}
