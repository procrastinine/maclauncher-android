import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {cachedBuildStep} from './upstream/build-step.mjs';
import {copyChanged,synchronizeSourceTree,writeChanged} from './upstream/source-files.mjs';
import {originalTreeFiles} from './upstream/reuse.mjs';

export const javaSources=directory=>fs.readdirSync(directory,{withFileTypes:true}).sort((a,b)=>a.name.localeCompare(b.name))
  .flatMap(entry=>entry.isDirectory()?javaSources(path.join(directory,entry.name)):entry.isFile()&&entry.name.endsWith('.java')?[path.join(directory,entry.name)]:[]);

export function javaToolInputs(jdk){
  return ['release','lib/modules','bin/java','bin/javac','bin/jar'].map(file=>path.join(jdk,file));
}
export const javaToolEnvironment=()=>Object.fromEntries(
  ['JDK_JAVAC_OPTIONS','JAVA_TOOL_OPTIONS','JDK_JAVA_OPTIONS','_JAVA_OPTIONS'].map(name=>[name,process.env[name]??null]));

/** Native Minikin maps the original pattern data by filesystem path. Copy the
 * declared framework JAR's resources and any accompanying licenses unchanged. */
export function stageFrameworkHyphenation(archive,output){
  const prefix='hyphen-data/',names=execFileSync('/usr/bin/unzip',['-Z1',archive,prefix+'*'],{encoding:'utf8',maxBuffer:4<<20})
    .trim().split(/\r?\n/).filter(name=>name&&!name.endsWith('/'));
  if(!names.some(name=>name.endsWith('.hyb')))throw Error('The supplied Android framework has no hyphenation data');
  if(new Set(names).size!==names.length)throw Error('Duplicate Android hyphenation resource');
  const destination=path.join(output,'android-root/system/usr/hyphen-data');
  for(const name of names){
    const relative=name.slice(prefix.length);
    if(!name.startsWith(prefix)||relative.includes('\\')||relative.split('/').some(part=>!part||part==='.'||part==='..'))
      throw Error('Invalid Android hyphenation resource path: '+name);
    const bytes=execFileSync('/usr/bin/unzip',['-p',archive,name],{maxBuffer:16<<20});
    if(name.endsWith('.hyb')&&(bytes.length<24||bytes.readUInt32LE(0)!==0x62ad7968||bytes.readUInt32LE(20)!==bytes.length))
      throw Error('Invalid supplied Android hyphenation data: '+name);
    writeChanged(path.join(destination,relative),bytes,0o644);
  }
  return names.map(name=>path.join(destination,name.slice(prefix.length)));
}

export async function buildFramework({root,build,output,jdk,framework,frameworkArchives,asmClasspath,graphicsRegistrations}){
  const frameworkTool=path.join(build,'framework-tool'),hostClasses=path.join(build,'classes'),jni=path.join(build,'jni');
  const archive=path.join(build,'framework-payload/android-framework.jar'),callbacks=path.join(build,'host-callbacks.h');
  const declarations=path.join(build,'framework-declarations'),generatedJni=path.join(declarations,'jni'),generatedCallbacks=path.join(declarations,'host-callbacks.h');
  const bindings=path.join(declarations,'native-bindings.tsv');
  const java=path.join(jdk,'bin/java'),javac=path.join(jdk,'bin/javac');
  const portability=javaSources(path.join(root,'portability')),tools=javaSources(path.join(root,'framework'));
  const run=(command,args)=>execFileSync(command,args,{stdio:'inherit'});
  const buildArchive=()=>cachedBuildStep({cache:path.join(build,'framework-archive.reuse.json'),
    inputs:[fileURLToPath(import.meta.url),...javaToolInputs(jdk),...originalTreeFiles(frameworkTool),bindings,framework.input.file,...frameworkArchives,...asmClasspath.split(path.delimiter)],
    files:[archive],key:{frameworkInput:framework.input.file,frameworkArchives,asmClasspath,environment:javaToolEnvironment()},build(){
      run(java,['-cp',[frameworkTool,asmClasspath,framework.input.file,hostClasses,...frameworkArchives].join(path.delimiter),'FrameworkArchive',framework.input.file,archive,...frameworkArchives]);
    }});
  await cachedBuildStep({cache:path.join(build,'framework.reuse.json'),
    inputs:[fileURLToPath(import.meta.url),...javaToolInputs(jdk),...portability,...tools,...framework.sources,
      framework.input.file,...frameworkArchives,...asmClasspath.split(path.delimiter),path.join(root,'host-callbacks.tsv'),
      ...(graphicsRegistrations?[graphicsRegistrations]:[])],
    trees:[frameworkTool,hostClasses,declarations],
    key:{release:21,asmClasspath,frameworkArchives,frameworkInput:framework.input.file,graphicsRegistrations,environment:javaToolEnvironment()},async build(){
      run(javac,['--release','21','-cp',asmClasspath+path.delimiter+framework.input.file,'-d',frameworkTool,...tools,...portability]);
      if(graphicsRegistrations)fs.copyFileSync(graphicsRegistrations,path.join(frameworkTool,'graphics-registrations.txt'));
      run(javac,['--release','21','-cp',[framework.input.file,frameworkTool,...frameworkArchives].join(path.delimiter),'-d',hostClasses,'-h',generatedJni,...framework.sources]);
      run(java,['-cp',[frameworkTool,asmClasspath,framework.input.file,hostClasses,...frameworkArchives].join(path.delimiter),'NativeBindings',bindings]);
      await buildArchive();
      run(java,['-cp',[frameworkTool,asmClasspath].join(path.delimiter),'ServiceEndpoints',archive,hostClasses]);
      run(java,['-cp',frameworkTool+path.delimiter+asmClasspath,'HostCallbacks',path.join(root,'host-callbacks.tsv'),generatedCallbacks,hostClasses,archive]);
    }});
  // The existing archive producer independently checks its complete inputs and
  // output. Host body changes affect this transform only through binding ABI.
  await buildArchive();
  // javac rebuilds clean declarations, while native compilers keep unchanged
  // header identities. Synchronization also retires removed native classes.
  synchronizeSourceTree(generatedJni,jni);
  copyChanged(generatedCallbacks,callbacks);
  copyChanged(path.join(declarations,'host-callbacks.c'),path.join(build,'host-callbacks.c'));
  fs.copyFileSync(archive,path.join(output,'android-framework.jar'),fs.constants.COPYFILE_FICLONE);
  stageFrameworkHyphenation(archive,output);
  return {frameworkTool,hostClasses};
}
