import fs from 'node:fs';import path from 'node:path';
import {writeChanged} from './upstream/source-files.mjs';
export function generate(output,jdk){
 const operations=jniCallOperations();
 let header='',assembly=`.text
.p2align 2
Lklj_jni_call:
 sub sp,sp,#256
 stp x0,x1,[sp,#0]
 stp x2,x3,[sp,#16]
 stp x4,x5,[sp,#32]
 stp x6,x7,[sp,#48]
 stp x8,x30,[sp,#64]
 stp q0,q1,[sp,#80]
 stp q2,q3,[sp,#112]
 stp q4,q5,[sp,#144]
 stp q6,q7,[sp,#176]
 add x9,sp,#256
 str x9,[sp,#208]
 str x16,[sp,#232]
 mov x0,sp
 mov x1,x16
 bl _klj_jni_dispatch

 ldr x30,[sp,#72]
 ldr x0,[sp,#216]
 ldr d0,[sp,#224]
 add sp,sp,#256
 ret
`;
 const variants=operations.flatMap(o=>[{...o,isV:false},{...o,isV:true}]);
 variants.forEach((o,i)=>{header+=`extern void klj_jni_call_${i}(void);\n`;assembly+=`.p2align 2\n.globl _klj_jni_call_${i}\n_klj_jni_call_${i}:\n mov x16,#${i}\n b Lklj_jni_call\n`;});
 header+='static struct JNINativeInterface_ guest_functions;\nint klj_jni_calls_prepare(JNIEnv *env,jniNativeInterface *table,jvmtiEnv *ti){if(!prepare_call_lifecycle(env,ti)||!prepare_native_load_methods(env,table))return 0;originals=*table;prepare_guest_functions();\n';
 variants.forEach((o,i)=>{const field=o.name+(o.isV?'V':'');header+=`guest_functions.${field}=(__typeof__(guest_functions.${field}))klj_jni_call_${i};\n`;});
 header+='return 1;}\nvoid *klj_jni_dispatch(CallFrame *f,unsigned operation){\nif(operation>=62)return NULL;\nf->integer=f->floating=0;JNIEnv *env=klj_host_env((JNIEnv*)f->x[0]);if((*env)->ExceptionCheck(env))return NULL;\nunsigned fixed=(operation>=42)?4:3;jmethodID method=(jmethodID)f->x[fixed-1];Method descriptor;\nif(!signature(env,method,&descriptor))return NULL;jvalue values[255]={{0}};if(!decode(env,f,fixed,operation&1,&descriptor,values))return NULL;\nswitch(operation/2){\n';
 operations.forEach((o,i)=>{let statement=o.kind==='Void'?o.invoke+';':o.kind==='Float'?`jfloat value=${o.invoke};memcpy(&f->floating,&value,sizeof value);`:o.kind==='Double'?`jdouble value=${o.invoke};memcpy(&f->floating,&value,sizeof value);`:`f->integer=(uint64_t)${o.invoke};`;header+=`case ${i}:{${statement}break;}\n`;});
 header=header.replace('switch(operation/2){','if(!klj_jni_before_java(env))return NULL;switch(operation/2){');
 header+='}\nklj_jni_after_java(env);(*env)->ExceptionCheck(env);return NULL;}\n';
 const fields=jniDeclarations(jdk);
 const variable=new Set(variants.map(o=>o.name+(o.isV?'V':'')));
 let forward='static struct JNINativeInterface_ guest_functions;\n';
 for(const {name,result,parameters}of fields){if(variable.has(name))continue;
  const args=parameters.slice(1).map(p=>{const match=p.match(/([A-Za-z_]\w*)$/);if(!match)throw Error('JNI parameter declaration is incomplete: '+p);return match[1];});
  const void_family={CallVoidMethodA:0,CallStaticVoidMethodA:1,CallNonvirtualVoidMethodA:2};
  const invoke=Object.hasOwn(void_family,name)?'invoke_void(klj_host_env(env),'+(name==='CallStaticVoidMethodA'?'NULL':args[0])+','+(name==='CallVoidMethodA'?'NULL':name==='CallStaticVoidMethodA'?args[0]:args[1])+','+args.slice(-2).join(',')+', (uintptr_t)__builtin_return_address(0),'+void_family[name]+')':name==='GetJavaVM'?'guest_get_vm(env,vm)':name==='FindClass'?'guest_find_class(env,name,__builtin_return_address(0))':'originals.'+name+'(klj_host_env(env)'+(args.length?','+args.join(','):'')+')';
  const reenters=/^(FindClass|DefineClass|AllocObject|Get(?:Static)?(?:Method|Field)ID|ExceptionDescribe|NewObjectA|Call(?:Static|Nonvirtual)?\w+MethodA)$/.test(name);
  forward+='static '+result+' JNICALL guest_'+name+'('+parameters.join(',')+'){'+(reenters?
    'JNIEnv *host=klj_host_env(env);if(!klj_jni_before_java(host))'+(result==='void'?'return;':'return ('+result+')0;')+
    (result==='void'?invoke+';':result+' value='+invoke+';')+'klj_jni_after_java(host);'+(result==='void'?'':'return value;'):
    (result==='void'?'':'return ')+invoke+';')+'}\n';
 }
 forward+='static void prepare_guest_functions(void){guest_functions=originals;\n';
 for(const {name}of fields)if(!variable.has(name))forward+='guest_functions.'+name+'=guest_'+name+';\n';
 forward+='}\n';
 writeChanged(path.join(output,'jni-forward-generated.h'),forward);
 writeChanged(path.join(output,'jni-calls-generated.h'),header);writeChanged(path.join(output,'jni-calls.S'),assembly);
}

export function jniCallOperations(){
 const operations=[{name:'NewObject',kind:'Object',fixed:3,invoke:'originals.NewObjectA(env,(jclass)f->x[1],method,values)'}];
 for(const group of ['','Static','Nonvirtual'])for(const kind of ['Object','Boolean','Byte','Char','Short','Int','Long','Float','Double','Void']){
  const name='Call'+group+kind+'Method',fixed=group==='Nonvirtual'?4:3;
  const args=group==='Nonvirtual'?'env,(jobject)f->x[1],(jclass)f->x[2],method,values':`env,(${group==='Static'?'jclass':'jobject'})f->x[1],method,values`;
  const invoke=kind==='Void'?`invoke_void(env,${group==='Static'?'NULL':'(jobject)f->x[1]'},${group==='Nonvirtual'?'(jclass)f->x[2]':group==='Static'?'(jclass)f->x[1]':'NULL'},method,values,(uintptr_t)f->lr,${group==='Static'?1:group==='Nonvirtual'?2:0})`:'originals.'+name+'A('+args+')';
  operations.push({name,kind,fixed,invoke});
 }
 return operations;
}

export function jniDeclarations(jdk,table="JNINativeInterface_"){
 const declaration=fs.readFileSync(path.join(jdk,'include/jni.h'),'utf8').split('struct '+table+' {')[1]?.split('\n};')[0];
 if(!declaration)throw Error('JNI function declarations are missing');
 const fields=[...declaration.matchAll(/([\w\s*]+?)\(JNICALL\s*\*(\w+)\)\s*\(([\s\S]*?)\)\s*;/g)].map(m=>({result:m[1].trim().replace(/\s+/g,' '),name:m[2],parameters:m[3].split(',').map(s=>s.trim().replace(/\s+/g,' '))}));
 if(fields.length!==[...declaration.matchAll(/JNICALL/g)].length)throw Error('JNI declaration parsing is incomplete');
 return fields;
}
