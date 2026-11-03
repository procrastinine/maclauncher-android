import {addLicense} from './upstream/licenses.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {parseBlueprint} from './upstream/blueprint.mjs';

const relative=value=>typeof value==='string'&&value.length&&!path.posix.isAbsolute(value)&&!value.split('/').includes('..');

/** Install every original framework data/etc prebuilt using its Soong destination. */
export function stageAndroidSystemConfig({source,output}){
  const root=path.join(output,'android-root'),licenses=path.join(output,'licenses'),products=[],targets=new Map();
  fs.rmSync(output,{recursive:true,force:true});fs.mkdirSync(root,{recursive:true});
  for(const definition of fs.globSync('data/etc/**/Android.bp',{cwd:source}).sort()){
    const declarations=parseBlueprint(fs.readFileSync(path.join(source,definition),'utf8'),{file:definition});
    for(const module of declarations.modules){
      if(module.type!=='prebuilt_etc')continue;
      const p=module.properties;
      if(!relative(p.src)||!relative(p.sub_dir)||typeof p.name!=='string')throw Error('Unresolved Android system configuration module: '+p.name);
      const partition=p.product_specific?'product':p.system_ext_specific?'system_ext':p.vendor||p.soc_specific?'vendor':p.device_specific?'odm':p.proprietary?'vendor':'system';
      const name=p.filename||(p.filename_from_src?path.posix.basename(p.src):p.name);
      if(!relative(name)||name.includes('/'))throw Error('Invalid Android system configuration filename: '+name);
      const original=path.posix.join(path.posix.dirname(definition),p.src),destination=path.posix.join(partition,'etc',p.sub_dir,name);
      if(targets.has(destination))throw Error('Duplicate Android system configuration destination: '+destination);
      const bytes=fs.readFileSync(path.join(source,original));
      const file=path.join(root,destination);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,bytes);targets.set(destination,p.name);
      products.push({module:p.name,definition,source:original,path:destination,sha256:createHash('sha256').update(bytes).digest('hex')});
    }
  }
  addLicense(path.dirname(licenses),'system-config',path.join(source,'NOTICE'));
  return {root,licenses,products};
}
