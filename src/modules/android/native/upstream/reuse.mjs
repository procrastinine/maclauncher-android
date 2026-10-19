import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const sha=value=>crypto.createHash('sha256').update(value).digest('hex');
function stamp(file){
  const value=fs.statSync(file,{bigint:true});
  if(!value.isFile())throw Error('Original build cache input is not a regular file: '+file);
  return ['dev','ino','size','mtimeNs','ctimeNs','mode'].map(key=>String(value[key])).join(':');
}
/** Content identities with a filesystem change detector. ctime and inode are
 * included so restored mtimes, atomic replacement and chmod cannot hide edits.
 * Neither paths nor timestamps alone accept a changed producer or output. */
export class OriginalReuse {
  constructor(file){
    this.file=file;this.digests={};
    try{this.previous=JSON.parse(fs.readFileSync(file));if(this.previous.version!==1)this.previous=null;}
    catch(error){if(error.code!=='ENOENT'&&!(error instanceof SyntaxError))throw error;}
  }
  cachedIdentity(file,before){
    const known=this.digests[file]||this.previous?.digests?.[file];
    if(known?.stamp!==before)return null;
    this.digests[file]=known;return {file,sha256:known.sha256};
  }
  rememberIdentity(file,before,hash){
    if(stamp(file)!==before)return null;
    const value={stamp:before,sha256:hash.digest('hex')};
    this.digests[file]=value;return {file,sha256:value.sha256};
  }
  async identity(file){
    file=path.resolve(file);
    for(let attempt=0;attempt<2;attempt++){
      const before=stamp(file),known=this.cachedIdentity(file,before);
      if(known)return known;
      const hash=crypto.createHash('sha256');for await(const bytes of fs.createReadStream(file))hash.update(bytes);
      const result=this.rememberIdentity(file,before,hash);if(result)return result;
    }
    throw Error('Original build input changed while reading: '+file);
  }
  identitySync(file){
    file=path.resolve(file);
    for(let attempt=0;attempt<2;attempt++){
      const before=stamp(file),known=this.cachedIdentity(file,before);
      if(known)return known;
      const hash=crypto.createHash('sha256'),buffer=Buffer.allocUnsafe(1024*1024),fd=fs.openSync(file,'r');
      try{for(let count;(count=fs.readSync(fd,buffer,0,buffer.length,null))!==0;)hash.update(buffer.subarray(0,count));}
      finally{fs.closeSync(fd);}
      const result=this.rememberIdentity(file,before,hash);if(result)return result;
    }
    throw Error('Original build input changed while reading: '+file);
  }
  // A completed producer may explicitly rebase metadata already proven to
  // describe unchanged semantic inputs. All other files and every mode still
  // require their recorded identity; ordinary callers skip nothing.
  async lookup(key,{updatedInputs=[]}={}){
    const prior=this.previous;
    if(!prior||prior.key!==sha(JSON.stringify(key))||!Array.isArray(prior.inputs))return null;
    const updated=new Set(updatedInputs);
    try{for(const row of prior.inputs)if(!updated.has(row.file)&&(await this.identity(row.file)).sha256!==row.sha256||
      (fs.statSync(row.file).mode&0o7777)!==row.mode)return null;}
    catch(error){if(error.code==='ENOENT'||error.code==='ENOTDIR')return null;throw error;}
    return prior.value;
  }
  async save(key,files,value){
    const inputs=[];for(const file of [...new Set(files)].sort())inputs.push({...await this.identity(file),mode:fs.statSync(file).mode&0o7777});
    this.writeRecord({version:1,key:sha(JSON.stringify(key)),inputs,digests:this.digests,value});
  }
  /** Digest-only callers retain just the files touched by this cache instance. */
  flushSync(){this.writeRecord({version:1,digests:this.digests});}
  writeRecord(record){
    fs.mkdirSync(path.dirname(this.file),{recursive:true});
    const temporary=this.file+'.'+process.pid+'.tmp';
    try{fs.writeFileSync(temporary,JSON.stringify(record)+'\n');fs.renameSync(temporary,this.file);}
    finally{fs.rmSync(temporary,{force:true});}
    this.previous=record;
  }
}

export function originalTreeFiles(root){
  const files=[];
  for(const entry of fs.readdirSync(root,{withFileTypes:true}).sort((a,b)=>a.name.localeCompare(b.name))){
    const file=path.join(root,entry.name);
    if(entry.isDirectory())files.push(...originalTreeFiles(file));
    else if(entry.isFile())files.push(file);
    else throw Error('Original build-tool snapshot requires regular files: '+file);
  }
  return files;
}

