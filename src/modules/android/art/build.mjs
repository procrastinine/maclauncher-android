import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {writeChanged,copyChanged} from '../native/upstream/source-files.mjs';
import {supplier} from './source.mjs';
import {buildArtHost} from './build-host.mjs';
import {buildArtJava} from './build-java.mjs';
import {buildArtGuest} from './build-guest.mjs';
import {buildArtSensors} from './sensors/build.mjs';
import {buildArtComposer} from './composer/build.mjs';
import {buildArtGraphics} from './graphics/build.mjs';
import {buildArtNativeBridge} from './build-arm32.mjs';
import {components as arm32Components} from '../native/arm32/components.mjs';
import {originalTreeFiles} from '../native/upstream/reuse.mjs';
import {buildBootImage} from './boot-image.mjs';
import {buildPlatformPackageCatalog} from '../native/platform-packages.mjs';

import components from './components.json' with {type:'json'};

/** Component destinations are shared by producers and explicit reassembly. */
export function artSupplierComponents(inputs={}){
 const surfaces=inputs.compositionSurface?{compositionSurface:'guest/libmaclauncher-composition-surface.so'}:{};
 return {...components,...surfaces,...(inputs.platformPackages?{platformPackages:'config/platform-packages.json'}:{}),...(inputs.serviceWaiter?{serviceWaiter:'guest/android-art-service-wait'}:{}),...(inputs.bootImage?{bootImage:'boot-image'}:{}),...(inputs.linuxTranslate?{linuxTranslate:'host/linux-translate'}:{}),...(inputs.ptyHolder?{ptyHolder:'host/aim-pty-holder'}:{}),...(inputs.glesLibrary?{glesLibrary:'hal/libgles.so'}:{}),...(inputs.nativeBridge?{nativeBridge:'guest/'+arm32Components.bindings.art,nativeBridgeNotices:'licenses/native-bridge',...Object.fromEntries(arm32Components.preloads.map(item=>[item.name,'guest/'+item.file]))}:{})};
}

/** Complete delivered image identity. This runs when producing a supplier,
 * never when resolving or launching it. Directory modes, symlink text and all
 * regular-file bytes participate without following links outside the image. */
export function imageContentIdentity(root){
 const hash=crypto.createHash('sha256'),buffer=Buffer.allocUnsafe(1024*1024);let records=0,bytes=0;
 function visit(relative){
  const file=path.join(root,relative),stat=fs.lstatSync(file),name=relative.split(path.sep).join('/'),mode=stat.mode&0o7777;
  let row;
  if(stat.isSymbolicLink())row=[name,'link',mode,fs.readlinkSync(file)];
  else if(stat.isDirectory())row=[name,'directory',mode];
  else if(stat.isFile()){
   const content=crypto.createHash('sha256'),fd=fs.openSync(file,'r');
   try{let count;while((count=fs.readSync(fd,buffer,0,buffer.length,null))>0)content.update(buffer.subarray(0,count));}finally{fs.closeSync(fd);}
   bytes+=stat.size;row=[name,'file',mode,stat.size,content.digest('hex')];
  }else throw Error('Android image member is not a regular file, directory or symlink: '+name);
  hash.update(JSON.stringify(row)+'\n');records++;
  if(stat.isDirectory())for(const name of fs.readdirSync(file).sort())visit(path.join(relative,name));
 }
 visit('');return {sha256:hash.digest('hex'),records,bytes};
}

/** Assemble a versioned supplier from explicit producer outputs. No live
 * checkout paths survive in the delivered manifest; app/profile data is never
 * a supplier input. This is separate from publishing the Android declaration. */
