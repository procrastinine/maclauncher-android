import {copyOptional} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';
const root=import.meta.dirname;
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));
function replace(text,from,to){if(text.split(from).length!==2)throw Error('Pinned pthread attributes changed: '+from);return text.replace(from,to);}
function region(text,from,to,value){const a=text.indexOf(from),b=text.indexOf(to,a);if(a<0||b<0)throw Error('Pinned pthread attributes region changed');return text.slice(0,a)+value+text.slice(b);}
/** All portable functions from the upstream unit; only thread representation is Darwin-owned. */
export function preparePthreadAttributes({bionicSource,build,pthreadSource,shimSource}){
 const directory=path.join(build,'pthread-attributes'),source=path.join(directory,'source');
 exportSources(bionicSource,pins.aospBionic,['libc/bionic/pthread_attr.cpp','libc/bionic/pthread_mutex.cpp','libc/include/pthread.h','libc/bionic/pthread_internal.h','libc/NOTICE'],source);
 const file=path.join(source,'libc/bionic/pthread_attr.cpp');let text=fs.readFileSync(file,'utf8');
 const names=[...text.matchAll(/\nint (pthread_(?:attr_\w+|getattr_np))\(/g)].map(match=>match[1]);
 if(!names.length)throw Error('Missing original pthread attribute module');
 for(const line of ['#include <async_safe/log.h>','#include "platform/bionic/page.h"','#include "private/ErrnoRestorer.h"','#include "private/bionic_defs.h"','#include "pthread_internal.h"'])text=replace(text,line,'');
 // Linux's main-stack VMA lookup belongs to the replaced thread snapshot getter.
 text=region(text,'static int __pthread_attr_getstack_main_thread(', '__BIONIC_WEAK_FOR_NATIVE_BRIDGE\nint pthread_attr_getstack(', '');
 text=region(text,'int pthread_getattr_np(', '__BIONIC_WEAK_FOR_NATIVE_BRIDGE\nint pthread_attr_setscope(', 'int pthread_getattr_np(pthread_t thread,pthread_attr_t* attr) { return klj_pthread_getattr_np(thread,attr); }\n\n');
 const publicHeader=fs.readFileSync(path.join(source,'libc/include/pthread.h'),'utf8');
 const mutexSource=fs.readFileSync(path.join(source,'libc/bionic/pthread_mutex.cpp'),'utf8');
 const mutexStart=mutexSource.indexOf('/* a mutex attribute holds'),mutexEnd=mutexSource.indexOf('// Priority Inheritance mutex implementation',mutexStart);
 if(mutexStart<0||mutexEnd<0)throw Error('Pinned mutex attribute family changed');
 const mutex=mutexSource.slice(mutexStart,mutexEnd);
 const mutexNames=[...mutex.matchAll(/\nint (pthread_mutexattr_\w+)\(/g)].map(match=>match[1]);
 names.push(...mutexNames);
 const constants=['PTHREAD_STACK_MIN','PTHREAD_CREATE_DETACHED','PTHREAD_CREATE_JOINABLE','PTHREAD_EXPLICIT_SCHED','PTHREAD_INHERIT_SCHED','PTHREAD_SCOPE_SYSTEM','PTHREAD_SCOPE_PROCESS'];
 const definitions=constants.map(name=>{const value=publicHeader.match(new RegExp('^#define '+name+' ([0-9]+)$','m'));if(!value)throw Error('Missing Bionic constant '+name);return '#undef '+name+'\n#define '+name+' '+value[1];});
 for(const name of new Set(mutex.match(/\bPTHREAD_\w+/g))){
  const value=publicHeader.match(new RegExp('(?:^#define '+name+' +|^ *'+name+' *= *)([A-Za-z_0-9]+)','m'));
  if(!value)throw Error('Missing Bionic mutex attribute constant '+name);
  definitions.push('#undef '+name+'\n#define '+name+' '+value[1]);
 }
 const privateHeader=fs.readFileSync(path.join(source,'libc/bionic/pthread_internal.h'),'utf8');
 for(const name of ['PTHREAD_ATTR_FLAG_DETACHED','PTHREAD_ATTR_FLAG_INHERIT','PTHREAD_ATTR_FLAG_EXPLICIT','SIGNAL_STACK_SIZE_WITHOUT_GUARD','PTHREAD_STACK_SIZE_DEFAULT']){
  const value=privateHeader.match(new RegExp('^#define '+name+' (.+)$','m'));if(!value)throw Error('Missing Bionic constant '+name);definitions.push('#define '+name+' '+value[1]);
 }
 const prelude='#include '+JSON.stringify(path.join(root,'abi.h'))+'\n#include <errno.h>\n#include <stdio.h>\n#include <string.h>\n#include <sys/resource.h>\n#include <unistd.h>\n'+definitions.join('\n')+'\n#undef ENOTSUP\n#define ENOTSUP 95\n#define PTHREAD_GUARD_SIZE ((size_t)getpagesize())\n#define SCHED_NORMAL 0\n#define __BIONIC_WEAK_FOR_NATIVE_BRIDGE\n#define pthread_attr_t bionic_attr\n#define sched_param klj_linux_sched_param\nstatic size_t page_size(){return (size_t)getpagesize();}\n'+names.map(name=>'#define '+name+' klb_'+name).join('\n')+'\n';
 const declarations=[...text.matchAll(/\nint (pthread_(?:attr_\w+|getattr_np))\(([^)]*)\) \{/g)].map(match=>'int '+match[1]+'('+match[2]+');').join('\n');
 const unit=path.join(directory,'attributes.cpp');fs.writeFileSync(unit,prelude+'\n#define pthread_mutexattr_t int\nextern "C" {\n'+declarations+'\n'+text+'\n'+mutex+'\n}\n');
 const object=path.join(directory,'attributes.o');execFileSync('clang++',[...nativePathFlags({source,build}),'-std=c++20','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Werror','-c',unit,'-o',object],{stdio:'inherit'});
 let pthread=fs.readFileSync(pthreadSource,'utf8');
 pthread=region(pthread,'// ---------- attributes ----------','// ---------- keys ----------','#include '+JSON.stringify(path.join(root,'abi.h'))+'\n');
 pthread=region(pthread,'// bionic pthread_mutexattr_t is a plain int holding the type.','// ---------- condition variable ----------','');
 pthread=region(pthread,'// Liveness registry for the mutex-owner dump:', 'typedef struct { void *(*fn)(void *); void *arg; } tramp;', '#include '+JSON.stringify(path.join(root,'threads.inc'))+'\n');
 pthread=replace(pthread,'typedef struct { void *(*fn)(void *); void *arg; } tramp;','typedef klj_thread tramp;');
 pthread=replace(pthread,'    tramp t = *(tramp *)p; free(p);','    tramp *entry=p;\n    struct { void *(*fn)(void *); void *arg; } t={entry->fn,entry->arg};');
 const createStart=pthread.indexOf('int klb_pthread_create(');
 const createEnd=pthread.indexOf('    // KL_TRACE_THREADS=1.',createStart);
 const attributesStart=pthread.indexOf('    pthread_attr_t da;',createStart);
 if(createStart<0||createEnd<0||attributesStart<0)throw Error('Pinned pthread creation changed');
 pthread=pthread.slice(0,attributesStart)+`    pthread_attr_t da;
    int rc=pthread_attr_init(&da);
    if(rc)return px(rc);
    tramp *t=calloc(1,sizeof(*t));
    if(!t){pthread_attr_destroy(&da);return 12;}
    rc=klj_thread_attributes(&da,ga,&t->attributes);
    if(rc){pthread_attr_destroy(&da);free(t);return px(rc);}
    t->fn=fn;t->arg=arg;
    // Publish under the same lock the new thread uses to register itself.
    pthread_mutex_lock(&klj_threads_lock);
    rc=px(pthread_create(out,&da,thread_tramp,t));
    if(!rc){t->thread=*out;t->running=1;t->next=klj_threads;klj_threads=t;}
    pthread_mutex_unlock(&klj_threads_lock);
    pthread_attr_destroy(&da);
    if(rc)free(t);
`+pthread.slice(createEnd);
 pthread=replace(pthread,'return px(pthread_join(t, r));','return klj_thread_join(t, r);');
 pthread=replace(pthread,'return px(pthread_detach(t));','return klj_thread_detach(t);');
 pthread=region(pthread,'// Scheduling policy is the host\'s business;', 'int klb_sem_timedwait(', '');
 fs.writeFileSync(pthreadSource,pthread);
 let shim=fs.readFileSync(shimSource,'utf8');
 // Replace the old partial family with every function exported by the original unit.
 for(const name of names)shim=shim.replace(new RegExp('X\\(klb_'+name+'\\)','g'),'').replace(new RegExp('^\\s*E\\("'+name+'", klb_'+name+'\\),\\n','gm'),'');
 const marker='    E("pthread_create", klb_pthread_create),';
 shim=replace(shim,marker,names.map(name=>'    E("'+name+'", klb_'+name+'),').join('\n')+'\n'+marker);
 shim=names.map(name=>'extern int klb_'+name+'();').join('\n')+'\n'+shim;
 fs.writeFileSync(shimSource,shim);
 const notices=path.join(directory,'notices');fs.mkdirSync(notices,{recursive:true});copyOptional(path.join(source,'libc/NOTICE'),path.join(notices,'NOTICE'));
 return {object,notices,functions:names};
}
