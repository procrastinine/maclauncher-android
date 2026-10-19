import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {cachedBuildStep} from '../native/upstream/build-step.mjs';

/** This developer tool delegates exact platform bodies in original DEX.
 * The original dexlib2 artifact owns metadata and code layout, including
 * Android hidden-API flags. No class loader or application audit is added. */
export async function buildDexBodies({output,jdk,dependencies}){
 const sources=['DexBodies.java','DexUiHooks.java','DexServiceCalls.java'].map(name=>path.join(import.meta.dirname,'tools',name));
 sources.push(...['UiHooks.java','PendingIntentCalls.java'].map(name=>path.resolve(import.meta.dirname,'../native/framework',name)));
 const java=path.join(jdk,'bin/java'),javac=path.join(jdk,'bin/javac');
 const libraries=['dexlib2','guava','annotations'].map(name=>{
  const file=dependencies[name];if(!file)throw Error('Missing DEX build dependency: '+name);return path.resolve(file);
 });
 const classes=path.join(output,'classes');
 await cachedBuildStep({cache:path.join(output,'cache.json'),inputs:[...sources,javac,java,...libraries],trees:[classes],key:{java:21},build:()=>{
  fs.mkdirSync(classes,{recursive:true});
  execFileSync(javac,['--release','21','-cp',libraries.join(path.delimiter),'-d',classes,...sources],{stdio:'inherit'});
 }});
 return {command:java,arguments:['-Xmx512m','-cp',[classes,...libraries].join(path.delimiter),'maclauncher.build.DexBodies']};
}

export function delegateDexBodies({input,bodies,targets,output,tool}){
 execFileSync(tool.command,[...tool.arguments,'--input',input,'--bodies',bodies,'--targets',targets,'--out',output],{stdio:'inherit'});
 return output;
}
