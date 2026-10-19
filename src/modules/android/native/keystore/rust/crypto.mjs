import fs from 'node:fs';
import {nativePathFlags} from '../../upstream/native-paths.mjs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {bindgenMethods,manifest,properties,write} from './blueprint.mjs';
import {prepareSourceTree,copyChanged} from '../../upstream/source-files.mjs';

const here=path.dirname(fileURLToPath(import.meta.url));

export function prepareCrypto({modules,sources,workspace,foundation,runCargo,versions}) {
  const boring=path.join(sources.boringssl,'src/rust'),security=path.join(sources.security,'keystore2/src/crypto');
  const generated=path.join(workspace,'generated-crypto'),native=path.join(workspace,'native-crypto'),tool=path.join(workspace,'bindgen-tool');
  const include=foundation.crypto.include;
  fs.mkdirSync(native,{recursive:true});
  const p=properties(modules,'libkeystore2_crypto_bindgen');
  const sdk=execFileSync('xcrun',['--show-sdk-path'],{encoding:'utf8'}).trim();
  const cflags=['-I'+include,'--target=arm64-apple-macos13.0','-isysroot',sdk,'-DBORINGSSL_SHARED_LIBRARY'];
  prepareSourceTree(tool,temporary=>{
  write(path.join(temporary,'Cargo.toml'),'[package]\nname="keystore-bindgen"\nversion="0.1.0"\nedition="2021"\n[workspace]\n[dependencies]\nbindgen={version="='+versions.bindgen.version+'",features=["experimental"]}\n');
  write(path.join(temporary,'src/main.rs'),`fn main(){
    let output=std::path::PathBuf::from(std::env::args_os().nth(1).expect("binding output directory"));
    let base=bindgen::Builder::default().clang_args(${JSON.stringify(cflags)});
    base.clone().header(${JSON.stringify(path.join(boring,'bssl-sys/wrapper.h'))})
      .derive_default(false).enable_function_attribute_detection().use_core()
      .default_macro_constant_type(bindgen::MacroTypeVariation::Signed)
      .rustified_enum("point_conversion_form_t")
      .allowlist_file(r".*[[:punct:]]include[[:punct:]]openssl[[:punct:]].*\\.h")
      .allowlist_file(r".*[[:punct:]]rust_wrapper\\.h")
      .wrap_static_fns(true).wrap_static_fns_path(output.join("bssl_wrapper.c"))
      .generate().unwrap().write_to_file(output.join("bssl_bindings.rs")).unwrap();
    base.header(${JSON.stringify(path.join(security,p.wrapper_src))})
      .clang_args(["-DBORINGSSL_NO_CXX","-x","c++","-std=c++17"])
      ${bindgenMethods(p.bindgen_flags||[])}.generate().unwrap()
      .write_to_file(output.join("keystore2_bindings.rs")).unwrap();
  }\n`);
  copyChanged(path.join(here,'bindgen-Cargo.lock'),path.join(temporary,'Cargo.lock'));
  });
  prepareSourceTree(generated,temporary=>runCargo(['run','--release','--locked','--manifest-path',path.join(tool,'Cargo.toml'),'--',temporary]));
  const flags=[...nativePathFlags({build:workspace,roots:Object.entries(sources).map(([name,directory])=>[directory,'/src/'+name])}),'-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-DBORINGSSL_SHARED_LIBRARY','-I'+include];
  const objects=path.join(native,'objects');fs.mkdirSync(objects,{recursive:true});
  const wrapper=path.join(objects,'bssl_wrapper.o');
  execFileSync('clang',[...flags,'-std=c11','-c',path.join(generated,'bssl_wrapper.c'),'-o',wrapper],{stdio:'inherit'});
  const units=properties(modules,'libkeystore2_crypto').srcs.map(src=>({file:path.join(security,src),object:path.join(objects,src.replaceAll('/','_')+'.o')}));
  for(const unit of units)execFileSync('clang++',[...flags,'-std=c++17','-I'+security+'/include','-I'+sources.logging+'/liblog/include','-c',unit.file,'-o',unit.object],{stdio:'inherit'});
  const archives=[path.join(native,'libbssl_sys_raw_staticfns.a'),path.join(native,'libkeystore2_crypto.a')];
  const assembly=fs.mkdtempSync(path.join(native,'.archives-'));
  try{for(const [index,inputs]of[[wrapper],units.map(u=>u.object)].entries()){
    const archive=path.join(assembly,path.basename(archives[index]));execFileSync('ar',['rcs',archive,...inputs]);copyChanged(archive,archives[index]);
  }}finally{fs.rmSync(assembly,{recursive:true,force:true});}
  const sys=path.join(workspace,'platform/bssl-sys'),crypto=path.join(workspace,'platform/bssl-crypto');
  prepareSourceTree(sys,temporary=>{
  fs.cpSync(path.join(boring,'bssl-sys/src'),path.join(temporary,'src'),{recursive:true});
  write(path.join(temporary,'Cargo.toml'),manifest({name:'bssl-sys',entry:'src/lib.rs'}));
  // The final native owner links the wrapper archives and shared crypto once.
  write(path.join(temporary,'build.rs'),`fn main(){println!("cargo:rerun-if-changed={}",${JSON.stringify(path.join(generated,'bssl_bindings.rs'))});std::fs::copy(${JSON.stringify(path.join(generated,'bssl_bindings.rs'))},std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("bindgen.rs")).unwrap();println!("cargo:rustc-check-cfg=cfg(bindgen_rs_file)");println!("cargo:rustc-check-cfg=cfg(unsupported_inline_wrappers)");}\n`);
  });
  prepareSourceTree(crypto,temporary=>fs.cpSync(path.join(boring,'bssl-crypto'),temporary,{recursive:true}));
  const bindgen=path.join(workspace,'platform/keystore2_crypto_bindgen');
  prepareSourceTree(bindgen,temporary=>{
  write(path.join(temporary,'lib.rs'),fs.readFileSync(path.join(generated,'keystore2_bindings.rs')));
  write(path.join(temporary,'Cargo.toml'),manifest({name:'keystore2_crypto_bindgen'}));
  });
  return {archives,directories:[sys,crypto,bindgen],providers:new Map([
    ['libbssl_crypto',{name:'bssl-crypto',directory:crypto,features:['std']}],
    ['libkeystore2_crypto_bindgen',{name:'keystore2_crypto_bindgen',directory:bindgen}]
  ])};
}
