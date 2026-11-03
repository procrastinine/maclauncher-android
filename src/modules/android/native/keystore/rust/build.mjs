import {copyOptional,prepareSourceTree,copyChanged} from '../../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import {parseBlueprint} from '../../upstream/blueprint.mjs';
import {adaptSoftwareProvider} from '../software-provider.mjs';
import {readModules,properties,write,replace,manifest,bindgenMethods} from './blueprint.mjs';
import {prepareAidl} from './aidl.mjs';
import {prepareFlags} from './flags.mjs';
import {prepareCrypto} from './crypto.mjs';
import {nativeRustPathEnvironment} from '../../upstream/native-paths.mjs';

const here=path.dirname(fileURLToPath(import.meta.url));
const version=text=>text.match(/^version\s*=\s*"([^"]+)"/m)[1];

function crateMetadata(sources) {
  const versions={},providers=new Map();
  const dirs=fs.readdirSync(path.join(sources['rust-crates'],'crates')).map(d=>path.join(sources['rust-crates'],'crates',d));
  dirs.push(sources['rust-cxx'],path.join(sources['rust-cxx'],'gen/build'));
  for(const directory of dirs) {
    const file=path.join(directory,'Cargo.toml');if(!fs.existsSync(file))continue;
    const text=fs.readFileSync(file,'utf8'),name=text.match(/^name\s*=\s*"([^"]+)"/m)?.[1];
    if(!name)continue;
    const bp=path.join(directory,'Android.bp');
    const modules=fs.existsSync(bp)?parseBlueprint(fs.readFileSync(bp,'utf8'),{file:bp}).modules:[];
    const module=modules.find(m=>['rust_library','rust_library_rlib','rust_proc_macro'].includes(m.type)&&m.properties.crate_name===name.replaceAll('-','_'));
    versions[name]={version:version(text),features:module?.properties.features||[]};
    for(const m of modules)if(['rust_library','rust_library_rlib','rust_proc_macro'].includes(m.type)&&m.properties.crate_name===name.replaceAll('-','_'))
      providers.set(m.properties.name,{name,version:version(text),features:m.properties.features||[]});
  }
  return {versions,providers};
}

