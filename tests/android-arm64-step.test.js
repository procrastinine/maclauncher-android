const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {spawnSync}=require('node:child_process');
test('original A64 precise owner preserves state, generations and actual native atomics',{timeout:60000},t=>{
 const source=process.env.ANDROID_UNICORN_SOURCE,library=process.env.ANDROID_UNICORN_LIBRARY;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!source||!library)return t.skip('Requires the original pinned A64 component build');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-a64-step-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const binary=path.join(temporary,'probe'),native=path.resolve(__dirname,'../src/modules/android/native/arm64');
 const run=(program,args)=>{const r=spawnSync(program,args,{encoding:'utf8',timeout:30000,maxBuffer:4<<20});assert.ifError(r.error);assert.equal(r.signal,null,r.stdout+r.stderr);assert.equal(r.status,0,r.stdout+r.stderr);return r.stdout;};
 run('clang++',['-std=c++20','-O2','-Wall','-Wextra','-Werror','-I'+native,'-I'+path.resolve(source,'include'),path.join(native,'precise.cpp'),path.join(__dirname,'fixtures/android-native/arm64-precise-step.cpp'),path.resolve(library),'-Wl,-rpath,'+path.dirname(path.resolve(library)),'-o',binary]);
 const output=run(binary,[]);assert.ok(output.includes('PASS precise original A64 state, memory generations, source visibility, SIMD, atomics/native peers, exclusive boundaries, SVC and exceptions'));t.diagnostic(output.trim());
});

test('original-word publication refuses stale bytes without partial restoration',{timeout:30000},t=>{
 const compiler=spawnSync('rustc',['--version'],{encoding:'utf8'});if(compiler.error?.code==='ENOENT')return t.skip('Requires Rust');assert.equal(compiler.status,0,compiler.stderr);
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-original-code-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const owner=path.resolve(__dirname,'../src/modules/android/art/native/original-code.rs'),source=path.join(temporary,'owner.rs'),binary=path.join(temporary,'owner');
 fs.writeFileSync(source,`extern crate self as libc;
pub const ENOEXEC:i32=8;
mod sys {pub mod fork_state {
 pub struct Writer;impl Writer {pub fn u64(&mut self,_:u64){} pub fn seq<I:IntoIterator>(&mut self,values:I,mut f:impl FnMut(&mut Self,I::Item)){for v in values{f(self,v);}}}
 pub struct Reader;impl Reader {pub fn u64(&mut self)->u64{unreachable!()} pub fn seq<T>(&mut self,_:impl FnMut(&mut Self)->T)->Vec<T>{unreachable!()}}
}}
mod patch {
 #[derive(Clone,Copy)]pub enum Kind {Svc,MrsTp,MsrTp,ScsPush,ScsPop,MrsCtr,X18}
 pub mod a64 {pub const SVC0:u32=0xd4000001;pub const MRS_TPIDR_EL0:u32=0xd53bd040;pub const MSR_TPIDR_EL0:u32=0xd51bd040;pub const SCS_PUSH:u32=0xf800865e;pub const SCS_POP:u32=0xf85f8e5e;pub const MRS_CTR_EL0:u32=0xd53b0020;}
 mod original {include!(${JSON.stringify(owner)});}
}`);
 const build=spawnSync('rustc',['--test','--edition=2024','-Awarnings',source,'-o',binary],{encoding:'utf8',timeout:20000});assert.ifError(build.error);assert.equal(build.status,0,build.stdout+build.stderr);
 const run=spawnSync(binary,[],{encoding:'utf8',timeout:10000});assert.ifError(run.error);assert.equal(run.status,0,run.stdout+run.stderr);assert.match(run.stdout,/2 passed/);
});
