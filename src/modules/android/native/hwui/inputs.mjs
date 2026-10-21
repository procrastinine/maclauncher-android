import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';

const manifest=JSON.parse(fs.readFileSync(new URL('./sources.json',import.meta.url)));
const sha256=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
const within=(root,file)=>file===root||file.startsWith(root+path.sep);
function relative(value){if(typeof value!=='string'||path.isAbsolute(value)||value.split(/[\\/]/).includes('..'))throw Error('Invalid original source path');return value==='.'?'':value;}
function directory(value,label){if(typeof value!=='string'||!path.isAbsolute(value))throw Error('An explicit absolute '+label+' is required');const root=fs.realpathSync(value);if(!fs.statSync(root).isDirectory())throw Error(label+' is not a directory');return root;}
function object(type,bytes){return crypto.createHash('sha1').update(type+' '+bytes.length+'\0').update(bytes).digest('hex');}
/** Git object identity is independent of checkout timestamps and archive names. */
function gitObject(file){
 const s=fs.lstatSync(file);
 if(s.isSymbolicLink())return object('blob',Buffer.from(fs.readlinkSync(file)));
 if(s.isFile())return object('blob',fs.readFileSync(file));
 if(!s.isDirectory())throw Error('Original source is not a file or directory');
 const entries=fs.readdirSync(file).filter(name=>name!=='.git').map(name=>({name,stat:fs.lstatSync(path.join(file,name))}));
 entries.sort((a,b)=>Buffer.compare(Buffer.from(a.name+(a.stat.isDirectory()?'/':'')),Buffer.from(b.name+(b.stat.isDirectory()?'/':''))));
 return object('tree',Buffer.concat(entries.map(({name,stat})=>Buffer.concat([Buffer.from((stat.isDirectory()?'40000':stat.isSymbolicLink()?'120000':stat.mode&0o100?'100755':'100644')+' '+name+'\0'),Buffer.from(gitObject(path.join(file,name)),'hex')]))));
}
function parts(input,key){
 const value=typeof input==='string'?{root:input}:input;
 if(!value||typeof value!=='object')throw Error('An explicit HWUI source is required: '+key);
 return [{root:directory(value.root,key+' source'),prefix:''},...(value.parts||[]).map(part=>({root:directory(part.root,key+' source part'),prefix:relative(part.prefix||'.')}))].sort((a,b)=>b.prefix.length-a.prefix.length);
}
function sourceFile(roots,rel){
 for(const part of roots){if(part.prefix&&rel!==part.prefix&&!rel.startsWith(part.prefix+'/'))continue;const tail=part.prefix?rel.slice(part.prefix.length).replace(/^\//,''):rel;const file=path.join(part.root,tail);try{fs.lstatSync(file);return file;}catch(error){if(error.code!=='ENOENT'&&error.code!=='ENOTDIR')throw error;}}
 throw Error('Required original source is absent: '+rel);
}
/** Verify exact committed contents or exact original archive tree/blob bytes.
 * Archive parts are useful for source distributions that export pinned subtrees.
 * Their claimed revision alone never authorizes content. */
export function verifyHwuiSource(record,input){
 const roots=parts(input,record.key),checks=[];
 for(const check of record.checks){const rel=relative(check.path),file=sourceFile(roots,rel);let identity;
  // A whole Git checkout avoids hashing unrelated source trees. Dirty tracked
  // or additional source files cannot enter its whole-root projection.
  if(!rel&&roots.length===1){
   try{
    const git=args=>execFileSync('git',['-C',file,...args],{encoding:'utf8',stdio:['ignore','pipe','pipe']}).trim();
    if(fs.realpathSync(git(['rev-parse','--show-toplevel']))===file){
     if(git(['rev-parse','HEAD'])!==record.revision)throw Error('Original source revision differs: '+record.key);
     git(['diff','--quiet','HEAD','--']);if(git(['ls-files','--others','--exclude-standard']).length||git(['ls-files','--others','--ignored','--exclude-standard']).length)throw Error('Original source has untracked files: '+record.key);
     identity=git(['rev-parse','HEAD^{tree}']);
    }
   }catch(error){if(error.message?.startsWith('Original source'))throw error;if(fs.existsSync(path.join(file,'.git')))throw Error('Original source checkout is not clean: '+record.key,{cause:error});}
  }
  identity??=gitObject(file);
  if(identity!==check.object)throw Error('Original source content differs: '+record.key+'/'+(rel||'.'));
  const s=fs.lstatSync(file),mode=s.isDirectory()?16384:s.isSymbolicLink()?40960:s.mode&0o100?33261:33188;if((check.type==='tree')!==s.isDirectory()||check.mode!==undefined&&check.mode!==mode)throw Error('Original source kind or mode differs: '+record.key+'/'+rel);
  checks.push({...check,path:rel||'.',file});
 }
 return{key:record.key,repository:record.repository,revision:record.revision,roots,checks};
}
function alias(target,source){
 fs.mkdirSync(path.dirname(target),{recursive:true});
 try{const s=fs.lstatSync(target);if(s.isSymbolicLink()&&fs.realpathSync(target)===fs.realpathSync(source))return;throw Error('HWUI work source already has different contents: '+target);}catch(error){if(error.code!=='ENOENT')throw error;}
 fs.symlinkSync(source,target);
}
export function materializeHwuiSource(destination,verified){
 for(const check of verified.checks){const rel=relative(check.path);alias(path.join(destination,rel),check.file);}
 return destination;
}
function tool(value,name,records){
 if(value&&typeof value==='object'&&!Array.isArray(value)&&!('path'in value))return Object.fromEntries(Object.entries(value).map(([key,item])=>[key,tool(item,name+'.'+key,records)]));
 const record=typeof value==='string'?{path:value}:value;
 if(!record||typeof record.path!=='string'||!path.isAbsolute(record.path))throw Error('An explicit HWUI tool input is required: '+name);
 const file=path.resolve(record.path),s=fs.statSync(file);if(!s.isFile()&&!s.isDirectory())throw Error('Invalid HWUI tool input: '+name);
 if(s.isFile()){
  const hash=sha256(fs.readFileSync(file));if(record.sha256&&record.sha256!==hash)throw Error('HWUI tool hash differs: '+name);
  if(!/(?:libclang|archive|library)/i.test(name)&&!/\.(?:a|dylib|so)(?:\.\d+)*$/i.test(file))fs.accessSync(file,fs.constants.X_OK);
  records.push({name,path:file,sha256:hash,size:s.size});
 }else records.push({name,path:file,directory:true});
 return file;
}
function imageFile(root,guest){
 let file=path.join(root,relative(guest));let hops=0;
 while(fs.lstatSync(file).isSymbolicLink()){
  if(++hops>20)throw Error('Recursive original image alias');const next=fs.readlinkSync(file);file=path.isAbsolute(next)?path.join(root,next.replace(/^\//,'')):path.resolve(path.dirname(file),next);
  if(!within(root,file))throw Error('Original image alias leaves image root');
 }
 if(!within(root,file)||!fs.statSync(file).isFile())throw Error('Original image file is unavailable: '+guest);return file;
}
/** Resolve the complete original HWUI source/tool/image closure offline.
 * Compilation, generated headers and compiler defaults belong to the producer.
 * Supplied original sources and image remain untouched. */
export async function prepareHwuiInputs({work,sources,tools,image}={}){
 if(typeof work!=='string'||!path.isAbsolute(work))throw Error('An explicit absolute HWUI work directory is required');
 work=path.resolve(work);if(!sources||!tools||!image)throw Error('Explicit HWUI sources, tools and original image are required');
 const verified=manifest.sources.map(record=>verifyHwuiSource(record,sources[record.key]));
 const root=directory(image.root,'original Android image');if(image.contentSha256!==manifest.target.imageContentSha256)throw Error('Original Android image content identity differs');
 const imageFiles=manifest.imageFiles.map(record=>{const file=imageFile(root,record.path),bytes=fs.readFileSync(file);if(bytes.length!==record.size||sha256(bytes)!==record.sha256)throw Error('Original Android image dependency differs: '+record.path);return{...record,file};});
 const toolRecords=[],resolvedTools=Object.fromEntries(Object.entries(tools).map(([name,value])=>[name,tool(value,name,toolRecords)]));
 for(const record of verified)for(const part of record.roots)if(within(part.root,work)||within(work,part.root))throw Error('HWUI work and original source roots must be separate');
 if(within(root,work)||within(work,root))throw Error('HWUI work and original image roots must be separate');
 // Resolve existing ancestors before creating any work path, including callers
 // that use a symlinked work parent.
 let ancestor=work;while(!fs.existsSync(ancestor))ancestor=path.dirname(ancestor);const physical=path.join(fs.realpathSync(ancestor),path.relative(ancestor,work));
 for(const record of verified)for(const part of record.roots)if(within(part.root,physical)||within(physical,part.root))throw Error('HWUI work alias overlaps original sources');
 if(within(root,physical)||within(physical,root))throw Error('HWUI work alias overlaps original image');
 const ctx={root:work,work,module:path.join(work,'module'),sources:{},tools:resolvedTools,image:{root,contentSha256:image.contentSha256}};
 for(const[name,rel]of Object.entries(manifest.derivedWork)){ctx[name]=path.join(work,rel);fs.mkdirSync(ctx[name],{recursive:true});}
 ctx.originalSources=Object.fromEntries(manifest.sources.map(record=>[record.key,{record,input:sources[record.key]}]));
 for(const record of verified){const destination=path.join(ctx.source,record.key);ctx.sources[record.key]=materializeHwuiSource(destination,record);}
 ctx.includes=manifest.includes.map(row=>row.source?path.join(ctx.sources[row.source],row.path):path.join(work,row.work));
 ctx.options=Object.fromEntries(Object.entries(manifest.optionSources).map(([name,key])=>[name,ctx.sources[key]]));
 const state={schema:1,target:manifest.target,sources:verified.map(({key,repository,revision,checks})=>({key,repository,revision,checks:checks.map(({file,...record})=>record)})),tools:toolRecords,image:{root,contentSha256:image.contentSha256,files:imageFiles.map(({file,...record})=>record)}};
 fs.writeFileSync(path.join(work,'hwui-inputs.json'),JSON.stringify(state,null,2)+'\n');return ctx;
}
