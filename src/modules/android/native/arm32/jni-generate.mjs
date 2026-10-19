import fs from 'node:fs';import path from 'node:path';
import {writeChanged} from '../upstream/source-files.mjs';
/** Original VM table order and C types are the complete input. Native pointer
 * representations and variadic families are the only specialized boundaries. */
export function generateJni({jdk,header:headerFile,output}) {
  const header=fs.readFileSync(headerFile||path.join(jdk,'include/jni.h'),'utf8').replace(/\/\*[\s\S]*?\*\//g,'');
  const table=header.match(/struct JNINativeInterface_?\s*\{([\s\S]*?)\n};/)?.[1];
  if(!table)throw Error('JNI function table declaration is missing');
  const fields=[...table.matchAll(/(?:void\s*\*\s*(reserved\d+)\s*;|([\w\s*]+?)\(\s*(?:JNICALL\s*)?\*(\w+)\)\s*\(([\s\S]*?)\)\s*;)/g)].map(m=>m[1]?{reserved:m[1]}:{result:m[2].trim().replace(/\s+/g,' '),name:m[3],parameters:m[4].split(',').map(s=>s.trim().replace(/\s+/g,' '))});
  if(fields.length!==4+[...table.matchAll(/\(\s*(?:JNICALL\s*)?\*/g)].length)throw Error('JNI declaration parsing is incomplete');
  let code='// Generated from the complete original VM JNI declaration.\n';
  const metadata=[];
  fields.forEach((field,index)=>{
    if(field.reserved){code+=`slots[${index}]=0;\n`;return;}
    const {name,result}=field;const args=field.parameters.slice(1);
    metadata.push({index,...field});
    if(/^(NewObject|Call(?:Static|Nonvirtual)?(?:Object|Boolean|Byte|Char|Short|Int|Long|Float|Double|Void)Method)(V|A)?$/.test(name)){
      const root=name.replace(/[VA]$/,''),family=root==='NewObject'?'NewObject':root.replace(/^Call/,'').replace(/Method$/,'');
      code+=`slots[${index}]=gate([this](Thread&t){return method(t,MethodKind::${family},ArgumentKind::${name.endsWith('A')?'Array':name.endsWith('V')?'VaList':'Registers'});});\n`;return;
    }
    // These APIs own native allocations, handle frames, or tables containing
    // pointers. Their full families use explicit width/lifetime adaptations.
    const specialized=/^(GetJavaVM|RegisterNatives|UnregisterNatives|PushLocalFrame|PopLocalFrame|Delete(?:Local|Global|WeakGlobal)Ref|New(?:Global|WeakGlobal)Ref|GetString(?:UTF)?Chars|ReleaseString(?:UTF)?Chars|GetStringUTFRegion|GetStringCritical|ReleaseStringCritical|GetPrimitiveArrayCritical|ReleasePrimitiveArrayCritical|GetDirectBufferAddress|NewDirectByteBuffer|Get(?:Boolean|Byte|Char|Short|Int|Long|Float|Double)ArrayElements|Release(?:Boolean|Byte|Char|Short|Int|Long|Float|Double)ArrayElements)$/.test(name);
    if(specialized){code+=`slots[${index}]=gate([this](Thread&t){return ${name}(t);});\n`;return;}
    const parsed=args.map((arg,i)=>{const m=arg.match(/^(.+?(?:\s|\*))([A-Za-z_]\w*)$/);return{type:m?m[1].trim():arg,name:m?.[2],variable:'a'+i};});
    code+=`slots[${index}]=gate([this](Thread&t){Cursor c(*this,t);c.word();auto e=env();`;
    for(const a of parsed)code+=`auto ${a.variable}=c.take<${a.type}>();`;
    if(name==='DefineClass')code+='checkBuffer(a2,a3,Memory::Read);';
    if(name==='NewString')code+='checkBuffer(a0,a1,Memory::Read);';
    if(name==='GetStringRegion')code+='checkBuffer(a3,a2,Memory::Write);';
    if(/^(Get|Set)(Boolean|Byte|Char|Short|Int|Long|Float|Double)ArrayRegion$/.test(name))code+='checkBuffer(a3,a2,Memory::'+(name.startsWith('Get')?'Write':'Read')+');';
    const call=`e->functions->${name}(e${parsed.length?',':''}${parsed.map(a=>a.variable).join(',')})`;
    code+=result==='void'?`${call};return uint64_t(0);`:`return returned<${result}>(${call});`;
    code+='});\n';
  });
  const invocation=header.match(/struct JNIInvokeInterface_?\s*\{([\s\S]*?)\n};/)?.[1];
  if(!invocation)throw Error('JNI invocation table declaration is missing');
  const vmFields=[...invocation.matchAll(/void\s*\*\s*(reserved\d+)\s*;|\(\s*(?:JNICALL\s*)?\*(\w+)\)/g)].map(m=>m[1]?{reserved:m[1]}:{name:m[2]});
  if(vmFields.length!==3+[...invocation.matchAll(/\(\s*(?:JNICALL\s*)?\*/g)].length)throw Error('JNI invocation declaration parsing is incomplete');
  fs.mkdirSync(output,{recursive:true});
  writeChanged(path.join(output,'jni-invocation.inc'),vmFields.map((field,index)=>field.reserved?`invocation[${index}]=0;`:`invocation[${index}]=gate([this](Thread&t){return ${field.name}(t);});`).join('\n')+'\n');
  writeChanged(path.join(output,'jni-table.inc'),code);writeChanged(path.join(output,'jni-table.json'),JSON.stringify(metadata,null,2)+'\n');
  return {table:path.join(output,'jni-table.inc'),slots:fields.length};
}

if(process.argv[1]&&path.resolve(process.argv[1])===new URL(import.meta.url).pathname){
  if(!process.argv[2]||!process.argv[3])throw Error('Expected JDK and output paths');
  generateJni({jdk:path.resolve(process.argv[2]),output:path.resolve(process.argv[3])});
}
