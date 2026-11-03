import {addLicense} from '../upstream/licenses.mjs';
import {includeDependencies} from '../upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';

const root=import.meta.dirname;
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));
const baseFlags=['-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC'];
function replace(text,before,after){if(text.split(before).length!==2)throw Error('Bionic nonlocal-jump boundary changed: '+before);return text.replace(before,after);}

/** ARM32 preserves the original guest CPU register implementation inside the
 * shared helper ELF. Its dependencies cross the existing typed libc boundary. */
export function prepareArm32JumpSources({bionicSource,output}){
  const source=path.join(output,'source');
  const files=['libc/arch-arm/bionic/setjmp.S','libc/bionic/setjmp_cookie.cpp','libc/private/bionic_asm.h','libc/private/bionic_asm_arm.h','libc/NOTICE'];
  exportSources(bionicSource,pins.aospBionic,files,source);
  const boundary=path.join(output,'cookie.h');
  fs.writeFileSync(boundary,`#pragma once
#include <stdlib.h>
#include <android/log.h>
struct libc_globals { long setjmp_cookie; };
extern libc_globals* const klj_arm32_jump_globals;
#define __libc_globals klj_arm32_jump_globals
#define __libc_safe_arc4random_buf arc4random_buf
#ifndef __LIBC_HIDDEN__
#define __LIBC_HIDDEN__ __attribute__((visibility("hidden")))
#endif
#define async_safe_fatal(...) __android_log_assert(nullptr,"libc",__VA_ARGS__)
`);
  let cookie=includeDependencies(fs.readFileSync(path.join(source,files[1]),'utf8'),'#include "cookie.h"');
  for(const header of ['async_safe/log.h'])cookie=cookie.replace('#include <'+header+'>','');
  for(const header of ['private/bionic_arc4random.h','private/bionic_globals.h'])cookie=cookie.replace('#include "'+header+'"','');
  const cookieFile=path.join(output,'setjmp_cookie.cpp');fs.writeFileSync(cookieFile,cookie);
  const owner=path.join(output,'cookie-owner.cpp');fs.writeFileSync(owner,`#include "cookie.h"
static libc_globals storage;
libc_globals* const klj_arm32_jump_globals=&storage;
void __libc_init_setjmp_cookie(libc_globals*);
__attribute__((constructor)) static void initialize_cookie(){__libc_init_setjmp_cookie(&storage);}
`);
  return {units:[path.join(source,files[0]),cookieFile,owner],include:path.join(source,'libc'),source,files};
}

/** Bionic owns the entire guest jump-buffer ABI. Only object format, guest
 * x18 storage, signal numbers and platform register extensions cross hosts. */
