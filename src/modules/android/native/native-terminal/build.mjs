import {copyOptional,writeChanged} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {exportSources} from '../binder-rpc/build.mjs';
import {compileHostObject} from '../host-compile.mjs';
import {includeDependencies} from '../upstream/source-includes.mjs';

const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));
const run=(command,args,input)=>execFileSync(command,args,{input,encoding:'utf8',maxBuffer:16*1024*1024});
const root=path.dirname(fileURLToPath(import.meta.url));

/** The original terminal family retains its Android records and flag logic.
 * Its ioctl calls share the same physical translation as direct guest ioctl. */
export function prepareNativeTerminal({bionicSource,build,ninja,python,libc}){
  const output=path.join(build,'native-terminal'),source=path.join(output,'source');
  exportSources(bionicSource,pins.aospBionic,['libc/bionic/termios.cpp','libc/include','libc/kernel','libc/NOTICE'],source);
  const original=path.join(source,'libc'),android=['-target','aarch64-linux-android36','-D_GNU_SOURCE','-ffreestanding','-fno-stack-protector','-fno-builtin','-fPIC','-O2','-Wno-nullability-completeness','-I'+path.join(original,'include'),'-I'+path.join(original,'kernel/uapi'),'-I'+path.join(original,'kernel/uapi/asm-arm64'),'-I'+path.join(original,'kernel/android/uapi')];
  const compile=(name,input,flags,compiler='clang')=>compileHostObject({name,inputs:[input],flags,directory:path.join(build,'host-objects'),cwd:source,ninja,python,compiler}).output;
  const boundary=path.join(output,'original-boundary.h');
  writeChanged(boundary,'#define __errno __error\n#include <errno.h>\n#include <sys/ioctl.h>\n#include <stdint.h>\nextern "C" int klj_terminal_ioctl(int,int,uintptr_t);\n#define ioctl(fd,request,arg) klj_terminal_ioctl(fd,request,(uintptr_t)(arg))\n');
  const bitcode=compile('bionic-terminal.bc',path.join(original,'bionic/termios.cpp'),[...android,'-include',boundary,'-emit-llvm'],'clang++');
  const ir=run('clang',['-Wno-override-module','-x','ir','-S','-emit-llvm',bitcode,'-o','-']);
  const names=[...ir.matchAll(/^define\b[^\n]*?@(\w+)\(/gm)].map(match=>match[1]);
  const prepared=path.join(output,'termios.ll');
  writeChanged(prepared,ir.replace(/@(\w+)/g,(token,name)=>names.includes(name)?'@klj_terminal_original_'+name:token));
  const objects=[compile('bionic-terminal.o',prepared,['-target','arm64-apple-macos13','-O2','-fPIC','-Wno-override-module','-x','ir'])];
  // Clang, rather than a second constant-expression evaluator, supplies the
  // exact pinned kernel constants and the original public record definitions.
  const headers=['asm-generic/termbits-common.h','asm-generic/termbits.h','asm-generic/termios.h','asm-generic/ioctls.h'];
  const constants=[...new Set(headers.flatMap(file=>[...fs.readFileSync(path.join(original,'kernel/uapi',file),'utf8').matchAll(/^#define\s+(\w+)\s+(?!_IO)([^\n]+)/gm)].map(match=>match[1])))].filter(name=>!name.startsWith('_'));
  const evaluated=run('clang',[...android,'-x','c','-S','-emit-llvm','-o','-','-'],'#include <termios.h>\nconst unsigned long klj_values[]={'+constants.join(',')+'};\n');
  const values=[...evaluated.match(/@klj_values =[^\n]+\[([^\n]+)\], align/)?.[1].matchAll(/i64 (\d+)/g)||[]].map(match=>match[1]);
  if(values.length!==constants.length)throw Error('Original terminal constants could not be evaluated');
  const declarations=run('clang',[...android,'-x','c','-E','-P','-'],'#include <termios.h>\n');
  const records=['termios','termios2','winsize'].map(name=>{
    const declaration=declarations.match(new RegExp('struct '+name+' \\{[^}]+\\};'))?.[0];
    if(!declaration)throw Error('Original terminal record is absent: '+name);
    return declaration.replace('struct '+name,'struct klj_android_'+name).replace(/\btcflag_t\b|\bspeed_t\b/g,'uint32_t').replace(/\bcc_t\b/g,'uint8_t').replace(/\bNCCS\b/g,'KLJ_ANDROID_NCCS');
  });
  writeChanged(path.join(output,'android-terminal.h'),'#include <stdint.h>\n'+constants.map((name,index)=>'#define KLJ_ANDROID_'+name+' '+values[index]+'ULL').join('\n')+'\n'+records.join('\n')+'\n');
  const hostMacros=new Set([...run('clang',['-dM','-E','-x','c','-include','termios.h','-include','sys/ioctl.h','/dev/null']).matchAll(/^#define (\w+) /gm)].map(match=>match[1]));
  const fields={
    iflag:'IGNBRK BRKINT IGNPAR PARMRK INPCK ISTRIP INLCR IGNCR ICRNL IUCLC IXON IXOFF IXANY IMAXBEL IUTF8',
    oflag:'OPOST OLCUC ONLCR OCRNL ONOCR ONLRET OFILL OFDEL',
    cflag:'CSTOPB CREAD PARENB PARODD HUPCL CLOCAL ADDRB CMSPAR CRTSCTS',
    lflag:'ISIG ICANON XCASE ECHO ECHOE ECHOK ECHONL NOFLSH TOSTOP ECHOCTL ECHOPRT ECHOKE FLUSHO PENDIN IEXTEN EXTPROC',
  };
  const pair=name=>'{KLJ_ANDROID_'+name+','+name+'}';
  const tables=Object.entries(fields).map(([field,list])=>'static const struct Flag '+field+'Flags[]={'+list.split(' ').filter(name=>hostMacros.has(name)).map(pair).join(',')+'};');
  const groups={CSIZE:['CS5','CS6','CS7','CS8'],NLDLY:['NL0','NL1'],CRDLY:['CR0','CR1','CR2','CR3'],TABDLY:['TAB0','TAB1','TAB2','TAB3'],BSDLY:['BS0','BS1'],VTDLY:['VT0','VT1'],FFDLY:['FF0','FF1']};
  const supported=Object.entries(groups).filter(([mask])=>hostMacros.has(mask));
  tables.push(...supported.map(([mask,names])=>'static const struct Flag '+mask+'Values[]={'+names.filter(name=>hostMacros.has(name)).map(pair).join(',')+'};'));
  tables.push('static const struct Group outputGroups[]={'+supported.filter(([mask])=>mask!=='CSIZE').map(([mask])=>'{KLJ_ANDROID_'+mask+','+mask+','+mask+'Values,COUNT('+mask+'Values)}').join(',')+'};');
  tables.push('static const struct Group sizeGroup={KLJ_ANDROID_CSIZE,CSIZE,CSIZEValues,COUNT(CSIZEValues)};');
  const commonBits=fs.readFileSync(path.join(original,'kernel/uapi/asm-generic/termbits.h'),'utf8');
  const controls=[...commonBits.slice(commonBits.indexOf('#define VINTR'),commonBits.indexOf('#define IUCLC')).matchAll(/^#define (\w+)/gm)].map(match=>match[1]).filter(name=>hostMacros.has(name));
  tables.push('static const struct Flag controls[]={'+controls.map(pair).join(',')+'};');
  tables.push('static const struct Flag speeds[]={'+constants.filter(name=>/^B\d+$/.test(name)).map(name=>'{KLJ_ANDROID_'+name+','+name.slice(1)+'}').join(',')+'};');
  writeChanged(path.join(output,'terminal-maps.h'),tables.join('\n')+'\n');
  const flags=['-std=c11','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Werror','-I'+output];
  objects.push(compile('bionic-terminal-boundary.o',path.join(root,'terminal.c'),flags));
  const lookup=path.join(output,'lookup.cpp');
  writeChanged(lookup,'#include <cstring>\n'+names.map(name=>'extern "C" void klj_terminal_original_'+name+'(void);').join('\n')+'\nextern "C" void *klj_terminal_lookup(const char *name){\n'+names.map(name=>'if(!std::strcmp(name,"'+name+'"))return reinterpret_cast<void *>(&klj_terminal_original_'+name+');').join('\n')+'\nreturn nullptr;\n}\n');
  objects.push(compile('bionic-terminal-lookup.o',lookup,[...flags.filter(flag=>flag!=='-std=c11'),'-std=c++20'],'clang++'));
  if(libc){
    let text=fs.readFileSync(libc,'utf8');
    const entry='int kl_ioctl(int fd, unsigned long req, uintptr_t arg) {';
    if(!text.includes(entry))throw Error('Original shared ioctl entry changed');
    text=includeDependencies(text.replace(entry,entry+'\n    int terminalResult;\n    if(klj_terminal_ioctl_dispatch(fd,req,arg,&terminalResult))return terminalResult;'), '#include <stdint.h>\nextern int klj_terminal_ioctl_dispatch(int,unsigned long,uintptr_t,int *);');
    writeChanged(libc,text);
  }
  const notices=path.join(output,'notices');
  copyOptional(path.join(original,'NOTICE'),path.join(notices,'NOTICE'));
  writeChanged(path.join(output,'exports.json'),JSON.stringify({exports:names},null,2)+'\n');
  return {objects,names,notices,output};
}
