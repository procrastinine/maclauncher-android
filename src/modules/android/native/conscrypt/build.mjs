import {copyOptional} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources,publisherList} from '../binder-rpc/build.mjs';
import {nativePathFlags,nativeFlagsString} from '../upstream/native-paths.mjs';

const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));
const run=(command,args)=>execFileSync(command,args,{stdio:'inherit'});

/** The complete original Android JNI table and its matching BoringSSL modules. */
export function buildConscrypt({conscryptSource,boringsslSource,binder,nativehelperSource,jdk,output}){
  const source=path.join(output,'source'),conscrypt=path.join(source,'conscrypt'),boringssl=path.join(source,'boringssl');
  const portable=nativePathFlags({source,build:output,roots:[[jdk,'/src/jdk']]});
  exportSources(conscryptSource,pins.aospConscrypt,['Android.bp','common/src/jni','apex/ca-certificates/files','LICENSE','NOTICE'],conscrypt);
  exportSources(boringsslSource,pins.aospBoringSSL,['CMakeLists.txt','NOTICE','src/LICENSE','src/crypto','src/ssl','src/include','src/gen','src/third_party/fiat','src/tool'],boringssl);
  // This Android snapshot's generated CMake file still requests C++14 although
  // its original public headers use C++17 deduction guides. Keep every source
  // and architecture choice in the publisher's targets, correcting only this
  // stale language-level setting in the exported build tree.
  const cmake=path.join(boringssl,'CMakeLists.txt');
  const original=fs.readFileSync(cmake,'utf8'),setting='set(CMAKE_CXX_STANDARD 14)';
  if(original.split(setting).length!==2)throw Error('BoringSSL C++ build setting changed');
  fs.writeFileSync(cmake,original.replace(setting,'set(CMAKE_CXX_STANDARD 17)'));
  const build=path.join(output,'boringssl');
  run('cmake',['-S',boringssl,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0',...['C','CXX','ASM'].map(language=>'-DCMAKE_'+language+'_FLAGS='+nativeFlagsString(portable)),'-DCMAKE_POSITION_INDEPENDENT_CODE=ON','-DBUILD_SHARED_LIBS=ON','-DCMAKE_INSTALL_NAME_DIR=@loader_path','-DCMAKE_BUILD_WITH_INSTALL_NAME_DIR=ON','-DCMAKE_SKIP_BUILD_RPATH=ON','-DCMAKE_LIBRARY_OUTPUT_DIRECTORY='+path.resolve(output)]);
  run('cmake',['--build',build,'--target','ssl','crypto','-j6']);
  const crypto=path.join(output,'libcrypto.dylib'),ssl=path.join(output,'libssl.dylib');
  const helper=binder?path.join(binder.output,'source/nativehelper'):path.join(source,'nativehelper');
  if(!binder)exportSources(nativehelperSource,pins.aospLibnativehelper,['header_only_include','NOTICE'],helper);
  const includes=[path.join(conscrypt,'common/src/jni/main/include'),path.join(conscrypt,'common/src/jni/unbundled/include'),path.join(boringssl,'src/include'),path.join(jdk,'include'),path.join(jdk,'include/darwin'),path.join(helper,'header_only_include')];
  const sources=publisherList(conscrypt,'conscrypt_global','srcs').map(file=>path.join(conscrypt,file));
  const library=path.join(output,'libjavacrypto.dylib');
  run('clang++',[...portable,'-std=c++17','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-fvisibility=hidden','-Wall','-Wextra','-Werror','-DCONSCRYPT_OPENJDK','-DCONSCRYPT_UNBUNDLED','-DBORINGSSL_SHARED_LIBRARY','-DJNI_JARJAR_PREFIX=com/android/',...includes.map(file=>'-I'+file),'-dynamiclib','-Wl,-install_name,@loader_path/libjavacrypto.dylib',...sources,ssl,crypto,'-o',library]);
  for(const file of[crypto,ssl,library])run('codesign',['--force','--sign','-',file]);
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  copyOptional(path.join(conscrypt,'LICENSE'),path.join(notices,'conscrypt.txt'));
  copyOptional(path.join(boringssl,'NOTICE'),path.join(notices,'boringssl.txt'));
  copyOptional(path.join(conscrypt,'NOTICE'),path.join(notices,'conscrypt-NOTICE.txt'));
  copyOptional(path.join(boringssl,'src/LICENSE'),path.join(notices,'boringssl-LICENSE.txt'));
  return {library,libraries:[crypto,ssl,library],boringssl:{crypto,ssl,include:path.join(boringssl,'src/include'),revision:pins.aospBoringSSL},certificates:path.join(conscrypt,'apex/ca-certificates/files')};
}
