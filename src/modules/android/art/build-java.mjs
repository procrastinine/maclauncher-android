import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {cachedBuildStep} from '../native/upstream/build-step.mjs';
import {originalTreeFiles} from '../native/upstream/reuse.mjs';
import {prepareCoreApi} from './core-api.mjs';
import {delegateDexBodies} from './dex-bodies.mjs';

/** D8 converts bytecode only. Keep every declared owned Java resource beside
 * the DEX so original ClassLoader resource lookup has the same input tree. */
export function packageArtDex({classes,dex,jar,target}){
 fs.cpSync(classes,dex,{recursive:true,filter:file=>!file.endsWith('.class')});
 execFileSync(jar,['--create','--no-compress','--file',target,'-C',dex,'.'],{stdio:'inherit'});
}

/** Common adapters use the same Android core declarations as ART entry code. */
export function compileArtSources({jdk,core,api,sharedClasses,classes,sources}){
 // Android's SDK module contains public libcore declarations. The original
 // capability structs and methods are hidden framework APIs; retain their
 // complete original class declarations for compilation, outside program output.
 const declarations=classes+'.platform-declarations';fs.mkdirSync(declarations,{recursive:true});
 execFileSync(path.join(jdk,'bin/jar'),['--extract','--file',path.resolve(api),'android/system/Os.class','android/system/OsConstants.class','android/system/StructCapUserHeader.class','android/system/StructCapUserData.class'],{cwd:declarations,stdio:'inherit'});
 execFileSync(path.join(jdk,'bin/javac'),['--system',core,'--patch-module','java.base='+declarations,'-source','21','-target','21','-cp',[api,sharedClasses].join(path.delimiter),'-d',classes,...sources],{stdio:'inherit'});
}

/** D8 handles only the launcher-owned Java hooks. ART consumes the original
 * matching libcore/framework DEX from its image without HotSpot transforms.
 * sharedClasses is the complete host-archive tree (or extracted android-host.jar),
 * including generated publisher resources, rather than bare javac output. */
export async function buildArtJava({output,jdk,api,coreSystemModules,sharedClasses,r8,imageServicesJar,imageFrameworkJar=path.join(path.dirname(imageServicesJar),'framework.jar'),dexBodies}){
 const root=import.meta.dirname,java=path.join(jdk,'bin/java'),javac=path.join(jdk,'bin/javac'),jar=path.join(jdk,'bin/jar');
 const core=await prepareCoreApi({output:path.join(output,'core-api'),jdk,coreSystemModules});
 const sources=originalTreeFiles(path.join(root,'java')).filter(file=>file.endsWith('.java'));
 const sharedRoot=path.resolve(root,'../native');
 // Recompile the common source group against Android's core declarations.
 // JVM-specific implementations remain in their separate java-jvm source
 // group and supply classpath declarations, rather than host-core APIs.
 const sharedSources=originalTreeFiles(path.join(sharedRoot,'java')).filter(file=>file.endsWith('.java'));
 const keyboard=path.join(sharedRoot,'resources/Virtual.kcm');
 const storageNamespace=path.join(sharedRoot,'resources/storage-namespace.json');
 const destination=path.join(output,'bootstrap');
 const serviceBodies=path.join(root,'adapters/service-bodies'),servicesJar=path.join(destination,'services.jar');
 const bodies=path.join(root,'adapters/framework-bodies'),frameworkJar=path.join(destination,'framework.jar');
 if(!dexBodies?.command||!Array.isArray(dexBodies.arguments))throw Error('The Android DEX platform-body build tool is required');
 const bodyInputs=dexBodies.arguments.flatMap(value=>value.split(path.delimiter)).filter(file=>fs.existsSync(file)).flatMap(file=>fs.statSync(file).isDirectory()?originalTreeFiles(file):[file]);
 await cachedBuildStep({cache:path.join(output,'bootstrap-cache.json'),inputs:[path.join(root,'build-java.mjs'),path.join(root,'dex-bodies.mjs'),...sources,...sharedSources,keyboard,storageNamespace,serviceBodies,bodies,api,coreSystemModules,...originalTreeFiles(core),r8,imageServicesJar,imageFrameworkJar,dexBodies.command,...bodyInputs,...originalTreeFiles(sharedClasses),java,javac,jar],trees:[destination],key:{api:36,dexBodies:dexBodies.arguments},build:()=>{
  const classes=path.join(destination,'classes'),dex=path.join(destination,'dex'),archive=path.join(destination,'hooks.jar');
  fs.mkdirSync(classes,{recursive:true});fs.mkdirSync(dex,{recursive:true});
  // Full shared owned hooks are retained; only VM entry and representations
  // differ. No framework methods or classes are selected out of the image.
  fs.cpSync(sharedClasses,classes,{recursive:true});
  fs.copyFileSync(keyboard,path.join(classes,'maclauncher/android/framework/Virtual.kcm'));
  fs.copyFileSync(storageNamespace,path.join(classes,'maclauncher/android/framework/storage-namespace.json'));
  compileArtSources({jdk,core,api,sharedClasses,classes,sources:[...sharedSources,...sources]});
  execFileSync(jar,['--create','--file',archive,'-C',classes,'.'],{stdio:'inherit'});
  execFileSync(java,['-cp',r8,'com.android.tools.r8.D8','--min-api','36','--lib',coreSystemModules,'--lib',api,'--output',dex,archive],{stdio:'inherit'});
  const targets=path.join(destination,'hooks-dex.jar');
  packageArtDex({classes,dex,jar,target:targets});
  delegateDexBodies({input:imageServicesJar,bodies:serviceBodies,targets,output:servicesJar,tool:dexBodies});
  delegateDexBodies({input:imageFrameworkJar,bodies,targets,output:frameworkJar,tool:dexBodies});
 }});
 return {bootstrapDex:path.join(destination,'hooks-dex.jar'),servicesJar,frameworkJar,frameworkResourcePreinitVersion:1};
}
