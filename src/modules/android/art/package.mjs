import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
const sha=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const inside=(root,file)=>{const relative=path.relative(root,file);return relative===''||relative!=='..'&&!relative.startsWith('..'+path.sep)&&!path.isAbsolute(relative);};
function destinationPath(file){
  const missing=[];let parent=file;
  while(!fs.existsSync(parent)){missing.unshift(path.basename(parent));const next=path.dirname(parent);if(next===parent)throw Error('Android ART output parent is unavailable.');parent=next;}
  return path.join(fs.realpathSync(parent),...missing);
}

/** Publish the ART adapter separately from the user-acquired Android image.
 * All dependencies are explicit existing producer outputs; this never builds
 * native code, reads app data or includes an extracted Google system image. */
export function packageArtAdapter({supplierRoot,commonRoot,commonDeclaration,extractor,sourceArchive,originalImage,output,version},metadata) {
  if(!metadata||['fileInventory','readMachOLoadCommands','versionAtLeast','validatePublicMetadata','validateDeclaration','verifyPayload'].some(name=>typeof metadata[name]!=='function'))
    throw Error('Android ART packaging requires explicit package metadata services.');
  if(typeof version!=='string'||!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(version)||version.includes('..'))
    throw Error('Android ART packaging requires an explicit package version.');
  for(const value of [supplierRoot,commonRoot,extractor,sourceArchive,output])
    if(typeof value!=='string'||!path.isAbsolute(value))throw Error('Android ART packaging requires absolute producer paths.');
  if(fs.existsSync(output))throw Error('Android ART package output already exists.');
  const destination=destinationPath(output);
  for(const root of [supplierRoot,commonRoot].map(file=>fs.realpathSync(file)))
    if(inside(root,destination)||inside(destination,root))throw Error('Android ART package output overlaps a producer tree.');
  const supplier=JSON.parse(fs.readFileSync(path.join(supplierRoot,'runtime.json'),'utf8'));
  const common=JSON.parse(fs.readFileSync(path.join(commonRoot,'runtime.json'),'utf8'));
  if(commonDeclaration){
    if(commonDeclaration.manifestRelative!=='runtime.json'||commonDeclaration.manifestSha256!==sha(path.join(commonRoot,'runtime.json')))
      throw Error('Android ART common runtime differs from its producer manifest.');
  }
  if(supplier.schemaVersion!==1||supplier.execution!=='art'||supplier.components?.image!=='image'||supplier.imageAcquisition?.delivery!=='explicit-download'||
    common.profile?.javaMajor!==21)throw Error('Android ART producer profiles are incompatible.');
  // The consumer extracts onto a case-sensitive volume. A development tree
  // copied onto a different filesystem is not its authoritative identity.
  if(originalImage?.archiveSha256!==supplier.android?.imageSha256||
    !/^[a-f0-9]{64}$/.test(originalImage?.content?.sha256||'')||
    !Number.isSafeInteger(originalImage.content.records)||originalImage.content.records<1||
    !Number.isSafeInteger(originalImage.content.bytes)||originalImage.content.bytes<1)
    throw Error('Android ART packaging requires the verified original system image identity.');
  supplier.platformSupplier={...supplier.platformSupplier,image:Object.fromEntries(
    ['sha256','records','bytes'].map(key=>[key,originalImage.content[key]]))};
  fs.mkdirSync(path.dirname(output),{recursive:true});
  const stage=fs.mkdtempSync(path.join(path.dirname(output),'.android-art-'));
  const activeDirectories=new Set();
  const copy=(from,to,owner)=>{
    const actual=fs.realpathSync(from),relative=path.relative(fs.realpathSync(owner),actual);
    if(relative==='..'||relative.startsWith('..'+path.sep)||path.isAbsolute(relative))throw Error('Android ART producer member escapes its source.');
    const stat=fs.statSync(actual);
    if(stat.isDirectory()){
      if(activeDirectories.has(actual))throw Error('Android ART producer directory aliases form a cycle.');
      activeDirectories.add(actual);
      try{
        fs.mkdirSync(to,{recursive:true,mode:0o755});
        for(const name of fs.readdirSync(actual).sort()){
          copy(path.join(actual,name),path.join(to,name),owner);
        }
      }finally{activeDirectories.delete(actual);}
    }else if(stat.isFile()){
      fs.mkdirSync(path.dirname(to),{recursive:true});
      fs.copyFileSync(actual,to,fs.constants.COPYFILE_FICLONE);fs.chmodSync(to,stat.mode&0o111?0o755:0o644);
    }else throw Error('Android ART producer contains a special file.');
  };
  try{
    // Original in-tree aliases are materialized as regular files for the same
    // immutable package verifier used by other Android runtime downloads.
    for(const name of fs.readdirSync(supplierRoot).sort())if(name!==supplier.components.image&&name!=='runtime.json'){
      // A producer may supply its Cargo output as halRoot. Only the runtime
      // services and shared libraries belong in the downloaded adapter.
      if(name===supplier.components.halRoot){
        for(const file of ['allocator-service','audio-service','composer-service','libaim_allocator.so','libgles.so','libmapper.so'])
          copy(path.join(supplierRoot,name,file),path.join(stage,'art',name,file),supplierRoot);
      }else copy(path.join(supplierRoot,name),path.join(stage,'art',name),supplierRoot);
    }
    copy(extractor,path.join(stage,'art/host/android-image-extract'),path.dirname(extractor));
    copy(sourceArchive,path.join(stage,'source.tar.gz'),path.dirname(sourceArchive));
    copy(path.join(commonRoot,'native/SDL3.framework'),path.join(stage,'native/SDL3.framework'),commonRoot);
    supplier.components={...supplier.components,imageExtractor:'host/android-image-extract'};
    metadata.validatePublicMetadata(supplier);
    fs.writeFileSync(path.join(stage,'art/runtime.json'),JSON.stringify(supplier,null,2)+'\n');
    const dependencies=[];let minimumMacOS='0.0';
    const files=metadata.fileInventory(stage).map(relative=>{
      const file=path.join(stage,relative),stat=fs.statSync(file),record={path:relative,size:stat.size,mode:stat.mode&0o777,sha256:sha(file)};
      let commands;
      try{commands=metadata.readMachOLoadCommands(file);}catch{}
      if(commands){
        record.architectures=commands.architectures;
        for(const slice of commands.slices)if(metadata.versionAtLeast(slice.minimumMacOS,minimumMacOS))minimumMacOS=slice.minimumMacOS;
        const links=[...new Set(commands.slices.flatMap(slice=>slice.dependencies.map(item=>item.path)))];
        for(const link of links)if(!link.startsWith('/System/Library/')&&!link.startsWith('/usr/lib/')){
          const candidates=commands.slices.flatMap(slice=>slice.runPaths.map(rpath=>link.replace('@rpath/',rpath+'/')))
            .concat(link).filter(value=>value.startsWith('@loader_path/')).map(value=>path.resolve(path.dirname(file),value.slice(13)));
          if(!candidates.some(value=>value.startsWith(stage+path.sep)&&fs.existsSync(value)))throw Error('Android ART native dependency is missing: '+link);
        }
        dependencies.push({file:relative,links});
      }
      return record;
    });
    const sourceHash=sha(sourceArchive),inputHash=crypto.createHash('sha256').update(JSON.stringify(files)).digest('hex');
    const manifest={schemaVersion:1,version,profile:{id:'maclauncher-android-art-16',artExecutionVersion:1},
      minimumMacOS,architectures:['arm64'],sourceHash,inputHash,dependencies,files};
    metadata.validatePublicMetadata(manifest);
    fs.writeFileSync(path.join(stage,'runtime.json'),JSON.stringify(manifest,null,2)+'\n');
    const declaration={id:'android-art',version,arch:'arm64',launcherArchitectures:['arm64'],minimumMacOS,
      directoryRelative:'android-art/'+version+'/arm64',manifestRelative:'runtime.json',manifestSha256:sha(path.join(stage,'runtime.json')),
      sourceRelative:'source.tar.gz',sourceSha256:sourceHash,files:[...files,{path:'runtime.json',size:fs.statSync(path.join(stage,'runtime.json')).size,mode:0o644,sha256:sha(path.join(stage,'runtime.json'))}]};
    metadata.validateDeclaration(declaration);metadata.verifyPayload(stage,declaration);
    fs.renameSync(stage,output);return {root:output,declaration};
  }finally{fs.rmSync(stage,{recursive:true,force:true});}
}
