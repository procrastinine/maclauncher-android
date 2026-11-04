import {addLicenses} from '../upstream/licenses.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {parseBlueprint} from '../upstream/blueprint.mjs';

const suppliers=JSON.parse(fs.readFileSync(new URL('./sources.json',import.meta.url)));
const defaultPins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));
const repository=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../../../../..');
const digest=bytes=>createHash('sha256').update(bytes).digest('hex');
const git=(source,args)=>execFileSync('git',['-C',source.checkout,...args],{maxBuffer:256<<20});
const safe=value=>typeof value==='string'&&value.length&&!path.posix.isAbsolute(value)&&!value.split('/').includes('..');
const list=(value,label)=>{
  if(!Array.isArray(value)||!value.every(item=>typeof item==='string'))throw Error(`Unresolved font module property: ${label}`);
  return value;
};

function read(source,file){
  if(!safe(file)||!source.files.has(file))throw Error(`Missing original font resource: ${source.key}:${file}`);
  return git(source,['show',`${source.revision}:${file}`]);
}

/** Stage the original prebuilt font modules; no font family or fallback is synthesized. */
export function stageAndroidFonts({sources={},output,sourcePins=defaultPins}={}){
  if(!output)throw Error('An Android font output directory is required');
  const modules=[],inputs=[];
  for(const supplier of suppliers){
    const source={...supplier,checkout:sources[supplier.key]||path.join(repository,supplier.path),revision:sourcePins[supplier.key]};
    if(!/^[a-f0-9]{40}$/.test(source.revision||''))throw Error(`Missing Android font source revision: ${supplier.key}`);
    if(git(source,['rev-parse','HEAD']).toString().trim()!==source.revision)throw Error(`Android font source revision changed: ${supplier.key}`);
    addLicenses(output,supplier.key,source.checkout);
 source.files=new Set(git(source,['ls-tree','-r','--name-only',source.revision,...(source.directory?[source.directory]:[])]).toString().trim().split('\n').filter(Boolean));
    if(source.files.has('noto-fonts.go'))inputs.push({source:source.key,revision:source.revision,path:'noto-fonts.go',sha256:digest(read(source,'noto-fonts.go'))});
    for(const file of source.files){
      if(path.posix.basename(file)!=='Android.bp')continue;
      const bytes=read(source,file),parsed=parseBlueprint(bytes.toString(),{file});
      inputs.push({source:source.key,revision:source.revision,path:file,sha256:digest(bytes)});
      for(const declaration of parsed.modules){
        const module={...declaration,source,file,directory:path.posix.dirname(file)};
        modules.push(module);
      }
    }
  }
  const fontModules=modules.filter(module=>['prebuilt_font','prebuilt_versioned_font'].includes(module.type));
  const configModules=modules.filter(module=>module.type==='prebuilt_fonts_xml');
  const byName=new Map();
  for(const module of [...fontModules,...configModules]){
    const name=module.properties.name;
    if(!safe(name)||name.includes('/')||byName.has(name))throw Error(`Invalid or duplicate Android font module: ${name}`);
    byName.set(name,module);
  }
  const root=path.join(output,'android-root'), products=[];
  const fontDirectory=path.join(root,'system/fonts'),etcDirectory=path.join(root,'system/etc');
  fs.rmSync(fontDirectory,{recursive:true,force:true});
  fs.mkdirSync(fontDirectory,{recursive:true});fs.mkdirSync(etcDirectory,{recursive:true});
  function copy(module,relative,target){
    const original=path.posix.normalize(path.posix.join(module.directory,relative)),bytes=read(module.source,original);
    fs.mkdirSync(path.dirname(target),{recursive:true});fs.writeFileSync(target,bytes);
    return {module:module.properties.name,source:module.source.key,revision:module.source.revision,definition:module.file,original,path:path.relative(output,target),bytes:bytes.length,sha256:digest(bytes)};
  }
  for(const module of fontModules){
    const p=module.properties;
    for(const required of list(p.required||[],`${p.name}.required`))if(!byName.has(required))throw Error(`Missing required font module: ${p.name} -> ${required}`);
    // The original noto-fonts.go selects defaultVersion when no Android release
    // build flag is supplied, as in SDK builds. Its file naming is retained.
    const original=module.type==='prebuilt_versioned_font'
      ?path.posix.join(p.versionDir||'font',p.defaultVersion||'',p.fontFile||p.name):p.src;
    if(module.type==='prebuilt_versioned_font'&&(!p.defaultVersion||!p.versionFlag))throw Error(`Incomplete versioned font module: ${p.name}`);
    if(!safe(original))throw Error(`Unresolved original font source: ${p.name}`);
    const filename=p.filename||p.name;
    if(!safe(filename)||filename.includes('/'))throw Error(`Invalid installed font filename: ${filename}`);
    const item=copy(module,original,path.join(fontDirectory,filename));
    item.required=p.required||[];
    if(module.type==='prebuilt_versioned_font')item.version={flag:p.versionFlag,value:p.defaultVersion};
    products.push(item);
    for(const alias of list(p.symlinks||[],`${p.name}.symlinks`)){
      if(!safe(alias)||alias.includes('/')||byName.has(alias))throw Error(`Invalid font compatibility symlink: ${alias}`);
      const target=path.join(fontDirectory,alias),link=filename;
      const prior=fs.lstatSync(target,{throwIfNoEntry:false});if(prior)fs.unlinkSync(target);
      fs.symlinkSync(link,target);products.push({module:p.name,path:path.relative(output,target),symlink:link});
    }
  }
  const references=new Set();
  for(const module of configModules){
    // No product-specific CJK flag is set: use the original module's default src.
    const p=module.properties,item=copy(module,p.src,path.join(etcDirectory,p.name));
    products.push(item);
    const xml=fs.readFileSync(path.join(output,item.path),'utf8').replace(/<!--[\s\S]*?-->/g,'');
    for(const match of xml.matchAll(/<font\b[^>]*>([\s\S]*?)<\/font>/g)){
      const file=match[1].replace(/<[^>]+>/g,'').trim();
      if(!safe(file)||file.includes('/')||!fs.existsSync(path.join(fontDirectory,file)))throw Error(`Unresolved original font configuration dependency: ${p.name} -> ${file}`);
      references.add(file);
    }
  }
  if(!configModules.length||!fontModules.length)throw Error('The original Android font component is empty');
  const inventory={schemaVersion:1,suppliers:suppliers.map(({key,url})=>({key,url,revision:sourcePins[key]})),inputs,products,configurationFontFiles:[...references].sort()};
  fs.writeFileSync(path.join(output,'font-resources.json'),JSON.stringify(inventory,null,2)+'\n');
  return {
    root,
    inventory,
    fontCount:fontModules.length,
    configCount:configModules.length
  };
}
