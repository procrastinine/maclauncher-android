import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {runtimeBootClasspath} from './boot-classpath.mjs';
const execute=promisify(execFile),root=import.meta.dirname;
const replace=(text,before,after)=>{if(text.split(before).length!==2)throw Error('Original ART image producer changed: '+before);return text.replace(before,after);};
const write=(file,bytes)=>fs.writeFileSync(file,bytes,{mode:0o600});

/** A completed directory is the cache publication unit, as with APK outputs. */
export function publishBootImage(built,destination,manifest){
 write(path.join(built,'boot-image.json'),JSON.stringify(manifest,null,2)+'\n');
 try{fs.renameSync(built,destination);}catch(error){
  if(!['EEXIST','ENOTEMPTY'].includes(error.code))throw error;
  const existing=JSON.parse(fs.readFileSync(path.join(destination,'boot-image.json'),'utf8'));
  if(existing.identity!==manifest.identity)throw error;
  return {root:destination,...existing};
 }
 return {root:destination,...manifest};
}

/** Original AIM dex2oat producer, supplied with the complete actual boot class
 * path. All jars share one primary multi-image, including the native adapters'
 * classes referenced by the preserved Android framework. No AOSP build runs. */
export async function buildBootImage({output,aim,components:c,bootClasspath,rustc='rustc',signal}){
 output=path.resolve(output);fs.mkdirSync(output,{recursive:true});
 const original=path.join(aim,'crates/aim-build/src/nodes/boot_image.rs'),originalLog=path.join(aim,'crates/aim-build/src/log.rs');
 let producer=fs.readFileSync(original,'utf8');
 producer=producer.slice(producer.indexOf('use crate::graph::'));
 const node=producer.indexOf('pub fn node()'),functions=producer.indexOf('/// A key of an oat',node);
 if(node<0||functions<node)throw Error('Original ART image producer entry is unavailable');
 producer=producer.slice(0,node)+producer.slice(functions);
 producer=replace(producer,'use crate::graph::{Action, Ctx, Dep, Node};','use crate::graph::Ctx;');
 producer=replace(producer,'use aim_android_image::assemble::force_remove;','use crate::aim_android_image::assemble::force_remove;\nuse crate::aim_paths;');
 producer=replace(producer,'let bcp = oat_key(&framework.join("arm64/boot.oat"), "bootclasspath")?;\n    let jars: Vec<&str> = bcp.split(\':\').collect();','let bcp = ctx.boot_classpath.join(":");\n    let jars: Vec<&str> = bcp.split(\':\').collect();');
 producer=replace(producer,'let stems = extension_stems(&image)?;','let stems: Vec<String> = Vec::new();');
 producer=replace(producer,'Dex2oat::new(ctx, &image, &work, &[])?','Dex2oat::new(ctx, &image, &work, &ctx.files)?');
 producer=replace(producer,'"rw\\t{guest}\\t{}\\n"','"ro\\t{guest}\\t{}\\n"');
 const log=fs.readFileSync(originalLog,'utf8'),driver=fs.readFileSync(path.join(root,'boot-image-driver.rs'),'utf8');
 const assembly=fs.readFileSync(path.join(aim,'crates/aim-android-image/src/assemble.rs'),'utf8'),remove=assembly.slice(assembly.indexOf('pub fn force_remove('));
 if(!remove.startsWith('pub fn force_remove('))throw Error('Original image cleanup owner is unavailable');
 const files=new Map([['/system/framework/framework.jar',c.frameworkJar],['/system/framework/services.jar',c.servicesJar],['/system/framework/maclauncher.jar',c.bootstrapDex]]);
 const classpath=runtimeBootClasspath(bootClasspath);
 const inputs=[...classpath.map(guest=>[guest,files.get(guest)||path.join(c.image,guest)]),...fs.readdirSync(path.join(c.art,'lib64')).filter(name=>name.endsWith('.so')).map(name=>['art/lib64/'+name,path.join(c.art,'lib64',name)]),['art/bin/dex2oat64',path.join(c.art,'bin/dex2oat64')],...['apex/com.android.art/etc/boot-image.prof','system/etc/boot-image.prof','apex/com.android.art/etc/dirty-image-objects','system/etc/dirty-image-objects','system/etc/preloaded-classes'].map(guest=>[guest,path.join(c.image,guest)])];
 const identities=await Promise.all(inputs.map(async([name,file])=>{
  const hash=crypto.createHash('sha256');
  for await(const chunk of fs.createReadStream(file,{signal}))hash.update(chunk);
  return [name,hash.digest('hex')];
 }));
 const hash=crypto.createHash('sha256').update('complete-primary-vdex-aliases-v1').update(producer).update(log).update(driver).update(remove).update(JSON.stringify(identities)).digest('hex');
 const destination=path.join(output,hash),receipt=path.join(destination,'boot-image.json');
 if(fs.existsSync(receipt))return {root:destination,...JSON.parse(fs.readFileSync(receipt,'utf8'))};
 const temporary=fs.mkdtempSync(path.join(output,'.boot-image-'));
 try{
  const tool=path.join(temporary,'producer'),source=path.join(temporary,'source');fs.mkdirSync(source);
  for(const[name,value]of [['producer.rs',producer],['log.rs',log],['main.rs',driver],['remove.rs',remove]])write(path.join(source,name),value);
  await execute(rustc,['--edition=2024','-O','--remap-path-prefix',temporary+'=/build/art-boot-image','--remap-path-prefix',path.resolve(aim)+'=/src/aim','--remap-path-prefix',root+'=/src/maclauncher/android/art',path.join(source,'main.rs'),'-o',tool],{signal,maxBuffer:1024*1024});
  const art=path.join(temporary,'art');fs.mkdirSync(art);fs.symlinkSync(path.resolve(c.art),path.join(art,'stripped'));
  const mappings=path.join(temporary,'inputs');write(mappings,[...files].map(([guest,host])=>guest+'\t'+path.resolve(host)).join('\n')+'\n');
  const built=path.join(temporary,'image'),logFile=path.join(temporary,'producer.log');
  await execute(tool,[path.resolve(c.linuxRun),mappings,logFile],{signal,timeout:120000,maxBuffer:1024*1024,env:{MACLAUNCHER_BOOT_IMAGE_SOURCE:path.resolve(c.image),MACLAUNCHER_BOOT_IMAGE_ART:art,MACLAUNCHER_BOOT_IMAGE_OUTPUT:built,MACLAUNCHER_BOOT_IMAGE_CLASSPATH:classpath.join(':')}});
  fs.copyFileSync(logFile,path.join(output,'last-build.log'));
  const framework=path.join(built,'framework');
  // AIM's original grouping reuses the stock image's ISA-directory VDEX
  // symlinks. The complete primary has one VDEX per actual class-path jar,
  // so publish their matching aliases rather than relying on the stock set.
  for(const file of fs.readdirSync(framework).filter(file=>file.endsWith('.vdex')))
   fs.symlinkSync('../'+file,path.join(framework,'arm64',file));
  const members=fs.readdirSync(framework).flatMap(name=>name==='arm64'?fs.readdirSync(path.join(framework,name)).map(file=>'arm64/'+file):[name]).sort();
  const manifest={identity:hash,location:'/system/framework/boot.art',files:members.map(file=>'framework/'+file),bootClasspath:classpath};
  return publishBootImage(built,destination,manifest);
 }catch(error){
  const logFile=path.join(temporary,'producer.log');if(fs.existsSync(logFile))fs.copyFileSync(logFile,path.join(output,'last-failure.log'));
  throw error;
 }finally{
  const writable=directory=>{for(const item of fs.readdirSync(directory,{withFileTypes:true})){if(item.isDirectory()){const file=path.join(directory,item.name);fs.chmodSync(file,0o700);writable(file);}}};
  writable(temporary);fs.rmSync(temporary,{recursive:true,force:true});
 }
}
