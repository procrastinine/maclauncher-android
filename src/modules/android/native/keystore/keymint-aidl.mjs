import fs from 'node:fs';
import {nativePathFlags} from '../upstream/native-paths.mjs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {parseBlueprint} from '../upstream/blueprint.mjs';
import {publisherList} from '../binder-rpc/build.mjs';
import {compileHostObject} from '../host-compile.mjs';
import {cachedBuildStep} from '../upstream/build-step.mjs';

const nativeRoot=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
export async function buildKeyMintAidl({output,backend,generated,binderSource,binderNdkAdaptation,ninja,python}) {
fs.mkdirSync(output,{recursive:true});
const binder=path.join(binderSource,'native/libs/binder');
const binderOwned=path.join(nativeRoot,'binder-rpc');
const sha=bytes=>createHash('sha256').update(bytes).digest('hex');
const propertyInclude=backend.property.include;
const keymaster=backend.sources.keymaster;
const definitions=fs.readFileSync(path.join(keymaster,'Android.bp'),'utf8');
const original=parseBlueprint(definitions,{file:'Android.bp'}).modules;
const modules=['libkeymint','lib_android_keymaster_keymint_utils'].map(name=>{
 const declaration=original.find(m=>m.properties.name===name);if(!declaration)throw Error('Missing module '+name);return declaration;
});
const units=new Map();
for(const m of modules)for(const src of m.properties.srcs){
 const file=path.join(keymaster,src),key='keymaster/'+src;
 if(!units.has(key))units.set(key,{key,file,sha256:sha(fs.readFileSync(file)),modules:[]});
 units.get(key).modules.push(m.properties.name);
}
for(const m of generated.records)for(const src of m.backends.ndk.files.filter(f=>f.file.endsWith('.cpp'))){
 const key=m.name+'-V'+m.version+'/'+path.relative(generated.output,src.file);
 if(sha(fs.readFileSync(src.file))!==src.sha256)throw Error('Changed generated input '+key);
 units.set(key,{key,file:src.file,sha256:src.sha256,modules:[m.name+'-V'+m.version+'-ndk']});
}
const dirs=backend.sources;
const includes=[propertyInclude,dirs.logging+'/liblog/include',binderNdkAdaptation,
 ...generated.records.map(m=>path.join(generated.output,m.name+'-V'+m.version,'ndk/include')),
 keymaster+'/include',keymaster+'/ng/include',dirs.libcppbor+'/include/cppbor',dirs.libhardware+'/include_all',dirs.libhardware+'/include_vendor',
 dirs.libbase+'/include',dirs.fmt+'/include',dirs.core+'/libcutils/include',dirs.core+'/libsystem/include',dirs.core+'/libutils/include',dirs.core+'/libutils/binder/include',
 binder+'/ndk/include_cpp',binder+'/ndk/include_ndk',binder+'/ndk/include_platform',binder+'/include',binder+'/liblog_stub/include',binderOwned+'/include',backend.crypto.include];
const flags=[...nativePathFlags({build:path.dirname(output),roots:[[binderSource,'/src/binder'],...Object.entries(dirs).map(([name,directory])=>[directory,'/src/'+name])]}),'-std=c++20','-arch','arm64','-mmacosx-version-min=13.0','-O1','-g','-fPIC','-fno-rtti','-fno-exceptions','-DKEYMASTER_NAME_TAGS','-DBINDER_WITH_KERNEL_IPC','-DANDROID_UTILS_CALLSTACK_ENABLED=0',...publisherList(binder,'binder_sdk_defaults','cflags').filter(f=>!f.includes('BINDER_DISABLE')),...includes.map(d=>'-I'+d),'-include',binderOwned+'/darwin.h'];
const adaptations=['android/binder_status.h','log/log.h'].map(file=>({file:path.join(binderNdkAdaptation,file),sha256:sha(fs.readFileSync(path.join(binderNdkAdaptation,file)))}));
adaptations.push(...backend.property.adaptations);
const objects=path.join(output,'objects');fs.mkdirSync(objects,{recursive:true});
const list=[...units.values()];
for(const u of list){
  const name=u.key.replaceAll('/','_')+'.o';u.object=path.join(objects,name);
  compileHostObject({name,inputs:[u.file],flags,compiler:'clang++',directory:path.join(output,'host-objects'),
    output:u.object,cwd:nativeRoot,ninja,python});
}
const archive=path.join(output,'libkeymint-aidl-host.a');
const ar=fs.realpathSync(execFileSync('xcrun',['--find','ar'],{encoding:'utf8'}).trim());
const archiveArgs=['rcs',archive,...list.map(u=>u.object)];
await cachedBuildStep({cache:path.join(output,'archive.reuse.json'),inputs:[fileURLToPath(import.meta.url),ar,...list.map(u=>u.object)],
  files:[archive],key:{ar,args:archiveArgs},build:()=>execFileSync(ar,archiveArgs,{stdio:'inherit'})});
const result={output,modules,units:list,flags,adaptations,archive};
fs.writeFileSync(path.join(output,'closure.json'),JSON.stringify(result,null,2)+'\n');
return result;
}
