import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';

const root=import.meta.dirname;
const supplier=JSON.parse(fs.readFileSync(path.join(root,'../angle-source.json'))).dependencies.find(record=>record.name==='third_party/zlib');
const baseFlags=['-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-std=c11','-DCHROMIUM_ZLIB_NO_CHROMECONF','-DHAVE_UNISTD_H'];
const run=(command,args)=>execFileSync(command,args,{encoding:'utf8',maxBuffer:32*1024*1024});
function rename(text,names) {
    // Darwin annotates POSIX declarations with assembler labels. Adapt those
    // together with the C identifier, or calls would still bypass the owner.
    text=text.replace(/\b__asm(?:__)?\s*\(((?:\s*"(?:\\.|[^"\\])*")+\s*)\)/g,(original,parts)=>{
      const name=[...parts.matchAll(/"(?:\\.|[^"\\])*"/g)].map(match=>JSON.parse(match[0])).join('');
      return name.startsWith('_')&&names.has(name.slice(1))?'__asm('+JSON.stringify('_'+names.get(name.slice(1)))+')':original;
    });
    return text.replace(/"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\b[A-Za-z_]\w*\b/g,token=>names.get(token)||token);
}

/** The complete original gzip file family shares the existing system codec
 * and guest POSIX owner. Compiler declarations determine its exported API. */
export function prepareGzip({zlibSource,klepton,build}) {
    const output=path.join(build,'gzip'),source=path.join(output,'source');
    const flags=[...baseFlags,...nativePathFlags({source:zlibSource,build,roots:[[klepton,'/src/klepton']]})];
    const files=run('git',['-C',zlibSource,'ls-tree','--name-only',supplier.commit]).trim().split('\n')
      .filter(name=>/^gz.*\.[ch]$/.test(name)||['zlib.h','zconf.h','LICENSE'].includes(name));
    exportSources(zlibSource,supplier.commit,files,source);
    const api=path.join(output,'api.c');fs.writeFileSync(api,'#include "gzguts.h"\n');
    const declarations=new Map();
    function visit(node){if(node.kind==='FunctionDecl'&&node.name?.startsWith('gz'))declarations.set(node.name,node.type.qualType);for(const child of node.inner||[])visit(child);}
    visit(JSON.parse(run('clang',[...flags,'-I'+source,'-fsyntax-only','-Xclang','-ast-dump=json',api])));
    const namespace=new Map([...declarations.keys()].map(name=>[name,'klj_z_'+name]));
    const nativeCalls={open:'klj_gzip_open',read:'klj_gzip_read',write:'klj_gzip_write',lseek:'klj_gzip_seek',close:'klj_gzip_close'};
    for(const [name,target] of Object.entries(nativeCalls))namespace.set(name,target);
    const objects=[];
    for(const unit of files.filter(file=>file.endsWith('.c'))){
      const prepared=path.join(output,unit),object=prepared+'.o';
      fs.writeFileSync(prepared,rename(run('clang',[...flags,'-I'+source,'-E','-P',path.join(source,unit)]),namespace));
      run('clang',[...flags,'-c',prepared,'-o',object]);objects.push(object);
    }
    const publicApi=[...declarations.keys()].filter(name=>!name.startsWith('gz_'));
    const overrides={gzprintf:'klj_gzip_printf',gzvprintf:'klj_gzip_vprintf'};
    const lookup=path.join(output,'lookup.c');
    fs.writeFileSync(lookup,'#include <stddef.h>\n#include <string.h>\n'+publicApi.map(name=>`extern void binding_${name}(void) __asm__("_${overrides[name]||'klj_z_'+name}");`).join('\n')+
      '\nvoid *klj_gzip_lookup(const char *name){\n'+publicApi.map(name=>` if(!strcmp(name,"${name}"))return (void *)binding_${name};`).join('\n')+'\n return NULL;\n}\n');
    for(const input of [lookup,path.join(root,'bridge.c')]){const object=path.join(output,path.basename(input)+'.o');run('clang',[...flags,'-I'+path.join(klepton,'runtime/libc'),'-c',input,'-o',object]);objects.push(object);}
    const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});fs.copyFileSync(path.join(source,'LICENSE'),path.join(notices,'LICENSE'));
    fs.writeFileSync(path.join(notices,'source.json'),JSON.stringify({...supplier,units:files.filter(file=>file.endsWith('.c')),adaptation:'Original complete gzip family; guest descriptor and path integration; original Klepton AAPCS64 printf argument conversion. Compression uses system libz.'},null,2)+'\n');
    fs.writeFileSync(path.join(output,'exports.json'),JSON.stringify(publicApi.map(name=>({name,type:declarations.get(name),owner:overrides[name]?'argument-abi':'original-gzip'})),null,2)+'\n');
    return {objects,notices,output,exports:publicApi};
}
