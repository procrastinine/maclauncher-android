import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {aidlCrate,manifest,properties,write} from './blueprint.mjs';
import {prepareSourceTree} from '../../upstream/source-files.mjs';

export function prepareAidl({modules,keyMintAidl,compiler,python,sources,workspace,asyncTraitVersion}) {
  const records=new Map(keyMintAidl.records.map(record=>[record.name,{...record,
    generated:path.join(keyMintAidl.output,record.name+'-V'+record.version,'rust')} ]));
  for(const module of modules.values())if(module.type==='aidl_interface'&&!records.has(module.properties.name)) {
    const p=module.properties,version=(p.versions_with_info||p.versions?.map(version=>({version})))?.at(-1)?.version;
    const snapshot=version?path.join(module.directory,'aidl_api',p.name,version):module.directory;
    const includeRoot=p.local_include_dir?path.join(module.directory,p.local_include_dir):snapshot;
    records.set(p.name,{name:p.name,version,snapshot,includeRoot,properties:p,
      generated:path.join(workspace,'generated-aidl',p.name),
      hash:version?fs.readFileSync(path.join(snapshot,'.hash'),'utf8').trim().split(/\s+/).at(-1):null,
      inputs:fs.globSync(p.srcs,{cwd:snapshot}).sort().map(file=>({file:path.join(snapshot,file)}))});
  }
  const includes=[...new Set([...records.values()].map(r=>r.includeRoot||r.snapshot))].flatMap(root=>['-I',root]);
  const generated=new Map();
  function generate(name) {
    name=name.replace(/(?:-V\d+)?-rust$/,'').replace(/-V\d+$/,'');
    if(generated.has(name))return generated.get(name);
    const record=records.get(name);
    if(!record)throw Error('Missing upstream AIDL module: '+name);
    const crate=aidlCrate(name),directory=path.join(workspace,'aidl',crate);
    const result={name:crate,directory};generated.set(name,result);
    record.crate=crate;record.crateDirectory=directory;
    const p=properties(modules,name);
    const imports=[...new Set((p.imports||[]).map(n=>n.replace(/-V\d+$/,'')))].map(generate);
    if(!keyMintAidl.records.some(r=>r.name===name)) {
      prepareSourceTree(record.generated,temporary=>{
      const args=['--lang=rust','--structured','--min_sdk_version=31','--omit_invocation',...includes,'-o',temporary];
      if(p.stability)args.push('--stability='+p.stability);
      if(record.version)args.push('--version='+record.version,'--hash='+record.hash);
      args.push(...record.inputs.map(i=>i.file));
      execFileSync(compiler,args,{stdio:'inherit'});
      });
    }
    const inputs=fs.globSync('**/*.rs',{cwd:record.generated}).sort().map(f=>path.join(record.generated,f));
    prepareSourceTree(directory,temporary=>{
    execFileSync(python,[path.join(sources.aidl,'build/aidl_rust_glue.py'),path.join(temporary,'lib.rs'),record.generated,...inputs,...imports.flatMap(i=>['-I',i.name])],{stdio:'inherit'});
    write(path.join(temporary,'Cargo.toml'),manifest({name:crate,dependencies:[
      'binder={path="../../binder"}',`async-trait="=${asyncTraitVersion}"`,
      ...imports.map(i=>`${i.name}={path=${JSON.stringify(path.relative(directory,i.directory))}}`)]}));
    });
    return result;
  }
  return {generate,generated,records};
}
