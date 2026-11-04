import {optionalFiles} from './upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {verifyBinaryInputs} from './binary-inputs.mjs';
import {platformPackageRecord} from './platform-packages.mjs';

/** Reuse the recorded original APK alongside the complete framework input.
 * Source/image identities describe provenance, not a byte reproduction claim. */
export function platformPackageInputs(root,directory){
 const definition=JSON.parse(fs.readFileSync(path.join(root,'platform-packages-binary.json')));
 if(definition.schemaVersion!==1||!Array.isArray(definition.files)||!definition.files.length)throw Error('Invalid platform package binary catalog');
 const files=verifyBinaryInputs(directory,definition.files);
 const destinations=new Set(),packages=new Set();
 const records=files.map((file,index)=>{
  const entry=definition.files[index],source=entry.destination;
  if(typeof source!=='string'||!source.startsWith('packages/')||!source.endsWith('.apk')||source.includes('\\')||source.split('/').some(part=>!part||part==='.'||part==='..')||destinations.has(source))throw Error('Invalid platform package destination');
  const record=platformPackageRecord(file.file,source);
  if(!record||record.packageName!==entry.packageName||packages.has(record.packageName))throw Error('Platform package manifest does not match its binary catalog: '+file.name);
  destinations.add(source);packages.add(record.packageName);
  return {...record,sha256:file.sha256,size:file.size};
 });
 return {
  definition,
  files,
  catalog:{schemaVersion:1,packages:records}
 };
}

/** Output is a fresh producer runtime, never a writable application profile. */
export function stagePlatformPackages({input,output}){
 const platform=path.join(output,'platform');
 for(let n=0;n<input.files.length;n++){
  const target=path.join(platform,input.catalog.packages[n].source);
  fs.mkdirSync(path.dirname(target),{recursive:true});
  fs.copyFileSync(input.files[n].file,target,fs.constants.COPYFILE_FICLONE);
 }
 const catalog=path.join(platform,'packages.json');
 fs.writeFileSync(catalog,JSON.stringify(input.catalog,null,2)+'\n');
 return {
  catalog,
  platform,
  packages:input.catalog.packages
 };
}
