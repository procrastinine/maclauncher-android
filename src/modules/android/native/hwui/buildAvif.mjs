import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {parseBlueprint} from '../upstream/blueprint.mjs';

export async function buildAvif(ctx) {
const output=path.join(ctx.work,'module/components/crabbyavif'),repo=ctx.work,source=path.join(ctx.work,'module/source');fs.mkdirSync(output,{recursive:true});
const options={...ctx.options,ninja:ctx.tools.ninja,'blueprint-parser':ctx.tools.blueprintParser};
process.env.ANDROID_BLUEPRINT_PARSER=options['blueprint-parser'];
const bin=path.dirname(ctx.tools.clangxx),ndk=path.dirname(bin),sysroot=ctx.tools.sysroot||path.join(ndk,'sysroot');
const workspace=path.join(output,'rust');
const crabby=path.join(source,'crabbyavif'),yuv=path.join(source,'libyuv'),rustutils=options['rustutils-source'];
const write=(file,text)=>{fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,text);};
const json=JSON.stringify;
const cflags=['--target=aarch64-linux-android35','--sysroot='+sysroot,'-I'+path.join(sysroot,'usr/include/c++/v1'),'-I'+path.join(source,'av/media/ndk/include'),'-I'+path.join(source,'core/libcutils/include'),'-I'+path.join(source,'core/include'),'-I'+path.join(source,'native/headers/media_plugin'),'-I'+path.join(yuv,'include')];
const bp=parseBlueprint(fs.readFileSync(path.join(yuv,'Android.bp'),'utf8'),{file:path.join(yuv,'Android.bp')}).modules.find(m=>m.properties.name==='libyuv').properties;
const objects=bp.srcs.map(file=>({source:path.join(yuv,file),object:path.join(output,'objects',file.replaceAll('/','_')+'.o')}));
fs.mkdirSync(path.join(output,'objects'),{recursive:true});
const quote=value=>'"'+value.replaceAll('"','\\"')+'"';
const flags=['-O2','-fPIC','-std=c++17','-fno-rtti','-fno-exceptions','-fvisibility-inlines-hidden','-DNDEBUG','-UDEBUG','-ffp-contract=off','-fno-strict-aliasing','-ffunction-sections','-fdata-sections','-fstack-protector-strong','-D_FORTIFY_SOURCE=2','-ffile-prefix-map='+source+'=/src/aosp',...cflags,...bp.cflags,'-I'+path.join(source,'jpeg')];
write(path.join(output,'build.ninja'),`rule cxx\n  command = ${quote(path.join(bin,'aarch64-linux-android35-clang++'))} ${flags.map(quote).join(' ')} -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\nrule archive\n  command = ${quote(path.join(bin,'llvm-ar'))} rcs $out $in\n${objects.map(v=>`build ${v.object}: cxx ${v.source}`).join('\n')}\nbuild ${path.join(output,'libyuv.a')}: archive ${objects.map(v=>v.object).join(' ')}\ndefault ${path.join(output,'libyuv.a')}\n`);
const manifest=(name,entry,dependencies,extra='')=>`[package]\nname=${json(name)}\nversion="0.1.0"\nedition="2021"\n[lib]\npath=${json(entry)}\n${extra}\n[dependencies]\n${dependencies}\n`;
const crate=(name,entry,dependencies,extra='',build)=>{
 const directory=path.join(workspace,name);write(path.join(directory,'Cargo.toml'),manifest(name,entry,dependencies,extra)+(build?'\n[build-dependencies]\nbindgen="=0.69.5"\n':''));if(build)write(path.join(directory,'build.rs'),build);return directory;
};
const bindgen=(header,file,methods='',args=[])=>`fn main(){bindgen::Builder::default().header(${json(header)}).clang_args(${json([...cflags,...args])}).layout_tests(false).generate_comments(false).wrap_unsafe_ops(true)${methods}.generate().expect("original Android header bindings").write_to_file(std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap()).join(${json(file)})).unwrap();}\n`;
crate('ndk_sys',path.join(crabby,'sys/ndk-sys/src/lib.rs'),'','',bindgen(path.join(crabby,'sys/ndk-sys/wrapper.h'),'ndk_media_bindgen.rs','.derive_default(true)'));
crate('libyuv_sys',path.join(crabby,'sys/libyuv-sys/src/lib.rs'),'','',bindgen(path.join(crabby,'sys/libyuv-sys/wrapper.h'),'libyuv_bindgen.rs'));
const bindingCrate=(name,header,methods,deps='')=>{
 const directory=path.join(workspace,name);crate(name,path.join(directory,'lib.rs'),deps,'',bindgen(header,'bindings.rs',methods));write(path.join(directory,'lib.rs'),'#![allow(non_camel_case_types,non_snake_case,non_upper_case_globals)]\ninclude!(concat!(env!("OUT_DIR"),"/bindings.rs"));\n');
};
bindingCrate('system_properties_bindgen',path.join(rustutils,'bindgen/system_properties.h'),'.allowlist_function("__system_property_find").allowlist_function("__system_property_foreach").allowlist_function("__system_property_read_callback").allowlist_function("__system_property_set").allowlist_function("__system_property_wait").blocklist_type("timespec").raw_line("use libc::timespec;")','libc="=0.2.161"');
bindingCrate('cutils_bindgen',path.join(rustutils,'bindgen/cutils.h'),'.allowlist_function("android_get_control_socket").allowlist_var("AID_KEYSTORE").allowlist_var("AID_ROOT").allowlist_var("AID_SHELL").allowlist_var("AID_SYSTEM").allowlist_var("AID_USER_OFFSET")');
crate('rustutils',path.join(rustutils,'lib.rs'),`anyhow="=1.0.97"\nlibc={version="=0.2.161",features=["extra_traits"]}\nnix={version="=0.29.0",features=["event","feature","fs","inotify","ioctl","memoffset","mman","mount","net","poll","process","sched","signal","socket","term","time","uio","user"]}\nthiserror="=2.0.11"\ncutils_bindgen={path="../cutils_bindgen"}\nsystem_properties_bindgen={path="../system_properties_bindgen"}`);
crate('logger',path.join(source,'logging/rust/logger.rs'),'log="=0.4.26"\nenv_filter="=0.1.4"\nenv_logger="=0.10.2"\nandroid_logger="=0.15.0"');
crate('crabbyavif',path.join(crabby,'src/lib.rs'),'ndk_sys={path="../ndk_sys"}\nlibyuv_sys={path="../libyuv_sys"}\nlog="=0.4.26"\nlogger={path="../logger"}\nrustutils={path="../rustutils"}',`crate-type=["staticlib","rlib"]\n[features]\ndefault=[]\nandroid_mediacodec=[]\ncapi=[]\nheic=[]\nlibyuv=[]`,bindgen(path.join(crabby,'sys/ndk-sys/mediaimage2_wrapper.hpp'),'mediaimage2_bindgen.rs','.allowlist_item("android::MediaImage2?").allowlist_recursively(false)',['-x','c++','-std=c++17']));
write(path.join(workspace,'Cargo.toml'),'[workspace]\nmembers=["crabbyavif","ndk_sys","libyuv_sys","logger","rustutils","cutils_bindgen","system_properties_bindgen"]\nresolver="2"\n[profile.release]\nopt-level=3\ndebug=1\npanic="abort"\n');
const env={...process.env,CARGO_HOME:ctx.tools.cargoHome,RUSTC:ctx.tools.rustc,CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER:path.join(bin,'aarch64-linux-android35-clang'),LIBCLANG_PATH:ctx.tools.libclang,RUSTFLAGS:'--cfg android_soong -C panic=abort -C relocation-model=pic --remap-path-prefix='+repo+'=/src/maclauncher --remap-path-prefix='+source+'=/src/aosp --remap-path-prefix='+output+'=/src/android-hwui/avif',CARGO_TARGET_DIR:path.join(output,'cargo-target')};
fs.copyFileSync(new URL('./Cargo.lock',import.meta.url),path.join(workspace,'Cargo.lock'));
const startedAt=new Date().toISOString();
execFileSync(options.ninja,['-f',path.join(output,'build.ninja'),'-j','4'],{cwd:output,stdio:'inherit'});
execFileSync(ctx.tools.cargo,['build','--offline','--locked','--release','--target','aarch64-linux-android','--manifest-path',path.join(workspace,'Cargo.toml'),'-p','crabbyavif','--features','android_mediacodec,capi,heic,libyuv','-j','3'],{cwd:output,env,stdio:'inherit'});
fs.writeFileSync(path.join(output,'build-result.json'),json({startedAt,finishedAt:new Date().toISOString(),target:'aarch64-linux-android',features:['android_mediacodec','capi','heic','libyuv'],cfg:['android_soong'],archive:path.join(output,'cargo-target/aarch64-linux-android/release/libcrabbyavif.a'),libyuv:path.join(output,'libyuv.a'),sources:{crabby,yuv,rustutils,logging:path.join(source,'logging')},originalLibyuvUnits:objects.length},null,2)+'\n');

}
