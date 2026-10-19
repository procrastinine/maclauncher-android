import fs from 'node:fs';
import {nativePathFlags} from '../upstream/native-paths.mjs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {prepareAuthorization} from './authorization.mjs';
import {prepareServiceAccess} from './service-access.mjs';

const root=path.dirname(fileURLToPath(import.meta.url));
const run=(command,args)=>execFileSync(command,args,{stdio:'inherit'});

/** One service DSO links original Rust services, software KeyMint and shared owners. */
export function linkKeyStore({output,sources,nativeCheckout,nativeRevision,binder,foundation,wrapper,rustLibrary,attestation,nativeHal,crypto,processLibrary}) {
  fs.mkdirSync(output,{recursive:true});
  const authorization=prepareAuthorization({output:path.join(output,'authorization'),source:sources.sepolicy});
  const access=prepareServiceAccess({output:path.join(output,'authorization'),source:nativeCheckout,revision:nativeRevision});
  const flags=[...binder.flags,...wrapper.flags,'-fexceptions'];
  const objects=[];
  for(const [name,file,extra] of [
    ['KeyStoreBootstrap',path.join(root,'KeyStoreBootstrap.cpp'),[]],
    ['sockets_unix',path.join(binder.output,'source/core/libcutils/sockets_unix.cpp'),[]],
    ['Access',access.source,['-DKL_HOST_SERVICE_ACCESS','-I'+access.include,'-I'+path.join(sources.selinux,'libselinux/include')]],
    ['ServiceAccess',path.join(root,'ServiceAccess.cpp'),['-DKL_HOST_SERVICE_ACCESS','-I'+access.include,'-I'+path.join(sources.selinux,'libselinux/include')]],
  ]){
    const object=path.join(output,name+'.o');
    run('clang++',[...flags,...extra,'-c',file,'-o',object]);objects.push(object);
  }
  const auth=path.join(output,'authorization.o');
  run('clang',[...nativePathFlags({build:output,roots:Object.entries(sources).map(([name,directory])=>[directory,'/src/'+name])}),'-arch','arm64','-mmacosx-version-min=13.0','-O2','-fvisibility=hidden','-I'+authorization.include,'-I'+path.join(sources.selinux,'libselinux/include'),'-c',path.join(root,'authorization.c'),'-o',auth]);
  objects.push(auth);
  const shared=new Set(foundation.shared.map(unit=>unit.key));
  const ownObjects=foundation.units.filter(unit=>!shared.has(unit.key)).map(unit=>unit.object);
  const library=path.join(output,'libandroid-keystore.dylib');
  const args=['-arch','arm64','-mmacosx-version-min=13.0','-dynamiclib','-Wl,-install_name,@loader_path/libandroid-keystore.dylib',
    ...objects,rustLibrary,...wrapper.units.map(unit=>unit.object),...ownObjects,
    attestation.archive,nativeHal.archive,...crypto.archives,
    binder.library,crypto.crypto,processLibrary,'-lsqlite3','-framework','Security','-framework','CoreFoundation','-o',library];
  run('clang++',args);
  run('install_name_tool',['-change','@rpath/libklepton-jvm.dylib','@loader_path/libklepton-jvm.dylib',library]);
  run('install_name_tool',['-change','@rpath/libandroid-binder-rpc.dylib','@loader_path/libandroid-binder-rpc.dylib',library]);
  run('codesign',['--force','--sign','-',library]);
  return {library};
}
