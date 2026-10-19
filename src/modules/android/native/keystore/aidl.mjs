import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {parseBlueprint} from '../upstream/blueprint.mjs';
import {prepareSourceTree} from '../upstream/source-files.mjs';

/** Complete enabled native KeyMint AIDL backends from their original declarations. */
export function generateKeyMintAidl({source,compiler,output}) {
const run=(command,args)=>execFileSync(command,args,{encoding:'utf8',stdio:'pipe'});
const sha=value=>createHash('sha256').update(value).digest('hex');
const walk=dir=>fs.readdirSync(dir,{withFileTypes:true}).flatMap(e=>e.isDirectory()?walk(path.join(dir,e.name)):[path.join(dir,e.name)]);
const records=[];
for(const component of ['secureclock','keymint','rkp','sharedsecret']){
  const moduleRoot=path.join(source,'security',component,'aidl');
  const definition=path.join(moduleRoot,'Android.bp');
  const declaration=parseBlueprint(fs.readFileSync(definition,'utf8'),{file:definition}).modules.find(m=>m.type==='aidl_interface');
  if(!declaration)throw Error('Missing original AIDL interface declaration');
  const p=declaration.properties;
  const versions=p.versions_with_info||p.versions.map(version=>({version}));
  const latest=versions.at(-1);
  const snapshot=path.join(moduleRoot,'aidl_api',p.name,latest.version);
  const inputs=fs.globSync(p.srcs,{cwd:snapshot}).sort().map(f=>path.join(snapshot,f));
  if(!inputs.length)throw Error('Original AIDL declaration has no source files: '+p.name);
  records.push({component,name:p.name,version:latest.version,hash:fs.readFileSync(path.join(snapshot,'.hash'),'utf8').trim().split(/\s+/).at(-1),snapshot,definition,definitionSha256:sha(fs.readFileSync(definition)),properties:p,inputs:inputs.map(file=>({file,sha256:sha(fs.readFileSync(file))})),backends:{}});
}
const includes=records.flatMap(r=>['-I',r.snapshot]);
prepareSourceTree(output,temporary=>{
for(const r of records){
  const common=['--structured','--stability='+r.properties.stability,...includes];
  for(const backend of ['ndk','rust']){
    if(r.properties.backend?.[backend]?.enabled===false)continue;
    const destination=path.join(temporary,r.name+'-V'+r.version,backend);
    const minSdk=r.properties.backend?.[backend]?.min_sdk_version||r.properties.min_sdk_version||({java:'1',cpp:'23',ndk:'29',rust:'31'}[backend]);
    const args=['-arm64',compiler,'--lang='+backend,...common,'--version='+r.version,'--hash='+r.hash,'--min_sdk_version='+minSdk,'--omit_invocation','-a','-o',destination];
    if(backend==='cpp'||backend==='ndk')args.push('-h',path.join(destination,'include'));
    args.push(...r.inputs.map(i=>i.file));
    fs.mkdirSync(destination,{recursive:true});
    run('/usr/bin/arch',args);
    r.backends[backend]={minSdk};
  }
}
});
for(const r of records)for(const [backend,record]of Object.entries(r.backends)){
  const destination=path.join(output,r.name+'-V'+r.version,backend);
  record.files=walk(destination).sort().map(file=>({file,bytes:fs.statSync(file).size,sha256:sha(fs.readFileSync(file))}));
}

return {compiler,records,output};
}