export function assembleArtSupplier({output,inputs,bootClasspath,sourceVersion,baseSupplier}){
 output=path.resolve(output);
 if(fs.existsSync(output))throw Error('ART supplier output already exists');
 const parent=path.dirname(output);fs.mkdirSync(parent,{recursive:true});const temporary=fs.mkdtempSync(path.join(parent,'.art-supplier-'));
 try{
  const delivered=artSupplierComponents(inputs);
  for(const [name,relative]of Object.entries(delivered)){
   const source=path.resolve(inputs[name]||'');if(!inputs[name]||!fs.existsSync(source))throw Error('Missing ART producer output: '+name);
   const destination=path.join(temporary,relative);fs.mkdirSync(path.dirname(destination),{recursive:true});
   fs.cpSync(source,destination,{recursive:true,mode:fs.constants.COPYFILE_FICLONE,dereference:false,verbatimSymlinks:true});
  }
  const license=supplier.imageAcquisition.license;
  copyChanged(path.join(import.meta.dirname,license.path),path.join(temporary,license.path));
  const android={...supplier.android,architectures:[...supplier.android.architectures,...(inputs.nativeBridge?['armeabi-v7a','armeabi']:[])]};
  const bootImage=inputs.bootImage?JSON.parse(fs.readFileSync(path.join(inputs.bootImage,'boot-image.json'),'utf8')):null;
  // An explicitly selected immutable supplier can lend its complete image
  // identity only when this assembly retains that exact image component.
  // Native/Java replacements then copy the image without reading it again.
  const previous=baseSupplier?.manifest,retained=previous?.platformSupplier;
  const sameImage=retained?.schemaVersion===1&&/^[a-f0-9]{64}$/.test(retained.image?.sha256||'')&&
   fs.realpathSync(inputs.image)===fs.realpathSync(path.join(baseSupplier.root,previous.components.image));
  const image=sameImage?retained.image:imageContentIdentity(path.join(temporary,delivered.image));
  const catalog=inputs.platformPackages?(()=>{const file=path.join(temporary,delivered.platformPackages),stat=fs.statSync(file);return {sha256:crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'),size:stat.size,mode:stat.mode&0o7777};})():null;
  const platformSupplier={schemaVersion:1,image,catalog};
  const manifest={schemaVersion:1,execution:'art',supplier:supplier.id,version:sourceVersion,android,imageAcquisition:supplier.imageAcquisition,hostcallVersion:supplier.hostcallVersion,components:delivered,platformSupplier,bootClasspath,...(bootImage?{bootImage}:{})};
  writeChanged(path.join(temporary,'runtime.json'),JSON.stringify(manifest,null,2)+'\n');
  fs.renameSync(temporary,output);return manifest;
 }finally{fs.rmSync(temporary,{recursive:true,force:true});}
}

/** The producer may reuse original AIM build outputs. It never boots AIM or
 * invokes an Android product graph to build these two owned boundary units. */
export async function buildArt({output,host,java,guest,sensors,composer,graphics,nativeBridge,inputs,bootClasspath,baseSupplier}){
 if(nativeBridge)host={...host,arm32:nativeBridge.arm32.cpu};
 const guestProducts=guest?buildArtGuest({...guest,output:path.join(output,'build/guest')}):{};
 const sensorProducts=sensors?buildArtSensors({...sensors,output:path.join(output,'build/sensors')}):{};
 const composerProducts=composer?buildArtComposer({...composer,output:path.join(output,'build/composer')}):{};
 const graphicsProducts=graphics?buildArtGraphics({...graphics,output:path.join(output,'build/graphics')}):{};
 const bridgeProducts=nativeBridge?await buildArtNativeBridge({...nativeBridge,output:path.join(output,'build/native-bridge')}):{};
 const native=buildArtHost({...host,output:path.join(output,'build/host')});
 const bootstrap=await buildArtJava({...java,output:path.join(output,'build/java')});
 const products={...inputs,...guestProducts,...sensorProducts,...composerProducts,...graphicsProducts,...bridgeProducts,...native,...bootstrap};
 const platformPackages=path.join(output,'build/platform-packages.json');
 buildPlatformPackageCatalog({image:products.image,output:platformPackages});
 products.platformPackages=platformPackages;
 const boot=await buildBootImage({output:path.join(output,'boot-image-cache'),aim:host.aim,components:products,bootClasspath});
 const identity=crypto.createHash('sha256');
 identity.update(boot.identity);
 for(const input of [...originalTreeFiles(import.meta.dirname),native.hostLibrary,bootstrap.bootstrapDex,...Object.values(guestProducts),...Object.values(sensorProducts),...Object.values(composerProducts),...Object.values(graphicsProducts),...Object.values(bridgeProducts)])
  for(const file of fs.statSync(input).isDirectory()?originalTreeFiles(input):[input])identity.update(fs.readFileSync(file));
 return assembleArtSupplier({output:path.join(output,'runtime'),inputs:{...products,bootImage:boot.root},bootClasspath,baseSupplier,sourceVersion:'0.1.0-'+identity.digest('hex').slice(0,16)});
}
