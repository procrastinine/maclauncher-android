import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {OriginalReuse,originalTreeFiles} from './reuse.mjs';

const implementation=[fileURLToPath(import.meta.url),fileURLToPath(new URL('./reuse.mjs',import.meta.url))];
const members=trees=>trees.map(tree=>[tree,fs.existsSync(tree)?originalTreeFiles(tree):null]);
const stamp=file=>{
  const value=fs.statSync(file,{bigint:true});
  return ['dev','ino','size','mtimeNs','ctimeNs','mode'].map(key=>String(value[key])).join(':');
};

/** One producer owns each output tree. Rebuilds start empty, so removed Java
 * classes/resources cannot survive an incremental build. Reuse includes every
 * input and output identity and the complete directory membership. */
export async function cachedBuildStep({cache,inputs,trees=[],files=[],key,build}){
  inputs=[...new Set([...implementation,...inputs])].sort();
  const reuse=new OriginalReuse(cache);
  const identity=()=>({key,inputs,files,trees:members(trees)});
  if(await reuse.lookup(identity()))return false;
  for(let attempt=0;attempt<2;attempt++){
    const before=[];
    for(const file of inputs)before.push({...await reuse.identity(file),stamp:stamp(file)});
    for(const file of [...trees,...files])fs.rmSync(file,{recursive:true,force:true});
    for(const directory of trees)fs.mkdirSync(directory,{recursive:true});
    for(const file of files)fs.mkdirSync(path.dirname(file),{recursive:true});
    await build();
    const changed=async()=>{
      for(const input of before)if(stamp(input.file)!==input.stamp||
        (await reuse.identity(input.file)).sha256!==input.sha256)return input.file;
      return null;
    };
    let change=await changed();
    if(!change){
      const outputs=members(trees).flatMap(([,entries])=>entries||[]);
      await reuse.save(identity(),[...inputs,...files,...outputs],true);
      // Saving identities also reads outputs. An input edit during that pass
      // must not leave a reusable association with the earlier producer run.
      try{change=await changed();}
      catch(error){fs.rmSync(cache,{force:true});throw error;}
      if(!change)return true;
    }
    fs.rmSync(cache,{force:true});
    if(attempt)throw Error('Build input kept changing during production: '+change);
  }
}
