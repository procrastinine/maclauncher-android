import fs from 'node:fs';import path from 'node:path';import crypto from 'node:crypto';
const work=import.meta.dirname,output=path.resolve(process.argv[3]||path.join(work,'generated'));
const source=path.resolve(process.argv[2]||(()=>{throw Error('Expected the original Clang host AST and generated output directory');})());
fs.mkdirSync(output,{recursive:true});
const ast=JSON.parse(fs.readFileSync(source)),headers=['bitmap.h','hardware_buffer.h','hardware_buffer_jni.h','hardware_buffer_aidl.h','native_window.h','native_window_jni.h','native_window_aidl.h'];
const declarations=headers.map(name=>fs.readFileSync(path.join(work,'include/android',name),'utf8')).join('\n');
const functions=Object.keys(ast.functions).filter(name=>/^(AndroidBitmap|AHardwareBuffer|ANativeWindow)/.test(name)&&new RegExp('\\b'+name+'\\s*\\(').test(declarations)).sort().map(name=>{const type=ast.types[ast.functions[name].type];if(type.variadic)throw Error('Unexpected varargs in original NDK graphics API');return{name,result:type.result,parameters:type.parameters};});
if(functions.length!==42)throw Error('Original declared graphics family changed: '+functions.length);
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
// The original header defines clearFrameRate inline. Keep that complete header
// under a local alias so the public carrier can also provide its typed symbol.
const base='#include "graphics-host.h"\n#include <android/bitmap.h>\n#include <android/hardware_buffer.h>\n#include <android/hardware_buffer_jni.h>\n#include <android/hardware_buffer_aidl.h>\n#define ANativeWindow_clearFrameRate klj_header_clearFrameRate\n#include <android/native_window.h>\n#include <android/native_window_jni.h>\n#include <android/native_window_aidl.h>\n#undef ANativeWindow_clearFrameRate\n';
const fields=functions.map((f,i)=>`typedef struct {${f.parameters.map((t,j)=>`${t} a${j};`).join('')}${f.result==='void'?'':`${f.result} result;`}} GraphicsArgs_${i};`).join('\n');
fs.writeFileSync(path.join(output,'graphics-arguments.h'),base+fields+'\n');
let host='#include "graphics-arguments.h"\n#include <errno.h>\n#include <pthread.h>\n#include <stdatomic.h>\n#include <stdio.h>\n#include <string.h>\nextern int kl_errno_to_linux(int),kl_errno_from_linux(int);\nstatic KljBionicGraphicsOwner owner;static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;static _Atomic int ready;\n';
host+=`typedef struct {JNIEnv *env;uintptr_t caller;void *user;AndroidBitmap_CompressWriteFunc callback;} Compression;
static bool write_compressed(void *raw,const void *data,size_t size){
 Compression *c=raw;int saved=errno;KljCallPacket packet={.size=sizeof packet,.version=KLJ_CALL_PACKET_VERSION,.target=(uintptr_t)c->callback,.x={(uintptr_t)c->user,(uintptr_t)data,size}};
 int64_t status=owner.invoke(owner.context,c->env,c->caller,&packet);errno=saved;
 return !status&&!packet.status&&(!c->env||!(*c->env)->ExceptionCheck(c->env))&&(uint8_t)packet.result_x[0]!=0;
}
static int64_t unavailable(const char *name,int marshalling){fprintf(stderr,"Android NDK operation unavailable: %s (%s)\\n",name,marshalling?"descriptor ownership bridge":"native provider");return -38;}
`;
const fdInputs={AHardwareBuffer_lock:2,AHardwareBuffer_lockPlanes:2,AHardwareBuffer_lockAndGetInfo:2,AHardwareBuffer_recvHandleFromUnixSocket:0,AHardwareBuffer_sendHandleToUnixSocket:1};
functions.forEach((f,i)=>{
 host+=`static ${f.result} (*target_${i})(${f.parameters.join(',')||'void'});\nstatic int64_t invoke_${i}(JNIEnv *env,KljBionicNdkPacket *packet){GraphicsArgs_${i} *a=(void*)(uintptr_t)packet->arguments;if(!target_${i})return unavailable(${JSON.stringify(f.name)},0);`;
 if(f.parameters[0]==='JNIEnv *')host+='if(!env)return -22;a->a0=owner.environment(owner.context,env,a->a0);if(!a->a0||a->a0!=env)return -22;';
 if(f.name==='AndroidBitmap_compress')host+=`if(a->a6&&!owner.invoke)return unavailable("AndroidBitmap_compress callback",0);Compression c={env,packet->caller,a->a5,a->a6};a->result=target_${i}(a->a0,a->a1,a->a2,a->a3,a->a4,&c,a->a6?write_compressed:NULL);`;
 else if(f.name==='AHardwareBuffer_unlock')host+=`if(a->a1)return unavailable(${JSON.stringify(f.name)},1);a->result=target_${i}(a->a0,NULL);`;
 else if(f.name in fdInputs){let p=fdInputs[f.name];host+=`if(a->a${p}>=0)return unavailable(${JSON.stringify(f.name)},1);${f.result==='void'?'':'a->result='}target_${i}(${f.parameters.map((_,j)=>`a->a${j}`).join(',')});`;}
 else host+=`${f.result==='void'?'':'a->result='}target_${i}(${f.parameters.map((_,j)=>`a->a${j}`).join(',')});`;
 host+='return 0;}\n';
});
host+='static const KljBionicNdkTarget targets[]={\n'+functions.map((f,i)=>`{(uintptr_t)invoke_${i},sizeof(GraphicsArgs_${i})},`).join('\n')+'\n};\nstatic const KljBionicNdkBindings bindings={sizeof(bindings),KLJ_BIONIC_NDK_ABI,sizeof(targets)/sizeof(*targets),0,targets};\nconst KljBionicNdkBindings *klj_bionic_graphics_bindings(void){return ready?&bindings:NULL;}\n';
host+='int klj_bionic_graphics_prepare(const KljBionicGraphicsOwner *input){if(!input||!input->resolve||!input->environment)return -22;pthread_mutex_lock(&lock);if(ready){int same=owner.resolve==input->resolve&&owner.environment==input->environment&&owner.invoke==input->invoke&&owner.context==input->context;pthread_mutex_unlock(&lock);return same?0:-16;}\n';
host+=functions.map((f,i)=>`void *p${i}=input->resolve(${JSON.stringify(f.name)});memcpy(&target_${i},&p${i},sizeof(target_${i}));`).join('\n')+'\nowner=*input;ready=1;pthread_mutex_unlock(&lock);return 0;}\n';
host+='int64_t klj_bionic_graphics_dispatch(JNIEnv *env,KljBionicNdkPacket *packet){if(!ready||!packet||packet->size!=sizeof(*packet)||packet->version!=KLJ_BIONIC_NDK_ABI||!packet->arguments)return -22;for(unsigned i=0;i<bindings.count;i++){if(packet->target!=targets[i].target)continue;if(packet->argument_size!=targets[i].argument_size)return -22;int saved=errno;errno=kl_errno_from_linux(packet->error);int64_t status=((int64_t(*)(JNIEnv*,KljBionicNdkPacket*))(uintptr_t)packet->target)(env,packet);packet->error=kl_errno_to_linux(errno);errno=saved;return status;}return -1;}\n';
fs.writeFileSync(path.join(output,'graphics-host-generated.c'),host);
let guest='#include "graphics-arguments.h"\n#include <errno.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <stdatomic.h>\n#include <string.h>\n'+`static KljBionicNdkTarget targets[${functions.length}];static _Atomic int ready;\n`;
guest+='static int64_t host_call(KljBionicNdkPacket *packet){register uint64_t x0 __asm__("x0")=0x4d4c,x1 __asm__("x1")=KLJ_BIONIC_GRAPHICS_HOSTCALL,x2 __asm__("x2")=(uintptr_t)packet,x3 __asm__("x3")=sizeof(*packet),x8 __asm__("x8")=0x48430000;__asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x8):"x4","x5","x6","x7","x9","x10","x11","x12","x13","x14","x15","x16","x17","x30","memory","cc");return (int64_t)x0;}\n';
guest+=`uint64_t klj_bionic_graphics_install(uint64_t raw){const KljBionicNdkBindings *b=(void*)(uintptr_t)raw;if(ready)return (uint64_t)-16;if(!b||b->size!=sizeof(*b)||b->version!=KLJ_BIONIC_NDK_ABI||b->count!=${functions.length}||!b->targets)return (uint64_t)-22;\n`;
guest+=functions.map((f,i)=>`if(!b->targets[${i}].target||b->targets[${i}].argument_size!=sizeof(GraphicsArgs_${i}))return (uint64_t)-22;`).join('\n')+'\nint expected=0;if(!atomic_compare_exchange_strong(&ready,&expected,2))return (uint64_t)-16;memcpy(targets,b->targets,sizeof(targets));ready=1;return 0;}\n';
functions.forEach((f,i)=>{guest+=`${f.result} ${f.name}(${f.parameters.map((t,j)=>`${t} a${j}`).join(',')||'void'}){if(ready!=1)abort();GraphicsArgs_${i} args={${f.parameters.map((_,j)=>`.a${j}=a${j},`).join('')}};KljBionicNdkPacket packet={sizeof(packet),KLJ_BIONIC_NDK_ABI,targets[i].target,(uintptr_t)&args,sizeof(args),(uintptr_t)__builtin_return_address(0),errno,-1};if(host_call(&packet)<0){fprintf(stderr,"Android NDK call failed: ${f.name}\\n");abort();}errno=packet.error;${f.result==='void'?'':'return args.result;'}}\n`.replace('targets[i]',`targets[${i}]`);});
fs.writeFileSync(path.join(output,'graphics-guest-generated.c'),guest);
fs.writeFileSync(path.join(output,'declarations.json'),JSON.stringify({source,sha256:hash(source),target:ast.target,headers:headers.map(name=>({name,sha256:hash(path.join(work,'include/android',name))})),functions,pendingDescriptorMarshaller:Object.keys(fdInputs).concat('AHardwareBuffer_unlock'),noGlobalAvailabilityGate:true},null,2)+'\n');
console.log(JSON.stringify({functions:functions.length,bitmap:functions.filter(f=>f.name.startsWith('AndroidBitmap')).length,output}));