export function prepareNonlocalJump({bionicSource,klepton,build}){
  const output=path.join(build,'nonlocal-jump'),source=path.join(output,'source');
  const flags=[...baseFlags,...nativePathFlags({source,build})];
  const units=['libc/arch-arm64/bionic/setjmp.S','libc/bionic/setjmp_cookie.cpp'];
  exportSources(bionicSource,pins.aospBionic,[...units,'libc/private/bionic_constants.h','libc/include/setjmp.h','libc/NOTICE'],source);
  let assembly=fs.readFileSync(path.join(source,units[0]),'utf8');
  const names=[...new Set([...assembly.matchAll(/^(?:ENTRY_WEAK_FOR_NATIVE_BRIDGE|ALIAS_SYMBOL_WEAK_FOR_NATIVE_BRIDGE)\((\w+)/gm)].map(match=>match[1]))];
  const tsd=fs.readFileSync(path.join(klepton,'runtime/kl_x18.h'),'utf8').match(/^#define KLX_TSD_SLOT (\d+)$/m)?.[1];
  if(!tsd)throw Error('Klepton guest x18 slot declaration is unavailable');
  assembly=replace(assembly,'#include <private/bionic_asm.h>','');
  // Darwin reserves physical x18. Original SCS save/restore instead accesses
  // the same guest register slot used by every Klepton instruction veneer.
  assembly=replace(assembly,'  and x3, x18, #SCS_MASK',`  mrs x3, TPIDRRO_EL0\n  ldr x3, [x3, #(${tsd} * 8)]\n  and x3, x3, #SCS_MASK`);
  assembly=replace(assembly,'  and x18, x18, #~SCS_MASK\n  orr x18, x3, x18',`  mrs x9, TPIDRRO_EL0\n  ldr x12, [x9, #(${tsd} * 8)]\n  and x12, x12, #~SCS_MASK\n  orr x12, x3, x12\n  str x12, [x9, #(${tsd} * 8)]`);
  const nativeNames=[...names,'__bionic_setjmp_cookie_get','__bionic_setjmp_cookie_check','__bionic_setjmp_checksum_mismatch'];
  for(const name of nativeNames)assembly=assembly.replace(new RegExp('\\b'+name+'\\b','g'),'_klj_jump_'+name);
  assembly=assembly.replace(/^ENTRY_WEAK_FOR_NATIVE_BRIDGE\((\w+)\)$/gm,(_,name)=>`.text\n.p2align 2\n.globl ${name}\n${name}:\n.cfi_startproc`)
    .replace(/^END\(\w+\)$/gm,'.cfi_endproc')
    .replace(/^ALIAS_SYMBOL_WEAK_FOR_NATIVE_BRIDGE\((\w+), (\w+)\)$/gm,(_,name,target)=>`.globl ${name}\n${name} = ${target}`)
    .replace('NOTE_GNU_PROPERTY()','');
  assembly=replace(assembly,'  bne _klj_jump___bionic_setjmp_checksum_mismatch','  beq Lklj_checksum_ok\n  b _klj_jump___bionic_setjmp_checksum_mismatch\nLklj_checksum_ok:');
  assembly=assembly.replaceAll('bl sigprocmask','bl _klj_jump_sigprocmask');
  // The guest executes on the ordinary Darwin stack: no Linux MTE tags,
  // HWASan shadow stack or SME lazy-save ownership is installed on it.
  // These extension operations are therefore absent at the platform boundary.
  assembly=assembly.replaceAll('  bl __arm_za_disable','  // Darwin owns optional architectural extension state.');
  assembly=assembly.replaceAll('  bl memtag_handle_longjmp','  // Darwin stack mappings have no Android MTE/HWASan tags.');
  // Reserved words remain in Bionic's own buffer and integrity calculation.
  // A nonlocal return can abandon a native syscall/signal callback frame.
  assembly=replace(assembly,'.irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23',
    '.irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,25,26,27,28');
  const scopeCall=name=>`  stp x0, x30, [sp, #-32]!
  .cfi_adjust_cfa_offset 32
  str x1, [sp, #16]
  bl _klj_jump_native_${name}
  ldr x1, [sp, #16]
  ldp x0, x30, [sp], #32
  .cfi_adjust_cfa_offset -32
`;
  assembly=replace(assembly,'  // Calculate the checksum.',scopeCall('save')+'\n  // Calculate the checksum.');
  assembly=replace(assembly,'Lklj_checksum_ok:','Lklj_checksum_ok:\n'+scopeCall('restore'));
  const asmFile=path.join(output,'setjmp.S');fs.writeFileSync(asmFile,assembly);
  let cookie=fs.readFileSync(path.join(source,units[1]),'utf8');
  for(const header of ['sys/auxv.h','async_safe/log.h'])cookie=cookie.replace('#include <'+header+'>','');
  for(const header of ['private/bionic_arc4random.h','private/bionic_globals.h'])cookie=cookie.replace('#include "'+header+'"','');
  const cookieFile=path.join(output,'setjmp_cookie.cpp');fs.writeFileSync(cookieFile,cookie);
  const objects=[];
  for(const [name,input,extra] of [
    ['setjmp',asmFile,['-I'+root,'-I'+path.join(source,'libc')]],
    ['cookie',cookieFile,['-std=c++20','-include',path.join(root,'darwin.h')]],
    ['owner',path.join(root,'owner.cpp'),['-std=c++20']],
  ]){
    const object=path.join(output,name+'.o');
    execFileSync('clang++',[...flags,...extra,'-c',input,'-o',object],{stdio:'inherit'});objects.push(object);
  }
  const lookup=path.join(output,'lookup.c');
  fs.writeFileSync(lookup,'#include <string.h>\n#include <stddef.h>\n'+names.map(name=>`extern void klj_jump_${name}(void);`).join('\n')+'\nvoid *klj_nonlocal_jump_lookup(const char *name){\n'+names.map(name=>` if(!strcmp(name,"${name}"))return (void *)&klj_jump_${name};`).join('\n')+'\n return NULL;\n}\n');
  const object=path.join(output,'lookup.o');execFileSync('clang',[...flags,'-c',lookup,'-o',object],{stdio:'inherit'});objects.push(object);
  const licenses=path.join(output,'licenses');
  addLicense(path.dirname(licenses),'nonlocal-jump',path.join(source,'libc/NOTICE'));
  return {output,objects,licenses,names};
}
