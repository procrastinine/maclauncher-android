import fs from 'node:fs';
import path from 'node:path';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';

export async function generateGraphicsAidl(ctx) {
const exec=promisify(execFile),root=ctx.work;
const compiler=ctx.tools.aidl;
const results=[];
const interfaces=[
 ['common','android.hardware.common',2],
 ['graphics/common','android.hardware.graphics.common',6],
 ['drm/common','android.hardware.drm.common',1],
 ['graphics/composer','android.hardware.graphics.composer3',4],
];
const roots=interfaces.map(([dir,name,version])=>path.join(root,'module/source/hardware-interfaces',dir,'aidl/aidl_api',name,String(version)));
for(const [number,[,name,version]]of interfaces.entries()){
 const base=roots[number],files=fs.globSync('**/*.aidl',{cwd:base});
 const hash=fs.readFileSync(path.join(base,'.hash'),'utf8').trim().split('\n').at(-1);
 const flags=['--lang=ndk','--structured','--stability=vintf','--version='+version,'--hash='+hash,...roots.map(p=>'-I'+p),
  '-h',path.join(root,'module/gen/include'),'-o',path.join(root,'module/gen/src')];
 for(let index=0;index<files.length;index+=4){
  results.push(...await Promise.all(files.slice(index,index+4).map(async file=>{
   try{const {stderr}=await exec(compiler,[...flags,path.join(base,file)],{timeout:15000});return {interface:name,file,status:0,stderr};}
   catch(error){return {interface:name,file,status:error.code,stderr:error.stderr||error.message};}
  })));
 }
}
fs.writeFileSync(path.join(root,'graphics-aidl-result.json'),JSON.stringify({compiler,interfaces,roots,results},null,2)+'\n');
console.log(JSON.stringify({inputs:results.length,passed:results.filter(r=>r.status===0).length,failed:results.filter(r=>r.status!==0)},null,2));
if(results.some(r=>r.status!==0))throw Error("Original HWUI interface generation failed");

}
