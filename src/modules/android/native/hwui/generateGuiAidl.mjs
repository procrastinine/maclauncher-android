import fs from 'node:fs';
import path from 'node:path';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {parseBlueprint} from '../upstream/blueprint.mjs';

export async function generateGuiAidl(ctx) {
const exec=promisify(execFile),root=ctx.work,gui=path.join(root,'module/source/native/libs/gui');
const parser=ctx.tools.blueprintParser;
const module=parseBlueprint(fs.readFileSync(path.join(gui,'Android.bp'),'utf8'),{file:'libs/gui/Android.bp',parser}).modules.find(m=>m.properties.name==='libgui_aidl').properties;
const files=[...new Set(module.srcs.flatMap(pattern=>fs.globSync(pattern,{cwd:gui})))];
const compiler=ctx.tools.aidl;
const flags=['--lang=cpp','-I'+path.join(gui,'aidl'),'-I'+gui,'-I'+path.join(root,'module/source/native/libs/binder/aidl'),
 '-h',path.join(root,'module/gen/include'),'-o',path.join(root,'module/gen/src')];
const results=[];
for(let index=0;index<files.length;index+=4){
 const batch=await Promise.all(files.slice(index,index+4).map(async file=>{
  try{const {stderr}=await exec(compiler,[...flags,path.join(gui,file)],{timeout:15000});return {file,status:0,stderr};}
  catch(error){return {file,status:error.code,stderr:error.stderr||error.message};}
 }));results.push(...batch);
}
fs.writeFileSync(path.join(root,'gui-aidl-result.json'),JSON.stringify({compiler,flags,results},null,2)+'\n');
console.log(JSON.stringify({inputs:files.length,passed:results.filter(r=>r.status===0).length,failed:results.filter(r=>r.status!==0)},null,2));
if(results.some(r=>r.status!==0))throw Error("Original HWUI interface generation failed");

}
