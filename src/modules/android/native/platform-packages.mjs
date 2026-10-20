import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
const require=createRequire(import.meta.url);
// Producer snapshots retain shared helpers in their original module layout.
// Direct source/ART calls use the same helpers beside this native directory.
const snapshotApk=new URL('../build-tools/src/modules/android/apk.js',import.meta.url);
const apkModule=fs.existsSync(snapshotApk)?snapshotApk:new URL('../apk.js',import.meta.url);

/** Original manifest metadata, shared by image and individual APK delivery. */
export function platformPackageRecord(file,source){
 const manifest=require(fileURLToPath(apkModule)).withApk(file,({read})=>require(fileURLToPath(new URL('./binary-manifest.js',apkModule))).readBinaryManifest(read('AndroidManifest.xml',2*1024*1024)));
 const application=manifest.children.find(node=>node.name==='application');
 if(!application?.children.some(node=>node.name==='provider'))return null;
 const authorities=application.children.filter(node=>node.name==='provider').map(node=>node.attrs.get('http://schemas.android.com/apk/res/android:authorities'));
 return {source,packageName:manifest.attrs.get(':package'),sharedUserId:manifest.attrs.get('http://schemas.android.com/apk/res/android:sharedUserId')||null,
  authorities:authorities.filter(value=>typeof value==='string').flatMap(value=>value.split(';')),resolvedAuthorities:authorities.every(value=>typeof value==='string')};
}

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
    const record=platformPackageRecord(file,path.relative(image,file).split(path.sep).join('/'));
    if(record)sources.push(record);
   }
  }
 }
 for(const partition of ['system','system_ext','product','vendor','odm','oem'])for(const kind of ['app','priv-app'])visit(path.join(image,partition,kind));
 const catalog={schemaVersion:1,packages:sources.sort((a,b)=>a.source.localeCompare(b.source))};
 fs.mkdirSync(path.dirname(output),{recursive:true});fs.writeFileSync(output,JSON.stringify(catalog,null,2)+'\n');
 return catalog;
}
