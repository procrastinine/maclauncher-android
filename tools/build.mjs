import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {spawnSync} from 'node:child_process';

const root=path.resolve(import.meta.dirname,'..'),args=process.argv.slice(2);
if(args.length!==2||args[0]!=='--inputs')throw Error('Usage: node tools/build.mjs --inputs build-inputs.json');
const options=JSON.parse(fs.readFileSync(path.resolve(args[1])));
if(!options||Array.isArray(options)||Object.values(options).some(value=>typeof value!=='string'||!path.isAbsolute(value)))
  throw Error('Build inputs must map option names to absolute paths.');
if(!options.output)throw Error('Missing output build input.');
const native=path.join(root,'src/modules/android/native');
const workspace=options['build-work']||path.join(path.dirname(options.output),'.android-build-cache');
const tools=options['tool-inputs']||path.join(workspace,'binary-tools');
const sources=path.join(root,'src');
if(tools===sources||tools.startsWith(sources+path.sep))throw Error('Build tool storage must be outside the source directory.');
fs.mkdirSync(tools,{recursive:true});
for(const name of ['asm-binary.json','apksig-binary.json']){
  const pin=JSON.parse(fs.readFileSync(path.join(native,name)));
  for(const file of pin.files){
    const output=path.join(tools,file.path);
    let bytes;
    try{bytes=fs.readFileSync(output);}catch(error){if(error.code!=='ENOENT')throw error;}
    if(bytes&&bytes.length===file.size&&crypto.createHash('sha256').update(bytes).digest('hex')===file.sha256)continue;
    const response=await fetch(file.url||pin.url);
    if(!response.ok)throw Error('Build tool download failed: '+response.status);
    bytes=Buffer.from(await response.arrayBuffer());
    if(bytes.length!==file.size||crypto.createHash('sha256').update(bytes).digest('hex')!==file.sha256)
      throw Error('Build tool checksum differs from its source pin.');
    const temporary=path.join(tools,'.tool-'+crypto.randomUUID());
    try{fs.writeFileSync(temporary,bytes,{flag:'wx',mode:0o644});fs.renameSync(temporary,output);}
    finally{fs.rmSync(temporary,{force:true});}
  }
}
options['tool-inputs']=tools;
const result=spawnSync(process.execPath,[path.join(native,'build.mjs'),
  ...Object.entries(options).flatMap(([key,value])=>['--'+key,value])],{stdio:'inherit'});
if(result.error)throw result.error;
process.exitCode=result.status??1;
