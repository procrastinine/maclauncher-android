import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from '../binder-rpc/build.mjs';
import {prepareSourceTree,writeChanged} from '../upstream/source-files.mjs';

const root=import.meta.dirname;
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));

/** Bionic's inline synchronization state belongs to the guest ABI. Keep its
 * complete original algorithms, including static/process-shared state, in the
 * existing guest-width helper image. Only TLS identity and diagnostics cross
 * the common native resource boundary. */
export function prepareArm32Synchronization({bionicSource,output}){
  const source=path.join(output,'source');
  const units=['pthread_mutex','pthread_cond','pthread_rwlock','pthread_barrier',
    'pthread_spinlock','pthread_once','semaphore','bionic_futex','bionic_time_conversions']
    .map(name=>'libc/bionic/'+name+'.cpp');
  const headers=['bionic_constants.h','bionic_fortify.h','bionic_futex.h',
    'bionic_lock.h','bionic_systrace.h','bionic_time_conversions.h']
    .map(name=>'libc/private/'+name);
  const files=[
    ...units,
    ...headers,
    'libc/platform/bionic/macros.h',
    'libc/async_safe/include/async_safe/log.h'
  ];
  prepareSourceTree(source,temporary=>{
    exportSources(bionicSource,pins.aospBionic,files,temporary);
    for(const relative of units){
      const file=path.join(temporary,relative);
      let body=fs.readFileSync(file,'utf8');
      body=body.replace('#include "pthread_internal.h"','#include "synchronization-boundary.h"')
        .replace('#include "private/bionic_tls.h"','')
        .replaceAll('__get_thread()->tid','gettid()');
      if(body.includes('__get_thread')||body.includes('bionic_tls.h'))
        throw Error('Original Bionic synchronization TLS boundary changed');
      writeChanged(file,body);
    }
  });
  const include=path.join(source,'libc');
  const boundary=path.join(output,'synchronization-boundary.h');
  writeChanged(boundary,`#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <android/api-level.h>
#ifndef __LIBC_HIDDEN__
#define __LIBC_HIDDEN__ __attribute__((visibility("hidden")))
#endif
#include "private/bionic_lock.h"
`);
  const diagnostics=path.join(output,'synchronization-diagnostics.cpp');
  writeChanged(diagnostics,`#include "synchronization-boundary.h"
#include <android/log.h>
#include <android/trace.h>
#include <async_safe/log.h>
#include "private/bionic_systrace.h"

// The original class retains its lifetime/state; tracing and fatal reporting
// use the same Android endpoints as all other guest native libraries.
ScopedTrace::ScopedTrace(const char* message):called_end_(false){bionic_trace_begin(message);}
ScopedTrace::~ScopedTrace(){End();}
void ScopedTrace::End(){if(!called_end_){bionic_trace_end();called_end_=true;}}
void bionic_trace_begin(const char* message){ATrace_beginSection(message);}
void bionic_trace_end(){ATrace_endSection();}
extern "C" void async_safe_fatal_va_list(const char* prefix,const char* format,va_list args){
  __android_log_vprint(ANDROID_LOG_FATAL,prefix,format,args);
}
`);
  return {source,include,includes:[output,path.join(source,'libc/async_safe/include')],
    annotations:boundary,api:36,units:[...units.map(unit=>path.join(source,unit)),diagnostics],
    libraries:['libandroid.so','liblog.so'],
    files,relativeUnits:units,revision:pins.aospBionic};
}
