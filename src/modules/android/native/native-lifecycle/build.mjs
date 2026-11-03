import {addLicense} from '../upstream/licenses.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {adaptGuestThreadEntry} from '../thread-entry-sources.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';
const root=import.meta.dirname;
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));
function replace(text,from,to){if(text.split(from).length!==2)throw Error('Native lifecycle boundary changed: '+from);return text.replace(from,to);}

/** Complete Bionic DSO/thread finalization and atfork implementation units. */
export function prepareNativeLifecycle({bionicSource,klepton,build}) {
  const output=path.join(build,'native-lifecycle'),source=path.join(output,'source');
  const portable=nativePathFlags({source,build});
  const units=['atexit','exit','pthread_atfork','__cxa_thread_atexit_impl'];
  exportSources(bionicSource,pins.aospBionic,[...units.map(name=>'libc/bionic/'+name+'.cpp'),'libc/bionic/atexit.h',
    'libc/platform/bionic/tls_defines.h','libc/include/bits/get_device_api_level_inlines.h','libc/NOTICE'],source);
  const objects=[];
  let threadDeclarations;
  for(const name of units) {
    const input=path.join(source,'libc/bionic',name+'.cpp');
    let text=fs.readFileSync(input,'utf8');
    for(const header of ['sys/prctl.h','async_safe/CHECK.h','async_safe/log.h'])text=text.replace('#include <'+header+'>','');
    for(const header of ['platform/bionic/page.h','platform/bionic/macros.h'])text=text.replace('#include "'+header+'"','');
    // Linux VMA labels are diagnostic metadata, not lifetime behavior.
    if(name==='atexit')text=replace(text,'    prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, new_pages, new_capacity_bytes, "atexit handlers");','');
    if(name==='pthread_atfork')text=replace(text,'  g_atfork_list_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;','  g_atfork_list_mutex = (pthread_mutex_t)PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;');
    if(name==='exit')text=replace(text,'#include "private/bionic_defs.h"','');
    if(name==='__cxa_thread_atexit_impl'){
      // Inventory every original C-linkage declaration/definition in this unit;
      // the native width adapter consumes declarations, never a symbol list.
      const declarations=[...text.matchAll(/^extern "C" ([^\n;{]+)[;{]/gm)].map(match=>match[1].replace(/\b__LIBC_HIDDEN__\s*/g,'').trim()+';');
      threadDeclarations=path.join(output,'cxa-thread-atexit.h');
      fs.writeFileSync(threadDeclarations,'#ifdef __cplusplus\nextern "C" {\n#endif\n'+declarations.join('\n')+'\n#ifdef __cplusplus\n}\n#endif\n');
      text=replace(text,'#include <private/bionic_defs.h>','');
      text=replace(text,'#include "pthread_internal.h"','#include '+JSON.stringify(path.join(root,'thread-local.h')));
      text=text.replaceAll('__attribute__((weak))','__attribute__((weak_import))');
    }
    fs.writeFileSync(input,text);
    const object=path.join(output,name+'.o');
    execFileSync('clang++',[...portable,'-std=c++20','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Werror',...(name==='__cxa_thread_atexit_impl'?[]:['-include',path.join(root,'darwin.h')]),
      ...(name==='exit'?['-Dexit=klj_bionic_exit','-D_exit=klb__exit','-D__cxa_thread_finalize=klj_guest_cxa_thread_finalize','-D__BIONIC_WEAK_FOR_NATIVE_BRIDGE=']:[]),'-c',input,'-o',object],{stdio:'inherit'});
    objects.push(object);
  }
  const owner=path.join(output,'owner.o');
  execFileSync('clang++',[...portable,'-std=c++20','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Werror','-c',path.join(root,'owner.cpp'),'-o',owner],{stdio:'inherit'});
  objects.push(owner);
  let shim=adaptGuestThreadEntry(fs.readFileSync(path.join(klepton,'runtime/libc/kl_shim.c'),'utf8'));
  shim=replace(shim,'static int  kl_cxa_atexit(void (*f)(void *), void *a, void *d) { (void)f;(void)a;(void)d; return 0; }\nstatic void kl_cxa_finalize(void *d) { (void)d; }',
    'extern int klj_guest_cxa_atexit(void (*)(void*), void*, void*);\nextern void klj_guest_cxa_finalize(void*);\nextern int klj_guest_atexit(void (*)(void));\nextern void klj_guest_exit(int);');
  shim=replace(shim,'E("__cxa_atexit", kl_cxa_atexit), E("__cxa_finalize", kl_cxa_finalize),','E("__cxa_atexit", klj_guest_cxa_atexit), E("__cxa_finalize", klj_guest_cxa_finalize), E("atexit", klj_guest_atexit),');
  shim=replace(shim,'E("exit", klb_exit),','E("exit", klj_guest_exit),');
  shim=replace(shim,'E("__cxa_thread_atexit_impl", klb___cxa_thread_atexit_impl),','E("__cxa_thread_atexit_impl", klb___cxa_thread_atexit_impl), E("__cxa_thread_atexit", klb___cxa_thread_atexit_impl),');
  const shimPath=path.join(output,'kl_shim.c');fs.writeFileSync(shimPath,shim);
  const pthreadFile=path.join(build,'pthread-source.c');
  let pthread=fs.readFileSync(pthreadFile,'utf8');
  pthread=replace(pthread,'    return pthread_atfork(p, c, ch);','    return klj_guest_pthread_atfork(p, c, ch);');
  fs.writeFileSync(pthreadFile,'extern int klj_guest_pthread_atfork(void (*)(void),void (*)(void),void (*)(void));\n'+pthread);
  let slink=fs.readFileSync(path.join(klepton,'runtime/libc/kl_libc_slink.c'),'utf8');
  const threadComment=slink.indexOf('// C++11 thread_local destructors.'),threadFunction=slink.indexOf('int klb___cxa_thread_atexit_impl(',threadComment);
  if(threadComment<0||threadFunction<0)throw Error('Pinned thread-local destructor boundary changed');
  slink=slink.slice(0,threadComment)+'// Original Bionic owns C++ thread-local registration and finalization.\n'+slink.slice(threadFunction);
  slink=replace(slink,'    (void)dso;\n    _tlv_atexit(fn, obj);\n    return 0;','    return klj_guest_cxa_thread_atexit_impl(fn, obj, dso);');
  slink=replace(slink,'    (void)dso;\n    return pthread_atfork(prep, parent, child);','    return klj_guest_register_atfork(prep, parent, child, dso);');
  const slinkPath=path.join(output,'kl_libc_slink.c');
  fs.writeFileSync(slinkPath,'extern int klj_guest_cxa_thread_atexit_impl(void (*)(void*),void*,void*);\nextern int klj_guest_register_atfork(void (*)(void),void (*)(void),void (*)(void),void*);\n'+slink);
  const licenses=path.join(output,'licenses');
  addLicense(path.dirname(licenses),'native-lifecycle',path.join(source,'libc/NOTICE'));
  return {objects,shim:shimPath,slink:slinkPath,licenses,
    abiFlags:['-DKLEPTON_TLS_DEFINES='+JSON.stringify(path.join(source,'libc/platform/bionic/tls_defines.h')),
      '-DKLEPTON_DEVICE_API_SOURCE='+JSON.stringify(path.join(source,'libc/include/bits/get_device_api_level_inlines.h'))],
    // Darwin requires explicitly named optional weak imports at final link.
    // Their null state is Bionic's original absent-loader-hook behavior.
    linkFlags:['-Wl,-U,_klj_guest_loader_add_thread_local_dtor','-Wl,-U,_klj_guest_loader_remove_thread_local_dtor'],
    ownerHeaders:[path.join(source,'libc/bionic/atexit.h'),threadDeclarations]};
}
