import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';

/** Reuse SDL's complete physical Linux key table with the supplied SDL3 ABI. */
export function prepareInputKeymap({source,sdl,output}){
 fs.mkdirSync(output,{recursive:true});
 const table=path.join(source,'src/events/scancodes_linux.h'),text=fs.readFileSync(table,'utf8');
 const original=path.join(output,'original.c'),adapted=path.join(output,'adapted.c');
 const headers=path.join(sdl,'Headers');
 const declarations=fs.readFileSync(path.join(headers,'SDL_scancode.h'),'utf8');
 const aliases=fs.readFileSync(path.join(headers,'SDL_oldnames.h'),'utf8');
 const supported=new Set([...declarations.matchAll(/\b(SDL_SCANCODE_\w+)\s*=/g)].map(m=>m[1]));
 for(const match of aliases.matchAll(/^#define\s+(SDL_SCANCODE_\w+)\s+(SDL_SCANCODE_\w+)\s*$/gm))if(supported.has(match[2]))supported.add(match[1]);
 const missing=new Set();
 let body=text.replace('#include "../../include/SDL_scancode.h"','');
 if(body===text)throw Error('Original SDL Linux key table include changed');
 body=body.replace(/\bSDL_SCANCODE_\w+\b/g,name=>{if(supported.has(name))return name;missing.add(name);return 'SDL_SCANCODE_UNKNOWN';});
 const main='\n#include <stdio.h>\nint main(void){for(size_t i=0;i<sizeof(linux_scancode_table)/sizeof(*linux_scancode_table);i++)printf("%u\\n",(unsigned)linux_scancode_table[i]);}\n';
 fs.writeFileSync(original,'#include '+JSON.stringify(path.resolve(table))+main);
 fs.writeFileSync(adapted,'#define SDL_ENABLE_OLD_NAMES\n#include <SDL3/SDL.h>\n'+body+main);
 const dump=(file,args)=>{const executable=file+'.out';execFileSync('clang',['-std=c11',file,...args,'-o',executable],{stdio:'pipe'});return execFileSync(executable,{encoding:'utf8'}).trim().split('\n').map(Number);};
 const before=dump(original,['-I'+path.join(source,'include')]);
 const after=dump(adapted,['-F'+path.dirname(sdl)]);
 if(before.length!==after.length)throw Error('SDL physical key table length changed');
 for(let i=0;i<before.length;i++)if(before[i]>0&&before[i]<256&&before[i]!==after[i])throw Error('SDL3 changed a USB keyboard scancode at Linux key '+i);
 const preamble=text.slice(0,text.indexOf('*/')+2),header=path.join(output,'input-keymap.h');
 fs.writeFileSync(header,preamble+'\n/* Adapted for the supplied SDL3 scancode ABI. */\n#pragma once\nstatic const unsigned short klj_linux_scancodes[] = {\n'+after.join(',')+'\n};\n');
 const revision=execFileSync('git',['-C',source,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
 const identity={repository:'https://github.com/libsdl-org/SDL',revision,table:'src/events/scancodes_linux.h',sha256:crypto.createHash('sha256').update(text).digest('hex'),sdl3ScancodesSHA256:crypto.createHash('sha256').update(declarations).digest('hex'),unavailableSdl3Scancodes:[...missing].sort()};
 fs.writeFileSync(path.join(output,'source.json'),JSON.stringify(identity,null,2)+'\n');
 return {
  header,
  identity
 };
}
