import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {cachedBuildStep} from '../native/upstream/build-step.mjs';

/** Android's SDK core-for-system-modules is compile metadata only. The module
 * descriptor/jmod/jlink sequence is the Android Gradle plugin's JDK image
 * transform; no host JDK implementation enters the ART boot classpath. */
export async function prepareCoreApi({output,jdk,coreSystemModules}){
 const tool=name=>path.join(jdk,'bin',name),jrt=path.join(jdk,'lib/jrt-fs.jar');
 const image=path.join(output,'image');
 await cachedBuildStep({cache:output+'.json',inputs:[import.meta.filename,coreSystemModules,jrt,...['javac','jar','jmod','jlink'].map(tool)],trees:[output],key:{coreSystemModules},build:()=>{
  const entries=execFileSync(tool('jar'),['--list','--file',coreSystemModules],{encoding:'utf8'}).split('\n');
  const packages=[...new Set(entries.filter(name=>name.endsWith('.class')&&name.includes('/')).map(name=>name.slice(0,name.lastIndexOf('/')).replaceAll('/','.')))].sort();
  const descriptor=path.join(output,'module-info.java');
  fs.writeFileSync(descriptor,'module java.base {\n'+packages.map(name=>' exports '+name+';').join('\n')+'\n}\n');
  execFileSync(tool('javac'),['--system','none','--patch-module','java.base='+coreSystemModules,'-d',output,descriptor],{stdio:'inherit'});
  const archive=path.join(output,'module.jar');fs.copyFileSync(coreSystemModules,archive);
  execFileSync(tool('jar'),['--update','--file',archive,'-C',output,'module-info.class'],{stdio:'inherit'});
  const modules=path.join(output,'jmod');fs.mkdirSync(modules);
  const version=execFileSync(tool('jlink'),['--version'],{encoding:'utf8'}).trim();
  // Modern jlink validates the OS/CPU tuple; Android's ART target uses the
  // Linux ARM64 ABI. This image contains compiler declarations, not a JVM.
  execFileSync(tool('jmod'),['create','--module-version',version,'--target-platform','linux-aarch64','--class-path',archive,path.join(modules,'java.base.jmod')],{stdio:'inherit'});
  execFileSync(tool('jlink'),['--module-path',modules,'--add-modules','java.base','--output',image,'--disable-plugin','system-modules'],{stdio:'inherit'});
  fs.copyFileSync(jrt,path.join(image,'lib/jrt-fs.jar'));
 }});
 return image;
}
