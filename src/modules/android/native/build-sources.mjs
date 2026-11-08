import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {pathToFileURL} from 'node:url';

const digest=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
// Legacy producer paths may be aliases into this pinned source owner. Cache
// directories remain with the caller; inventories contain the owner only.
function sourceRoot(root){
  root=fs.realpathSync(root);
  const entry=path.join(root,'build.mjs');
  return fs.existsSync(entry)&&fs.lstatSync(entry).isSymbolicLink()?path.dirname(fs.realpathSync(entry)):root;
}
// One complete inventory serves compilation and staging. New source files are
// included automatically; generated products never enter the source tree.
export function sourceInventory(root,destination,{links=false,exclude=[]}={}){
  root=sourceRoot(root);
  const files=[];
  function visit(relative){
    for(const entry of fs.readdirSync(path.join(root,relative),{withFileTypes:true}).sort((a,b)=>a.name.localeCompare(b.name))){
      const name=path.join(relative,entry.name),file=path.join(root,name);
      if(exclude.includes(name))continue;
      if(entry.isDirectory()){visit(name);continue;}
      if(entry.isSymbolicLink()&&links){files.push({path:name,link:fs.readlinkSync(file)});continue;}
      if(!entry.isFile()&&!(entry.isSymbolicLink()&&fs.statSync(file).isFile()))throw Error('Android build source must be a regular file: '+name);
      const bytes=fs.readFileSync(file);files.push({path:name,sha256:digest(bytes)});
      if(destination){const target=path.join(destination,name);fs.mkdirSync(path.dirname(target),{recursive:true});fs.writeFileSync(target,bytes);}
    }
  }
  visit('');return files;
}
export function productInventory(root){
  return sourceInventory(root,undefined,{links:true,exclude:['build','source','sources','build-source.json','build-receipt.json']});
}
/** Private receipt inputs become logical source identities when publishing
 * source metadata. Real paths remain available only to the local builder. */
export function buildPathMappings({source,build,output,options}){
  const mappings=new Map();
  function add(directory,identity){
    if(typeof directory!=='string'||!path.isAbsolute(directory))return;
    if(mappings.has(path.resolve(directory)))return;
    const locations=[path.resolve(directory)];
    try{locations.push(fs.realpathSync(directory));}catch(error){if(error.code!=='ENOENT'&&error.code!=='ENOTDIR')throw error;}
    for(const location of locations)for(const from of [location,pathToFileURL(location).pathname])
      if(!mappings.has(from))mappings.set(from,identity);
    try{if(fs.lstatSync(directory).isSymbolicLink())add(path.resolve(path.dirname(directory),fs.readlinkSync(directory)),identity);}
    catch(error){if(error.code!=='ENOENT'&&error.code!=='ENOTDIR')throw error;}
  }
  add(source,'ANDROID_ADAPTER');add(path.resolve(source,'../build-tools'),'ANDROID_BUILD_TOOLS');
  add(build,'ANDROID_BUILD');add(output,'ANDROID_RUNTIME');
  for(const [name,value]of Object.entries(options).sort(([a],[b])=>a.localeCompare(b)))
    add(value,'INPUT_'+name.toUpperCase().replace(/[^A-Z0-9_]/g,'_'));
  return [...mappings].map(([from,to])=>({from,to}));
}
export function sourceIdentity(ownedSources,profileBytes,buildToolsSources=[],artSources=[]){
  const profileSha256=digest(profileBytes);
  const identity={ownedSources,profileSha256,...(buildToolsSources.length?{buildToolsSources}:{}),...(artSources.length?{artSources}:{})};
  return {...identity,sourceHash:digest(JSON.stringify(identity))};
}
export function snapshotSources(root,destination,{toolInputs}={}){
  root=sourceRoot(root);
  sourceInventory(root,path.join(destination,'native'));
  if(toolInputs){
    const tools=path.join(destination,'native/tools');fs.mkdirSync(tools,{recursive:true});
    for(const name of ['asm-binary.json','apksig-binary.json'])
      for(const file of JSON.parse(fs.readFileSync(path.join(root,name))).files){
        if(path.basename(file.path)!==file.path)throw Error('Invalid Android tool input path');
        fs.copyFileSync(path.join(toolInputs,file.path),path.join(tools,file.path));
      }
  }
  const ownedSources=sourceInventory(path.join(destination,'native'));
  const profile=fs.readFileSync(path.join(root,'../profile.js'));
  fs.writeFileSync(path.join(destination,'profile.js'),profile);
  const art=path.join(root,'../art');
  const artSources=fs.existsSync(art)?sourceInventory(art,path.join(destination,'art'),{exclude:['launch.mjs','layout.mjs','isolation.mjs','composition.mjs','composition-cache.mjs']}):[];
  return sourceIdentity(ownedSources,profile,[],artSources);
}

/** Native suppliers reuse the complete shared build helpers and their binary
 * catalog. Capturing those inputs does not evaluate the Android platform graph. */
export function snapshotBuildCompanions({repository,destination}){
  const helpers='src/modules/shared/runtime',companions=[
    'src/modules/android/apk.js','src/modules/android/binary-manifest.js'
  ];
  sourceInventory(path.join(repository,helpers),path.join(destination,helpers));
  // apk.js also imports shared/runtime/zip, already retained above. The
  // original relative layout keeps that complete metadata-only closure intact.
  for(const companion of companions){
    const target=path.join(destination,companion);fs.mkdirSync(path.dirname(target),{recursive:true});
    fs.copyFileSync(path.join(repository,companion),target);
  }
  return {root:destination};
}
