import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {mergeAngleDeclarations} from './angle-declarations.mjs';
const root=import.meta.dirname,out=path.resolve(process.argv[3]||path.join(root,'generated'));
const source=path.resolve(process.argv[2]||(()=>{throw Error('Expected the original Clang host AST and generated output directory');})());
fs.mkdirSync(out,{recursive:true});
const originalAst=JSON.parse(fs.readFileSync(source));
const ast=mergeAngleDeclarations(originalAst);
const headers=['EGL/egl.h','EGL/eglext.h','EGL/eglext_angle.h','GLES/gl.h','GLES/glext.h','GLES2/gl2.h','GLES2/gl2ext.h','GLES3/gl32.h','GLES3/gl3ext.h'];
const baseText=headers.map(n=>fs.readFileSync(path.join(root,'include',n),'utf8')).join('\n');
headers.push('GLES2/gl2ext_angle.h');
const text=headers.map(n=>fs.readFileSync(path.join(root,'include',n),'utf8')).join('\n');
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const callback=t=>ast.types[t]?.kind==='Pointer'&&ast.types[ast.types[t].element]?.kind?.startsWith('Function');
// A declared C array parameter is a pointer, including the original NV [2]
// output. Keep the compiler spelling in metadata and apply C parameter decay.
const parameter=t=>t.replace(/\[[^\]]*\]$/,' *');
const fdFunctions=new Set(['eglCreateStreamFromFileDescriptorKHR','eglGetStreamFileDescriptorKHR','eglDupNativeFenceFDANDROID','eglExportDMABUFImageMESA','glImportMemoryFdEXT','glImportSemaphoreFdEXT']);
const baseNames=Object.keys(originalAst.functions).filter(n=>/^(?:egl|gl)[A-Z]/.test(n)&&new RegExp('\\b'+n+'\\s*\\(').test(baseText)).sort();
const originalNames=new Set(baseNames);
const addedNames=Object.keys(ast.functions).filter(n=>!originalNames.has(n)&&/^(?:egl|gl)[A-Z]/.test(n)&&new RegExp('\\b'+n+'\\s*\\(').test(text)).sort();
const functions=[...baseNames,...addedNames].map(name=>{
 const f=ast.functions[name],t=ast.types[f.type];if(t.variadic)throw Error('Unexpected variadic graphics declaration: '+name);
 return{name,result:t.result,originalParameters:t.parameters,parameters:t.parameters.map(parameter),callbacks:t.parameters.flatMap((t,i)=>callback(t)?[i]:[]),functionResult:callback(t.result),fd:fdFunctions.has(name)};
});
if(baseNames.filter(n=>n.startsWith('egl')).length!==167||baseNames.filter(n=>n.startsWith('gl')).length!==1025)throw Error('Original complete NDK EGL/GLES declaration set changed');
fs.mkdirSync(out,{recursive:true});
const includes='#define EGL_EGLEXT_PROTOTYPES 1\n#define GL_GLEXT_PROTOTYPES 1\n'+headers.map(n=>`#include <${n}>`).join('\n')+'\n#include "egl-host.h"\n';
const fields=functions.map((f,i)=>`typedef struct {${f.parameters.map((t,j)=>`${t} a${j};`).join('')}${f.result==='void'?'':`${f.result} result;`}${!f.parameters.length&&f.result==='void'?'char unused;':''}} Args_${i};`).join('\n');
fs.writeFileSync(path.join(out,'egl-arguments.h'),includes+fields+'\n');
const lookupIndices=functions.map((f,i)=>i).sort((a,b)=>functions[a].name<functions[b].name?-1:functions[a].name>functions[b].name?1:0);
const lookupTable='static const unsigned lookup_indices[]={'+lookupIndices.join(',')+'};\n';
const debugCallbacks=new Set(['glDebugMessageCallback','glDebugMessageCallbackKHR']);
const supported=f=>(debugCallbacks.has(f.name)||f.name==='glBlobCacheCallbacksANGLE')||!f.fd&&!f.callbacks.length&&(!f.functionResult||f.name==='eglGetProcAddress');
let host='#include "egl-arguments.h"\n#include <errno.h>\n#include <pthread.h>\n#include <stdatomic.h>\n#include <stdio.h>\n#include <string.h>\nextern int kl_errno_to_linux(int),kl_errno_from_linux(int);\nstatic KljBionicEglOwner owner;static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;static _Atomic int ready;\n';
host+=`static _Atomic(uintptr_t) addresses[${functions.length}];\nstatic const char *names[]={${functions.map(f=>JSON.stringify(f.name)).join(',')}};\nstatic const unsigned char procedure_supported[]={${functions.map(f=>supported(f)?1:0).join(',')}};\n`;
host+='static void *resolve(unsigned i){uintptr_t value=atomic_load_explicit(&addresses[i],memory_order_acquire);if(!value){void *p=owner.resolve(names[i]);value=p?(uintptr_t)p:1;atomic_store_explicit(&addresses[i],value,memory_order_release);}return value==1?NULL:(void*)value;}\n';
host+=lookupTable;
host+='#include "debug-callback.c"\n#include "blob-callback.c"\n';
host+='static int64_t unsupported(unsigned i,const char *reason){fprintf(stderr,"[bionic-egl] %s: %s\\n",names[i],reason);return -95;}\n';
host+='static int available(const char *name){if(!name)return 0;if((!strcmp(name,"glDebugMessageCallback")||!strcmp(name,"glDebugMessageCallbackKHR"))&&(!owner.invoke||!owner.debug_callback||!owner.vm))return 0;if(!strcmp(name,"glBlobCacheCallbacksANGLE")&&(!owner.invoke||!owner.vm||!blob_owner()))return 0;size_t lo=0,hi=sizeof(lookup_indices)/sizeof(*lookup_indices);while(lo<hi){size_t mid=lo+(hi-lo)/2,i=lookup_indices[mid];int compare=strcmp(name,names[i]);if(!compare)return procedure_supported[i]&&resolve((unsigned)i)!=NULL;if(compare<0)hi=mid;else lo=mid+1;}return 0;}\n';
functions.forEach((f,i)=>{
 host+=`static int64_t invoke_${i}(KljBionicNdkPacket *packet){Args_${i} *a=(void*)(uintptr_t)packet->arguments;(void)a;`;
 if(f.name==='eglGetProcAddress'){host+='a->result=available(a->a0)?(__eglMustCastToProperFunctionPointerType)(uintptr_t)1:NULL;return 0;}\n';return;}
 if(debugCallbacks.has(f.name)){host+=`if(!owner.invoke||!owner.debug_callback||!owner.vm)return unsupported(${i},"native debug callback owner is unavailable");void *p=resolve(${i});if(!p)return -38;KljAngleDebugInstall target=NULL;memcpy(&target,&p,sizeof(target));debug_install(target,(uintptr_t)a->a0,(uintptr_t)a->a1,packet->caller);return 0;}\n`;return;}
 if(f.name==='glBlobCacheCallbacksANGLE'){host+=`void *p=resolve(${i});if(!p)return -38;KljAngleBlobInstall target=NULL;memcpy(&target,&p,sizeof(target));return blob_install(target,(uintptr_t)a->a0,(uintptr_t)a->a1,(uintptr_t)a->a2,packet->caller)?0:-38;}\n`;return;}
 const failure=reason=>{
  if(!f.name.startsWith('egl'))return `return unsupported(${i},${JSON.stringify(reason)});`;
  const result=f.result==='void'?'':f.result==='EGLNativeFileDescriptorKHR'||f.name==='eglDupNativeFenceFDANDROID'?'a->result=-1;':f.name==='eglDebugMessageControlKHR'?'a->result=0x300c;':'a->result=0;';
  return `(void)unsupported(${i},${JSON.stringify(reason)});owner.error(0x3002);${result}return 0;`;
 };
 if(f.fd){host+=failure('native descriptor transfer is not installed')+'}\n';return;}
 if(f.functionResult){host+=`(void)a;return unsupported(${i},"returned procedure signature has no installed guest carrier");}\n`;return;}
 if(f.callbacks.length)host+=`if(${f.callbacks.map(j=>`a->a${j}`).join('||')}){${failure('guest callback lifetime carrier is not installed')}}`;
 if(/^eglCreateImage(?:KHR)?$/.test(f.name))host+=`if(a->a2==0x3270){${failure('DMA-BUF native descriptor transfer is not installed')}}`;
 if(/^eglCreateSync(?:64KHR|KHR)?$/.test(f.name))host+=`if(a->a1==0x3144&&a->a2){for(size_t n=0;a->a2[n]!=0x3038;n+=2)if(a->a2[n]==0x3145&&a->a2[n+1]>=0){${failure('native fence descriptor transfer is not installed')}}}`;
 host+=`void *p=resolve(${i});if(!p){fprintf(stderr,"[bionic-egl] %s: original native provider has no entry\\n",names[${i}]);return -38;}${f.result} (*target)(${f.parameters.join(',')||'void'});memcpy(&target,&p,sizeof(target));${f.result==='void'?'':'a->result='}target(${f.parameters.map((_,j)=>`a->a${j}`).join(',')});`;
 if(/^glGetPointerv(?:KHR|ANGLE)?$/.test(f.name))host+='debug_pointer(a->a0,(void**)a->a1,target);blob_pointer(a->a0,(void**)a->a1,target);';
 host+='return 0;}\n';
});
host+='static int64_t(*const invocations[])(KljBionicNdkPacket*)={'+functions.map((f,i)=>`invoke_${i}`).join(',')+'};\n';
host+='static const KljBionicNdkTarget targets[]={'+functions.map((f,i)=>`{${i+1},sizeof(Args_${i})}`).join(',')+'};\nstatic const KljBionicNdkBindings bindings={sizeof(bindings),KLJ_BIONIC_NDK_ABI,sizeof(targets)/sizeof(*targets),0,targets};\n';
host+='const KljBionicNdkBindings *klj_bionic_egl_bindings(void){return atomic_load_explicit(&ready,memory_order_acquire)?&bindings:NULL;}\nint klj_bionic_egl_prepare(const KljBionicEglOwner *input){if(!input||!input->resolve||!input->error)return -22;pthread_mutex_lock(&lock);if(ready){int same=owner.resolve==input->resolve&&owner.error==input->error&&owner.invoke==input->invoke&&owner.context==input->context&&owner.debug_callback==input->debug_callback&&owner.vm==input->vm;pthread_mutex_unlock(&lock);return same?0:-16;}owner=*input;atomic_store_explicit(&ready,1,memory_order_release);pthread_mutex_unlock(&lock);return 0;}\n';
host+='int64_t klj_bionic_egl_dispatch(JNIEnv *env,KljBionicNdkPacket *packet){if(!atomic_load_explicit(&ready,memory_order_acquire)||!packet||packet->size!=sizeof(*packet)||packet->version!=KLJ_BIONIC_NDK_ABI||!packet->arguments||!packet->target||packet->target>bindings.count)return -22;unsigned i=(unsigned)packet->target-1;if(packet->argument_size!=targets[i].argument_size)return -22;int saved=errno;errno=kl_errno_from_linux(packet->error);int64_t status=invocations[i](packet);packet->error=kl_errno_to_linux(errno);errno=saved;return status;}\n';
fs.writeFileSync(path.join(out,'egl-host-generated.c'),host);
let guest='#include "egl-arguments.h"\n#include <errno.h>\n#include <stdlib.h>\n#include <stdatomic.h>\n#include <string.h>\n'+`static KljBionicNdkTarget targets[${functions.length}];static _Atomic int ready;\n`;
guest+='static int64_t host_call(KljBionicNdkPacket *packet){register uint64_t x0 __asm__("x0")=0x4d4c,x1 __asm__("x1")=KLJ_BIONIC_EGL_HOSTCALL,x2 __asm__("x2")=(uintptr_t)packet,x3 __asm__("x3")=sizeof(*packet),x8 __asm__("x8")=0x48430000;__asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x8):"x4","x5","x6","x7","x9","x10","x11","x12","x13","x14","x15","x16","x17","x30","memory","cc");return (int64_t)x0;}\n';
guest+=`uint64_t klj_bionic_egl_install(uint64_t raw){const KljBionicNdkBindings *b=(void*)(uintptr_t)raw;if(ready)return(uint64_t)-16;if(!b||b->size!=sizeof(*b)||b->version!=KLJ_BIONIC_NDK_ABI||b->count!=${functions.length}||!b->targets)return(uint64_t)-22;`;
guest+=functions.map((f,i)=>`if(b->targets[${i}].target!=${i+1}||b->targets[${i}].argument_size!=sizeof(Args_${i}))return(uint64_t)-22;`).join('\n')+'int expected=0;if(!atomic_compare_exchange_strong(&ready,&expected,2))return(uint64_t)-16;memcpy(targets,b->targets,sizeof(targets));atomic_store_explicit(&ready,1,memory_order_release);return 0;}\n';
guest+='static const char *names[]={'+functions.map(f=>JSON.stringify(f.name)).join(',')+'};\nstatic const __eglMustCastToProperFunctionPointerType veneers[]={'+functions.map(f=>`(__eglMustCastToProperFunctionPointerType)${f.name}`).join(',')+'};\n';
guest+=lookupTable;
guest+='static __eglMustCastToProperFunctionPointerType procedure(const char *name){if(!name)return NULL;size_t lo=0,hi=sizeof(lookup_indices)/sizeof(*lookup_indices);while(lo<hi){size_t mid=lo+(hi-lo)/2,i=lookup_indices[mid];int compare=strcmp(name,names[i]);if(!compare)return veneers[i];if(compare<0)hi=mid;else lo=mid+1;}return NULL;}\n';
functions.forEach((f,i)=>{
 guest+=`${f.result} ${f.name}(${f.parameters.map((t,j)=>`${t} a${j}`).join(',')||'void'}){if(atomic_load_explicit(&ready,memory_order_acquire)!=1)abort();Args_${i} args={${f.parameters.map((_,j)=>`.a${j}=a${j},`).join('')}};KljBionicNdkPacket packet={sizeof(packet),KLJ_BIONIC_NDK_ABI,targets[${i}].target,(uintptr_t)&args,sizeof(args),(uintptr_t)__builtin_return_address(0),errno,-1};if(host_call(&packet)<0)abort();errno=packet.error;`;
 guest+=f.name==='eglGetProcAddress'?'return args.result?procedure(a0):NULL;':f.result==='void'?'':'return args.result;';
 guest+='}\n';
});
fs.writeFileSync(path.join(out,'egl-guest-generated.c'),guest);
fs.writeFileSync(path.join(out,'declarations.json'),JSON.stringify({source:path.basename(source),sha256:hash(source),target:ast.target,angleDeclarations:ast.angleDeclarations,headers:headers.map(name=>({name,sha256:hash(path.join(root,'include',name))})),functions},null,2)+'\n');
console.log(JSON.stringify({functions:functions.length,egl:functions.filter(f=>f.name.startsWith('egl')).length,gles:functions.filter(f=>f.name.startsWith('gl')).length,missingCallbackCarrier:functions.filter(f=>f.callbacks.length&&!debugCallbacks.has(f.name)&&f.name!=='glBlobCacheCallbacksANGLE').map(f=>f.name),missingFdTransfer:functions.filter(f=>f.fd).map(f=>f.name),returnedOpaqueProcedure:functions.filter(f=>f.functionResult&&f.name!=='eglGetProcAddress').map(f=>f.name)}));
