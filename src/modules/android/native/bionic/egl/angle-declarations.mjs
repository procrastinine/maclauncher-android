import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const root=import.meta.dirname,header=path.join(root,'include/GLES2/gl2ext_angle.h');
const hash=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const names=text=>new Set([...text.matchAll(/\b(gl[A-Z]\w*)\s*\(/g)].map(m=>m[1]));
const parameter=name=>name.replace(/\[[^\]]*\]$/,' *');
const shape=value=>Object.fromEntries(['kind','size','alignment','result','parameters','element','count','variadic'].filter(k=>value[k]!==undefined).map(k=>[k,k==='parameters'?value[k].map(parameter):value[k]]));
/** Append the original ANGLE extension declarations without changing NDK ABI. */
export function mergeAngleDeclarations(original){
 const supplement=JSON.parse(fs.readFileSync(path.join(root,'angle-declarations.json')));
 if(supplement.target!==original.target||supplement.header.sha256!==hash(header))throw Error('ANGLE declaration supplier changed; regenerate its compiler snapshot');
 const types={...original.types},functions={...original.functions};
 for(const [key,value]of Object.entries(supplement.types)){
  if(types[key]&&JSON.stringify(shape(types[key]))!==JSON.stringify(shape(value)))throw Error('Original ANGLE/NDK ABI differs for '+key);
  if(!types[key])types[key]=value;
 }
 for(const [key,value]of Object.entries(supplement.functions)){
  if(functions[key]&&functions[key].type!==value.type)throw Error('Original ANGLE/NDK signature differs for '+key);
  if(!functions[key])functions[key]=value;
 }
 return {...original,types,functions,angleDeclarations:{header:supplement.header,snapshotSha256:hash(path.join(root,'angle-declarations.json')),extractorSha256:supplement.extractorSha256,functions:Object.keys(supplement.functions).sort()}};
}
/** Rebuild the snapshot from the complete original header with the NDK extractor. */
export function generateAngleDeclarations({include,python,extractor=path.resolve(root,'../../arm32/imports-ast.py'),output=path.join(root,'angle-declarations.json')}){
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-angle-declarations-'));
 try{
  const input=path.join(temporary,'input.json'),result=path.join(temporary,'result.json'),target='aarch64-linux-android36';
  const prelude='#include <stddef.h>\n#include <stdint.h>\n#include <GLES3/gl32.h>\n#include <GLES2/gl2ext.h>\n';
  fs.writeFileSync(input,JSON.stringify({include:path.resolve(include),target,headers:[header],headerPrelude:{[header]:prelude},arguments:['-DGL_GLEXT_PROTOTYPES']}));
  execFileSync(python,[extractor,input,result],{stdio:'inherit'});
  const ast=JSON.parse(fs.readFileSync(result));
  if(ast.headers.some(row=>row.error||row.errors?.length))throw Error('Clang could not read original ANGLE extension declarations: '+JSON.stringify(ast.headers));
  const declared=names(fs.readFileSync(header,'utf8')),functions=Object.fromEntries([...declared].sort().map(name=>{if(!ast.functions[name])throw Error('Original ANGLE declaration missing: '+name);return[name,ast.functions[name]];}));
  const types={},pending=Object.values(functions).map(fn=>fn.type);
  while(pending.length){const key=pending.pop();if(types[key])continue;const type=ast.types[key];if(!type)throw Error('Original ANGLE type missing: '+key);types[key]=type;pending.push(...[type.element,type.result,...(type.parameters||[]),...(type.fields||[]).map(field=>field.type)].filter(Boolean));}
  for(const type of Object.values(types))if(type.declaration?.file===header)type.declaration.file='<angle>/GLES2/gl2ext_angle.h';
  const value={schemaVersion:1,target,header:{path:'GLES2/gl2ext_angle.h',sha256:hash(header)},extractorSha256:hash(extractor),types,functions};
  fs.writeFileSync(output,JSON.stringify(value,null,2)+'\n');return value;
 }finally{fs.rmSync(temporary,{recursive:true,force:true});}
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)){
 const options={};for(let i=2;i<process.argv.length;i+=2){const key=process.argv[i].replace(/^--/,'');if(!['include','python','extractor','output'].includes(key)||!process.argv[i+1])throw Error('Expected --include, --python and optional --extractor/--output');options[key]=process.argv[i+1];}
 if(!options.include||!options.python)throw Error('Expected --include and --python');const value=generateAngleDeclarations(options);console.log(JSON.stringify({functions:Object.keys(value.functions).length,header:value.header,target:value.target}));
}
