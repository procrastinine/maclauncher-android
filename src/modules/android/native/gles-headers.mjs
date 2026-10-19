import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';

const recorded=JSON.parse(fs.readFileSync(new URL('./gles-headers.json',import.meta.url)));
export function verifyGlesHeaders(directory, manifest=recorded) {
  const root=fs.realpathSync(directory);
  if(manifest.schemaVersion!==1||!/^[a-f0-9]{40}$/.test(manifest.revision)||!Array.isArray(manifest.files)||!manifest.files.length)
    throw Error('Invalid GLES header identity');
  const names=new Set();
  for(const entry of [...manifest.files,...(manifest.registry?[manifest.registry]:[])]){
    if(typeof entry.path!=='string'||!(/^include\/(GLES\d?|EGL|KHR)\/[a-z0-9_]+\.h$/.test(entry.path)||entry===manifest.registry&&entry.path==='scripts/gl.xml')||names.has(entry.path))throw Error('Invalid GLES header path');
    names.add(entry.path);
    const file=fs.realpathSync(path.join(root,entry.path));
    if(!file.startsWith(root+path.sep)||!fs.statSync(file).isFile())throw Error('GLES header leaves its source directory');
    const bytes=fs.readFileSync(file);
    if(bytes.length!==entry.size||crypto.createHash('sha256').update(bytes).digest('hex')!==entry.sha256)
      throw Error('GLES header does not match its recorded source: '+entry.path);
  }
  const git=(args)=>execFileSync('git',['-C',root,...args],{encoding:'utf8',timeout:10000}).trim();
  if(git(['rev-parse','HEAD'])!==manifest.revision)throw Error('GLES header source revision does not match');
  if(git(['status','--porcelain','--untracked-files=no','--',...names]))throw Error('GLES headers contain tracked changes');
  return manifest;
}
