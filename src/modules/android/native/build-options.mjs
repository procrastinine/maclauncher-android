import fs from 'node:fs';
import path from 'node:path';

export const requiredInputs=[
  'klepton',
  'converter',
  'jdk',
  'sdl',
  'vulkan-headers',
  'sndfile-headers',
  'codecs',
  'angle',
  'gles-headers',
  'framework',
  'output'
];
export const platformTools=['aidl','aconfig','rust-toolchain','python'];
export const optionalInputs=['ndk-headers','blueprint-parser','blueprint-parser-sources','ninja','build-work','tool-inputs','ndk','bionic-inputs','bionic-target'];

/** The snapshot builder and compiler consume the same pinned supplier catalog. */
export function sourceOptions(root){
  const suppliers={
    'zlib-source':'android-angle-deps/third_party-zlib','aim-source':'aim-custom','libvpx-source':'libvpx',
    'epoll-shim-source':'epoll-shim','inotify-source':'libinotify-kqueue','dynarmic-source':'dynarmic','framework-source':'aosp-framework','native-source':'aosp-native','libbase-source':'aosp-libbase',
    'fmt-source':'aosp-fmt','apache-http-source':'aosp-apache-http','statsd-source':'aosp-statsd','system-core-source':'aosp-system-core',
    'nativehelper-source':'aosp-libnativehelper','bionic-source':'aosp-bionic','compiler-rt-source':'aosp-compiler-rt','av-source':'aosp-av',
    'libcore-source':'aosp-xmlpull','art-source':'aosp-art','conscrypt-source':'aosp-conscrypt','boringssl-source':'aosp-boringssl','wilhelm-source':'aosp-wilhelm'
  };
  for(const supplier of JSON.parse(fs.readFileSync(path.join(root,'fonts/sources.json')))){
    const name=path.basename(supplier.path);suppliers[name.slice('aosp-'.length)+'-source']=name;
  }
  for(const supplier of JSON.parse(fs.readFileSync(path.join(root,'ndk-graphics/sources.json'))))
    suppliers[supplier.name+'-source']=supplier.checkout;
  for(const supplier of JSON.parse(fs.readFileSync(path.join(root,'keystore/sources.json'))))
    suppliers[supplier.name+'-source']=supplier.checkout;
  for(const supplier of Object.values(JSON.parse(fs.readFileSync(path.join(root,'bionic/linker-sources.json'))).sources))
    suppliers[supplier.id.slice('aosp-'.length)+'-source']=path.basename(supplier.path);
  return suppliers;
}

export function completeBuildOptions(root,options){
  for(const name of [...requiredInputs,...platformTools])if(!options[name])throw Error('Missing --'+name);
  const upstream=sourceOptions(root);
  for(const name of Object.keys(options))if(!requiredInputs.includes(name)&&!platformTools.includes(name)&&!optionalInputs.includes(name)&&!Object.hasOwn(upstream,name)&&name!=='angle-manifest')throw Error('Unknown build option: '+name);
  if(JSON.parse(fs.readFileSync(path.join(root,'source-pins.json'))).arm32Version===1&&!options['ndk-headers'])throw Error('Missing --ndk-headers for ARM32 native execution');
  for(const [option,checkout] of Object.entries(upstream))options[option]??=path.resolve(root,'../../../external',checkout);
  return options;
}
