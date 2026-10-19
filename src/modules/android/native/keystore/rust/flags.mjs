import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {manifest,properties,write} from './blueprint.mjs';
import {prepareSourceTree} from '../../upstream/source-files.mjs';

export function prepareFlags({modules,sources,workspace,aconfig,release='bp2a',logVersion}) {
  const order=[];
  function inherit(name) {
    if(order.includes(name))return;
    const text=fs.readFileSync(path.join(sources.release,'release_configs',name+'.textproto'),'utf8');
    for(const match of text.matchAll(/inherits:\s*"([^"]+)"/g))inherit(match[1]);
    order.push(name);
  }
  inherit(release);
  // Keep the original storage reader crates even though this release's original
  // framework flags are constant. The generated API remains complete.
  const directories=[];
  for(const name of ['aconfig_storage_file','aconfig_storage_read_api']) {
    const source=path.join(sources.build,'tools/aconfig',name),destination=path.join(workspace,'platform',name);
    prepareSourceTree(destination,temporary=>fs.cpSync(source,temporary,{recursive:true}));directories.push(destination);
  }
  function generate(module) {
    const p=properties(modules,module.properties.name),declaration=modules.get(p.aconfig_declarations);
    if(!declaration)throw Error('Missing upstream flag declaration: '+p.aconfig_declarations);
    const d=declaration.properties,name=p.crate_name,directory=path.join(workspace,'platform',name);
    const declarations=fs.globSync(d.srcs,{cwd:declaration.directory}).sort().map(f=>path.join(declaration.directory,f));
    const values=order.flatMap(release=>{
      const base=path.join(sources.release,'aconfig',release,d.package);
      return fs.existsSync(base)?fs.globSync('*.textproto',{cwd:base}).sort().map(f=>path.join(base,f)):[];
    });
    prepareSourceTree(directory,temporary=>{
    const cache=path.join(temporary,'flags.pb');
    execFileSync(aconfig,['create-cache','--package',d.package,'--container',d.container,'--default-permission','read_only',...declarations.flatMap(f=>['--declarations',f]),...values.flatMap(f=>['--values',f]),'--cache',cache],{stdio:'inherit'});
    execFileSync(aconfig,['create-rust-lib','--cache',cache,'--mode','production','--out',temporary],{stdio:'inherit'});
    write(path.join(temporary,'Cargo.toml'),manifest({name,entry:'src/lib.rs',dependencies:[
      'aconfig_storage_read_api={path="../aconfig_storage_read_api"}',`log="=${logVersion}"`]}));
    });
    return {name,directory};
  }
  return {generate,release,order,directories};
}
