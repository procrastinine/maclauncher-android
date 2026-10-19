import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
const require=createRequire(import.meta.url);

/** Inventory original provider APKs, without selecting classes or converting
 * their code. The original package parser owns component discovery at runtime. */
export function buildPlatformPackageCatalog({image,output}){
 const sources=[];
 function visit(directory){
  if(!fs.existsSync(directory))return;
  for(const item of fs.readdirSync(directory,{withFileTypes:true})){
   const file=path.join(directory,item.name);
   if(item.isDirectory())visit(file);
   else if(item.isFile()&&item.name.endsWith('.apk')){
    const manifest=require('../apk').withApk(file,({read})=>require('../binary-manifest').readBinaryManifest(read('AndroidManifest.xml',2*1024*1024)));
    const application=manifest.children.find(node=>node.name==='application');
    if(application?.children.some(node=>node.name==='provider')){
     const authorities=application.children.filter(node=>node.name==='provider').map(node=>node.attrs.get('http://schemas.android.com/apk/res/android:authorities'));
     sources.push({source:path.relative(image,file).split(path.sep).join('/'),packageName:manifest.attrs.get(':package'),sharedUserId:manifest.attrs.get('http://schemas.android.com/apk/res/android:sharedUserId')||null,
      authorities:authorities.filter(value=>typeof value==='string').flatMap(value=>value.split(';')),resolvedAuthorities:authorities.every(value=>typeof value==='string')});
    }
   }
  }
 }
 for(const partition of ['system','system_ext','product','vendor','odm','oem'])for(const kind of ['app','priv-app'])visit(path.join(image,partition,kind));
 const catalog={schemaVersion:1,packages:sources.sort((a,b)=>a.source.localeCompare(b.source))};
 fs.mkdirSync(path.dirname(output),{recursive:true});fs.writeFileSync(output,JSON.stringify(catalog,null,2)+'\n');
 return catalog;
}
