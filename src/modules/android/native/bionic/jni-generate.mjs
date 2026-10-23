import fs from 'node:fs';
import path from 'node:path';
import {generate as originalGenerate,jniDeclarations,jniCallOperations} from '../loader-generate-jni-calls.mjs';
const identifier=declaration=>{const match=declaration.match(/([A-Za-z_]\w*)$/);if(!match)throw Error('JNI parameter name missing: '+declaration);return match[1];};
export function generate(output,jdk){
 fs.mkdirSync(output,{recursive:true});originalGenerate(output,jdk);
 const fields=jniDeclarations(jdk),vm=jniDeclarations(jdk,'JNIInvokeInterface_');
 const variants=jniCallOperations().flatMap(op=>[op.name,op.name+'V']);
 const variable=new Map(variants.map((name,index)=>[name,index]));
 let declarations='#pragma once\n#include "jni-carrier.h"\n';
 let guest='#include "bionic-jni-generated.h"\n#include <string.h>\nstatic const KljBionicJniBindings *bindings;\nstatic int64_t send(KljBionicJniPacket *packet){register uint64_t x0 __asm__("x0")=0x4d4c,x1 __asm__("x1")=KLJ_BIONIC_JNI_HOSTCALL,x2 __asm__("x2")=(uintptr_t)packet,x3 __asm__("x3")=sizeof(*packet),x8 __asm__("x8")=0x48430000;__asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x8):"x4","x5","x6","x7","x9","x10","x11","x12","x13","x14","x15","x16","x17","x30","memory","cc");return(int64_t)x0;}\n';
 let host='#include "bionic-jni-generated.h"\n#include <string.h>\nextern void *klj_jni_dispatch(KljBionicJniVariadicFrame *,unsigned);\nstatic int64_t variadic(void *raw){KljBionicJniVariadicFrame *frame=raw;frame->lr=klj_bionic_jni_caller();frame->x[0]=(uintptr_t)klj_bionic_jni_host_environment((JNIEnv*)(uintptr_t)frame->x[0]);if(!frame->x[0])return -22;klj_jni_dispatch(frame,(unsigned)frame->operation);return 0;}\n';
 const target={jni:[],vm:[]};let guestFields='',guestVM='';
 function emit(field,index,group){
  const {name,result,parameters}=field,key=group+'_'+index;
  if(group==='jni'&&variable.has(name)){
   guest+=`extern void klj_bionic_jni_call_${variable.get(name)}(void);\n`;
   guestFields+=`.${name}=(__typeof__(((struct JNINativeInterface_*)0)->${name}))klj_bionic_jni_call_${variable.get(name)},\n`;
   target.jni.push('{(uintptr_t)variadic,sizeof(KljBionicJniVariadicFrame)}');return;
  }
  const names=parameters.map(identifier),first=names[0];
  declarations+=`typedef struct {${parameters.map(p=>p+';').join('')}${result==='void'?'':result+' klj_result;'}uintptr_t caller;} Payload_${key};\n`;
  const payload=`Payload_${key}`,initial=names.map(n=>'.'+n+'='+n).join(',');
  guest+=`static ${result} JNICALL carrier_${key}(${parameters.join(',')}){${payload} call={${initial},.caller=(uintptr_t)__builtin_return_address(0)};KljBionicJniPacket packet={sizeof packet,KLJ_BIONIC_JNI_ABI,bindings->${group}[${index}].function,(uintptr_t)&call,sizeof call,call.caller};${group==='vm'?'if(send(&packet)!=0)return JNI_ERR;':'send(&packet);'}${result==='void'?'':'return call.klj_result;'}}\n`;
  (group==='jni'?()=>{guestFields+=`.${name}=carrier_${key},\n`;}:()=>{guestVM+=`.${name}=carrier_${key},\n`;})();
  let invoke=`(*inner)->${name}(inner${names.length>1?','+names.slice(1).map(n=>'call->'+n).join(','):''})`;
  let pre=`${group==='jni'?'JNIEnv':'JavaVM'} *inner=klj_bionic_jni_host_${group==='jni'?'environment':'vm'}(call->${first});if(!inner)return -22;`;
  let post='';
  if(group==='jni'&&name==='FindClass')invoke='klj_bionic_jni_find_class(inner,call->name,call->caller)';
  if(group==='jni'&&name==='GetJavaVM')post='if(call->klj_result==JNI_OK)*call->vm=klj_bionic_jni_wrap_vm(*call->vm);';
  if(group==='vm'&&(name==='AttachCurrentThread'||name==='AttachCurrentThreadAsDaemon')){
   pre+='KljJniAttachScope scope;klj_jni_attach_scope_enter(&scope,klj_bionic_jni_prepare_attach,call->vm);';
   post='klj_jni_attach_scope_leave(&scope);if(call->klj_result==JNI_OK)*call->penv=klj_bionic_jni_wrap_environment(*call->penv);';
  }
  if(group==='vm'&&name==='GetEnv')post='if(call->klj_result==JNI_OK&&((uint32_t)call->version&0x70000000u)==0)*call->penv=klj_bionic_jni_wrap_environment(*call->penv);';
  host+=`static int64_t host_${key}(void *raw){${payload} *call=raw;call->caller=klj_bionic_jni_caller();${pre}${result==='void'?invoke+';':'call->klj_result='+invoke+';'}${post}return 0;}\n`;
  target[group].push(`{(uintptr_t)host_${key},sizeof(${payload})}`);
 }
 fields.forEach((f,i)=>emit(f,i,'jni'));vm.forEach((f,i)=>emit(f,i,'vm'));
 guest+='static const struct JNINativeInterface_ guest_jni={'+guestFields+'};\nstatic const struct JNIInvokeInterface_ guest_vm={'+guestVM+'};\nstatic const KljBionicJniGuestTables tables={&guest_jni,&guest_vm};\n';
 guest+=`uint64_t klj_bionic_jni_guest_install(uint64_t raw){const KljBionicJniBindings *value=(void*)(uintptr_t)raw;if(!value||value->size!=sizeof(*value)||value->version!=KLJ_BIONIC_JNI_ABI||value->jni_count!=${fields.length}||value->vm_count!=${vm.length}||!value->jni||!value->vm)return(uint64_t)(int64_t)-22;for(unsigned i=0;i<value->jni_count;i++)if(!value->jni[i].function)return(uint64_t)(int64_t)-22;for(unsigned i=0;i<value->vm_count;i++)if(!value->vm[i].function)return(uint64_t)(int64_t)-22;bindings=value;return 0;}\nuint64_t klj_bionic_jni_guest_tables(uint64_t unused){(void)unused;return(uintptr_t)&tables;}\n`;
 guest+='void klj_bionic_jni_variadic_send(KljBionicJniVariadicFrame *frame,unsigned operation){\nstatic const unsigned indices[]={'+variants.map(n=>fields.findIndex(f=>f.name===n)).join(',')+'};\nif(operation>=sizeof(indices)/sizeof(*indices))return;KljBionicJniPacket packet={sizeof packet,KLJ_BIONIC_JNI_ABI,bindings->jni[indices[operation]].function,(uintptr_t)frame,sizeof(*frame),frame->lr};send(&packet);}\n';
 host+='static const KljBionicJniTarget jni_targets[]={'+target.jni.join(',')+'};\nstatic const KljBionicJniTarget vm_targets[]={'+target.vm.join(',')+'};\n';
 host+='static const KljBionicJniBindings bindings={sizeof bindings,KLJ_BIONIC_JNI_ABI,sizeof(jni_targets)/sizeof(*jni_targets),sizeof(vm_targets)/sizeof(*vm_targets),jni_targets,vm_targets};\nconst KljBionicJniBindings *klj_bionic_jni_host_bindings(void){return &bindings;}\n';
 host+='int64_t klj_bionic_jni_dispatch_packet(KljBionicJniPacket *packet){if(!packet||packet->size!=sizeof(*packet)||packet->version!=KLJ_BIONIC_JNI_ABI||!packet->argument)return -22;const KljBionicJniTarget *groups[]={jni_targets,vm_targets};const size_t counts[]={sizeof(jni_targets)/sizeof(*jni_targets),sizeof(vm_targets)/sizeof(*vm_targets)};for(unsigned g=0;g<2;g++)for(size_t i=0;i<counts[g];i++){if(packet->target!=groups[g][i].function||packet->argument_size!=groups[g][i].argument_size)continue;int64_t(*function)(void*)=(void*)(uintptr_t)packet->target;return function((void*)(uintptr_t)packet->argument);}return -22;}\n';
 let assembly=fs.readFileSync(path.join(output,'jni-calls.S'),'utf8').replaceAll('_klj_jni_call_','klj_bionic_jni_call_').replaceAll('Lklj_jni_call','Lklj_bionic_jni_call').replace('_klj_jni_dispatch','klj_bionic_jni_variadic_send');
 assembly+='\n.section .note.GNU-stack,"",%progbits\n';
 for(const [name,text]of[['bionic-jni-generated.h',declarations],['bionic-jni-guest.c',guest],['bionic-jni-host.c',host],['bionic-jni-calls.S',assembly]])fs.writeFileSync(path.join(output,name),text);
 fs.writeFileSync(path.join(output,'jni-carrier-manifest.json'),JSON.stringify({jni:fields.map(f=>f.name),vm:vm.map(f=>f.name),variadic:variants,allDeclarationsRetained:true},null,2)+'\n');
}
