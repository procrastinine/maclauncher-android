import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {snapshotSources,snapshotBuildCompanions,sourceIdentity,sourceInventory} from './build-sources.mjs';
import {completeBuildOptions} from './build-options.mjs';
import {synchronizeSourceTree} from './upstream/source-files.mjs';
import {enterBuildWorkspace} from './build-workspace.mjs';

const root=import.meta.dirname,options={};let workspaceFd,outputFd;
for(let i=2;i<process.argv.length;i+=2){
  if(process.argv[i]==='--workspace-fd'){workspaceFd=Number(process.argv[i+1]);if(!Number.isInteger(workspaceFd)||workspaceFd<0)throw Error('Invalid compiler workspace descriptor');continue;}
  if(process.argv[i]==='--output-fd'){outputFd=Number(process.argv[i+1]);if(!Number.isInteger(outputFd)||outputFd<0)throw Error('Invalid build output descriptor');continue;}
  if(!process.argv[i].startsWith('--')||!process.argv[i+1]||options[process.argv[i].slice(2)])throw Error('Expected --option path pairs');
  options[process.argv[i].slice(2)]=path.resolve(process.argv[i+1]);
}
completeBuildOptions(root,options);
if(options.output===root||options.output.startsWith(root+path.sep))throw Error('Android build output must be outside its source directory');
options['build-work']??=path.join(path.dirname(options.output),'.android-build-cache');
if(!enterBuildWorkspace({directory:options['build-work'],output:options.output,python:options.python,entry:process.argv[1],args:process.argv.slice(2),fd:workspaceFd,outputFd,manageOutput:true}))process.exit(0);
const source=path.join(options.output,'source');
snapshotSources(root,source);
const toolSnapshot=snapshotBuildCompanions({repository:path.resolve(root,'../../../..'),destination:path.join(source,'build-tools')});
const workingSource=path.join(options['build-work'],'source');synchronizeSourceTree(source,workingSource);
const identity=sourceIdentity(sourceInventory(path.join(source,'native')),fs.readFileSync(path.join(source,'profile.js')),
  sourceInventory(toolSnapshot.root),sourceInventory(path.join(source,'art')));
fs.writeFileSync(path.join(options.output,'build-source.json'),JSON.stringify(identity,null,2)+'\n');
synchronizeSourceTree(source,workingSource);
// Start a fresh module graph: every generator, Java adapter and native source
// now comes from the same fixed snapshot, even if development continues.
execFileSync(process.execPath,[path.join(workingSource,'native/compile.mjs'),...Object.entries(options).flatMap(([name,value])=>['--'+name,value])],{stdio:'inherit'});
