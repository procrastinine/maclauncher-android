const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os'),{spawnSync}=require('node:child_process');
test('original AOT source identity restores real mapped file words and fork metadata',{timeout:120000},async t=>{
 const source=process.env.ANDROID_AIM_SOURCE,rustc=process.env.ANDROID_RUSTC,cargo=process.env.ANDROID_CARGO;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!source||!rustc||!cargo)return t.skip('Requires original AIM source and native Rust compiler');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-original-files-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const prepared=path.join(temporary,'source/crates/aim-linux-abi/src');fs.mkdirSync(path.join(prepared,'sys'),{recursive:true});
 for(const file of ['xrt.rs','loader.rs','sys/copies.rs','sys/mod.rs'])fs.copyFileSync(path.join(source,'crates/aim-linux-abi/src',file),path.join(prepared,file));
 const {prepareOriginalFiles}=await import('../src/modules/android/art/original-file-source.mjs');prepareOriginalFiles(path.join(temporary,'source'));
 assert.match(fs.readFileSync(path.join(prepared,'loader.rs'),'utf8'),/note_mapped\(&mapped, name, Path::new\(path\), &original_stat\)/);
 const slice=(text,start,end)=>{const at=text.indexOf(start),last=text.indexOf(end,at);assert.ok(at>=0&&last>at,'Original source boundary');return text.slice(at,last);};
 // Keep the original FileStat, actual Darwin VM walker and original copy map.
 // Only the unrelated fork FD-transfer owner is excluded from this metadata
 // codec fixture; its integer/path/sequence implementation is copied unchanged.
 const cache=fs.readFileSync(path.join(source,'crates/aim-linux-abi/src/cache.rs'),'utf8');
 const stat=slice(cache,'#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]\npub struct FileStat','#[derive(Clone, Debug, PartialEq, Eq)]\npub enum EntryKind');
 const codec=fs.readFileSync(path.join(source,'crates/aim-linux-abi/src/sys/fork/state.rs'),'utf8');
 let writer=slice(codec,'    pub fn into_bytes(self)','/// Reads what');writer=writer.replace('self.0','self.0');
 let reader=slice(codec,'pub struct Reader<','/// Every module\'s state');reader=reader.replace('    owner_error: Option<crate::errno::Errno>,\n','').replace(', owner_error: None','').replace('    pub fn owner_error(&self) -> Option<crate::errno::Errno> { self.owner_error }\n','');
 fs.writeFileSync(path.join(temporary,'codec.rs'),'use std::path::PathBuf;\n#[derive(Default)] pub struct Writer(Vec<u8>);\nimpl Writer {\n'+writer+'\n'+reader);
 const vmmap=path.join(source,'crates/aim-linux-abi/src/sys/vmmap.rs'),window=fs.readFileSync(path.join(source,'crates/aim-linux-abi/src/sys/window.rs'),'utf8').match(/^pub const TAG: u32 = \d+;/m)?.[0];assert.ok(window);
 const lib=`#![allow(dead_code)]\nmod sys {pub mod window {${window}}\n#[path=${JSON.stringify(vmmap)}] pub mod vmmap;\n#[path=${JSON.stringify(path.join(prepared,'sys/copies.rs'))}] pub mod copies;\n#[path=${JSON.stringify(path.join(temporary,'codec.rs'))}] pub mod fork_state;\npub use copies::note as note_copy;pub(crate) use copies::original_file_regions as original_file_copy_regions;pub(crate) use vmmap::regions as original_file_mapping_regions;pub(crate) use copies::forget as forget_original_file_test_copy;\n}\nmod xrt {use std::{ffi::CString,io,path::{Path,PathBuf},os::unix::ffi::OsStrExt};\n${stat}\nstruct Cache{dir:PathBuf}\nimpl Cache{${slice(cache,'    pub fn contains(&self, p: &Path) -> bool {','    /// The sha256 recorded')}}\nfn in_cache(path:&Path)->bool{Cache{dir:std::fs::canonicalize(std::env::temp_dir().join(format!(\"aim-original-files-{}\",std::process::id()))).unwrap()}.contains(path)}\n#[path=${JSON.stringify(path.join(prepared,'original-files.rs'))}] mod original_files;\n}\n`;
 fs.writeFileSync(path.join(temporary,'lib.rs'),lib);
 fs.writeFileSync(path.join(temporary,'Cargo.toml'),`[package]\nname="original-file-mapping-fixture"\nversion="0.1.0"\nedition="2024"\n[lib]\npath="lib.rs"\n[dependencies]\nlibc="=0.2.189"\naim-storage={path=${JSON.stringify(path.join(source,'crates/aim-storage'))}}\n`);
 const result=spawnSync(cargo,['test','--lib','--offline','--manifest-path',path.join(temporary,'Cargo.toml'),'--','--nocapture','--test-threads=1'],{encoding:'utf8',timeout:110000,maxBuffer:2<<20,env:{...process.env,RUSTC:rustc,RUSTDOC:path.join(path.dirname(rustc),'rustdoc'),CARGO_TARGET_DIR:process.env.ANDROID_ORIGINAL_FILES_TARGET||path.join(temporary,'target')}});
 assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);assert.match(result.stdout,/1 passed; 0 failed/);t.diagnostic(result.stdout.trim());
});