/** Prepare complete upstream service sources with only native host adaptations. */
export function prepareKeyStoreRust(context) {
  const {output,sources,foundation,binder,keyMintAidl,nativeHal,aconfig,rustToolchain,python}=context;
  const workspace=path.join(output,'rust');fs.mkdirSync(workspace,{recursive:true});
  // Each complete adapted crate is published after preparation. Cargo keeps
  // unchanged inputs and its separate artifacts across native-hook rebuilds.
  const env={...process.env,PATH:path.join(rustToolchain,'bin')+path.delimiter+process.env.PATH,
    RUSTC:path.join(rustToolchain,'bin/rustc'),MACOSX_DEPLOYMENT_TARGET:'13.0',
    CARGO_TARGET_DIR:path.join(output,'cargo-target')};
  Object.assign(env,nativeRustPathEnvironment({source:here,build:output,
    roots:[[process.env.CARGO_HOME||path.join(os.homedir(),'.cargo'),'/cargo'],[rustToolchain,'/toolchain/rust']]},env));
  const runCargo=(args,options={})=>execFileSync(path.join(rustToolchain,'bin/cargo'),args,{env,stdio:'inherit',...options});
  const roots=[sources.security,sources.rustutils,path.join(sources.selinux,'libselinux'),sources.logging,
    path.join(sources['hardware-interfaces'],'security'),path.join(sources['system-hardware'],'keystore2'),
    path.join(sources.apex,'apexd'),path.join(sources.framework,'core/java')];
  const modules=readModules(roots);
  const flagFile=path.join(sources.framework,'AconfigFlags.bp');
  for(const module of parseBlueprint(fs.readFileSync(flagFile,'utf8'),{file:flagFile}).modules)
    if(module.properties.name)modules.set(module.properties.name,{...module,file:flagFile,directory:sources.framework});
  const {versions,providers:external}=crateMetadata(sources);
  const externalLine=(name,extra=[])=>`${name}={version="=${versions[name].version}",features=${JSON.stringify([...new Set([...versions[name].features,...extra])])}}`;
  const nativeBinder=path.join(sources.native,'libs/binder'),binderDir=path.join(workspace,'binder');
  prepareSourceTree(binderDir,directory=>{
  fs.cpSync(path.join(nativeBinder,'rust'),directory,{recursive:true});
  write(path.join(directory,'Cargo.toml'),manifest({name:'binder',entry:'src/lib.rs',dependencies:[
    'binder_ndk_sys={path="sys"}',externalLine('downcast-rs'),externalLine('libc',['extra_traits']),externalLine('log'),externalLine('zerocopy',['alloc'])]}));
  write(path.join(directory,'build.rs'),'fn main(){println!("cargo:rustc-check-cfg=cfg(android_vendor,android_ndk,android_vndk,trusty)");}\n');
  write(path.join(directory,'sys/Cargo.toml'),manifest({name:'binder_ndk_sys',dependencies:[externalLine('libc')],buildDependencies:[externalLine('bindgen')]}));
  const sysBuild=path.join(directory,'sys/build.rs');
  let sys=fs.readFileSync(sysBuild,'utf8'),first=sys.indexOf('    let ndk_home ='),end=sys.indexOf('    let bindings =',first);
  if(first<0||end<0)throw Error('Original Binder toolchain entry changed');
  sys=sys.slice(0,first)+sys.slice(end);write(sysBuild,sys);
  replace(sysBuild,'.clang_arg(format!("--sysroot={}", sysroot.display()))',
    `.clang_args(${JSON.stringify(['-DANDROID_PLATFORM','-std=c++20',...['include_cpp','include_ndk','include_platform'].map(d=>'-I'+nativeBinder+'/ndk/'+d)])})\n.blocklist_type("sockaddr").raw_line("use libc::sockaddr;")`);
  replace(sysBuild,'rustc-link-lib=binder_ndk','rustc-link-lib=android-binder-rpc');
  });
  const aidl=prepareAidl({modules,keyMintAidl,compiler:keyMintAidl.compiler,python,sources,workspace,asyncTraitVersion:versions['async-trait'].version});
  const flags=prepareFlags({modules,sources,workspace,aconfig,release:context.release||'bp2a',logVersion:versions.log.version});
  const crypto=prepareCrypto({modules,sources,workspace,foundation,runCargo,versions});
  const prepared=new Map([['libbinder_rs',{name:'binder',directory:binderDir}],...crypto.providers]);
  const dependencies=(p,directory)=>[...new Set(p.rustlibs||[])].map(name=>{
    const provider=external.get(name);if(provider)return `${provider.name}={version="=${provider.version}",features=${JSON.stringify(provider.features)}}`;
    const supplied=prepare(name);
    return `${supplied.name}={path=${JSON.stringify(path.relative(directory,supplied.directory))}${supplied.features?.length?',features='+JSON.stringify(supplied.features):''}}`;
  });
  function prepare(name) {
    if(prepared.has(name))return prepared.get(name);
    if(/-rust$/.test(name)&&modules.get(name.replace(/(?:-V\d+)?-rust$/,''))?.type==='aidl_interface')return aidl.generate(name);
    let module=modules.get(name);if(!module)throw Error('Missing upstream Rust dependency declaration: '+name);
    let p=properties(modules,name),generatedSource;
    // Soong sometimes wraps a generated rlib in rust_library for dylib output.
    // Cargo compiles that generated source directly into the same original crate.
    if(p.srcs?.length===1&&p.srcs[0].startsWith(':')) {
      generatedSource=p.srcs[0].slice(1);
      const generator=modules.get(generatedSource);
      if(generator?.type==='rust_bindgen') {
        const source=properties(modules,generatedSource);
        p={...source,...p,rustlibs:[...(source.rustlibs||[]),...(p.rustlibs||[])]};module=generator;
      }
    }
    if(module.type==='rust_aconfig_library') {const result=flags.generate(module);prepared.set(name,result);return result;}
    const crate=p.crate_name,directory=path.join(workspace,'platform',crate),result={name:crate,directory};
    prepared.set(name,result);
    if(generatedSource)prepared.set(generatedSource,result);
    prepareSourceTree(directory,source=>{
    if(module.type==='rust_bindgen') {
      const cflags=['--target=arm64-apple-macos13.0','-I'+module.directory,...(p.local_include_dirs||[]).map(d=>'-I'+path.join(module.directory,d))];
      if(module.directory===sources.rustutils)cflags.push('-I'+sources.core+'/libcutils/include','-I'+sources.core+'/include','-I'+foundation.property.include);
      if(module.directory===path.join(sources.logging,'liblog'))cflags.push('-I'+sources.logging+'/liblog/include');
      if(/\.hpp$/.test(p.wrapper_src))cflags.push('-x','c++','-std=c++17');
      write(path.join(source,'lib.rs'),'#![allow(non_camel_case_types,non_snake_case,non_upper_case_globals)]\ninclude!(concat!(env!("OUT_DIR"), "/bindings.rs"));\n');
      write(path.join(source,'build.rs'),`fn main(){bindgen::Builder::default().header(${JSON.stringify(path.join(module.directory,p.wrapper_src))}).clang_args(${JSON.stringify(cflags)})${bindgenMethods(p.bindgen_flags||[])}.generate().unwrap().write_to_file(std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("bindings.rs")).unwrap();}\n`);
      write(path.join(source,'Cargo.toml'),manifest({name:crate,dependencies:[externalLine('libc')],buildDependencies:[externalLine('bindgen')]}));
    } else {
      fs.cpSync(module.directory,source,{recursive:true});
      const entry=p.srcs?.[0];if(!entry||entry.startsWith(':'))throw Error('Unsupported original Rust source entry: '+name);
      write(path.join(source,'Cargo.toml'),manifest({name:crate,entry,features:p.features||[],dependencies:dependencies(p,directory),
        buildDependencies:crate==='keystore2_hal_names'?[externalLine('cxx-build')]:[]}));
      if(crate==='rustutils') {
        const file=path.join(source,entry);write(file,fs.readFileSync(file,'utf8').replaceAll('#[cfg(target_os = "android")]','#[cfg(any(target_os = "android", target_os = "macos"))]'));
      }
      if(crate==='keystore2_hal_names')write(path.join(source,'build.rs'),`fn main(){cxx_build::bridge(${JSON.stringify(entry)}).file(${JSON.stringify(path.join(nativeHal.source,'hal_names.cpp'))}).include(".").flag_if_supported("-std=c++17").flag_if_supported("-mmacosx-version-min=13.0").compile("keystore2_hal_names_cpp");}\n`);
      if(crate==='keystore2') {
        const src=path.join(source,'src');adaptSoftwareProvider(src);
        replace(path.join(src,'utils.rs'),'unsafe { libc::clock_gettime(libc::CLOCK_BOOTTIME, &mut current_time) };',
          'extern "C" { fn klb_clock_gettime(clock: libc::c_int, time: *mut libc::timespec) -> libc::c_int; }\n    unsafe { klb_clock_gettime(7, &mut current_time) };');
        const early=fs.readFileSync(path.join(sources.release,'flag_values',flags.release,'RELEASE_AVF_ENABLE_EARLY_VM.textproto'),'utf8');
        write(path.join(source,'build.rs'),`fn main(){println!("cargo:rustc-check-cfg=cfg(early_vm)");${/bool_value:\s*true/.test(early)?'println!("cargo:rustc-cfg=early_vm");':''}}\n`);
      }
    }
    });
    return result;
  }
  const service=properties(modules,'keystore2'),bootstrap=path.join(workspace,'platform/keystore_bootstrap');
  const entry=path.join(sources.security,'keystore2',service.srcs[0]);
  prepareSourceTree(bootstrap,directory=>{
  write(path.join(directory,'lib.rs'),fs.readFileSync(entry));
  const file=path.join(directory,'lib.rs');
  replace(file,'fn main() {','fn start(directory: &Path) {');
  replace(file,'    let mut args = std::env::args();\n    args.next().expect("That\'s odd. How is there not even a first argument?");','');
  replace(file,'let id_rotation_state = if let Some(dir) = args.next() {\n        let db_path = Path::new(&dir);','let id_rotation_state = {\n        let db_path = directory;');
  replace(file,'    } else {\n        panic!("Must specify a database directory.");\n    };','    };');
  replace(file,'    binder::ProcessState::start_thread_pool();','    // The original RpcServer owns platform connection workers.');
  replace(file,'    binder::ProcessState::join_thread_pool();','    // Return to the owning platform process Looper.');
  fs.appendFileSync(file,`\n#[no_mangle]\npub unsafe extern "C" fn kl_keystore_start(directory:*const std::ffi::c_char)->i32 {\n    if directory.is_null(){return -1;}\n    let path=std::ffi::CStr::from_ptr(directory);\n    use std::os::unix::ffi::OsStrExt;\n    let directory=Path::new(std::ffi::OsStr::from_bytes(path.to_bytes()));\n    match std::panic::catch_unwind(||start(directory)){Ok(())=>0,Err(_)=>-1}\n}\n`);
  write(path.join(directory,'Cargo.toml'),manifest({name:'keystore_bootstrap',kind:'staticlib',dependencies:dependencies(service,bootstrap)}));
  });
  const retained=new Set([bootstrap,...[...prepared.values(),...aidl.generated.values()].map(value=>value.directory),...flags.directories,...crypto.directories,
    ...[...aidl.records.values()].filter(record=>aidl.generated.has(record.name)).map(record=>record.generated)]);
  // The resolved original dependency closure owns the complete source set.
  // Retire whole crates/generators no longer reached by upstream declarations.
  for(const parent of ['platform','aidl','generated-aidl']){
    const directory=path.join(workspace,parent);if(!fs.existsSync(directory))continue;
    for(const name of fs.readdirSync(directory))if(!retained.has(path.join(directory,name)))fs.rmSync(path.join(directory,name),{recursive:true,force:true});
  }
  write(path.join(workspace,'Cargo.toml'),'[workspace]\nmembers=["binder","binder/sys","aidl/*","platform/*"]\nexclude=["bindgen-tool"]\nresolver="2"\n[profile.release]\nopt-level=3\ndebug=1\n');
  copyChanged(path.join(here,'Cargo.lock'),path.join(workspace,'Cargo.lock'));
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  // The source exporter owns notices for AOSP inputs. Cargo's lock identifies
  // the exact registry closure; package source licenses accompany distribution.
  return {workspace,env,runCargo,crypto:{archives:crypto.archives},notices,aidl:{records:[...aidl.records.values()].filter(r=>aidl.generated.has(r.name))},
    rustLibrary:path.join(output,'cargo-target/release/libkeystore_bootstrap.a')};
}

export function buildKeyStoreRust(context) {
  const result=prepareKeyStoreRust(context);
  result.runCargo(['build','--release','--locked','--manifest-path',path.join(result.workspace,'Cargo.toml'),'-p','keystore_bootstrap']);
  const metadata=JSON.parse(result.runCargo(['metadata','--locked','--format-version=1','--manifest-path',path.join(result.workspace,'Cargo.toml')],{encoding:'utf8',stdio:'pipe'}));
  const crates=[];
  for(const pkg of metadata.packages.filter(p=>p.source?.startsWith('registry+'))) {
    const root=path.dirname(pkg.manifest_path),destination=path.join(result.notices,'crates',pkg.name+'-'+pkg.version);
    const files=new Set(fs.globSync(['LICENSE*','LICENCE*','COPYING*','NOTICE*'],{cwd:root}));
    if(pkg.license_file)files.add(pkg.license_file);
    for(const relative of files)copyOptional(path.join(root,relative),path.join(destination,relative));
    crates.push({name:pkg.name,version:pkg.version,source:pkg.source,license:pkg.license});
  }
  return {rustLibrary:result.rustLibrary,crypto:result.crypto,notices:result.notices,aidl:result.aidl};
}
