const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),{spawnSync}=require('node:child_process');

test('ART VDSO exports original arm64 Linux symbol versions in ELF metadata',async t=>{
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-art-vdso-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const target=path.join(root,'crates/aim-linux-abi/src');fs.mkdirSync(target,{recursive:true});
 const original=path.resolve(process.env.ANDROID_AIM_SOURCE||'src/external/aim-custom','crates/aim-linux-abi/src/vdso.rs');
 fs.copyFileSync(original,path.join(target,'vdso.rs'));
 const {prepareVdsoVersions}=await import('../src/modules/android/art/vdso-source.mjs');prepareVdsoVersions(root);
 const source=fs.readFileSync(path.join(target,'vdso.rs'),'utf8');
 // Exercise the original producer's complete ELF builder. The fixture supplies
 // bounded native entry points; no guest execution or clock emulation is needed.
 const start=source.indexOf('const SONAME:'),end=source.indexOf('unsafe extern "C" {\n    fn sys_icache_invalidate',start);
 assert.ok(start>0&&end>start,'original VDSO builder boundaries');
 const builder=path.join(root,'builder.rs');fs.writeFileSync(builder,source.slice(start,end));
 const fixture=path.resolve('tests/fixtures/android-native/art-vdso-versions.rs'),probe=path.join(root,'probe');
 const run=(program,args,env=process.env)=>{const r=spawnSync(program,args,{env,encoding:'utf8',timeout:30000});assert.ifError(r.error);assert.equal(r.status,0,r.stdout+r.stderr+String(r.signal||''));return r;};
 run('rustc',['--edition=2024','--test',fixture,'-o',probe],{...process.env,VDSO_BUILD_SOURCE:builder});
 const result=run(probe,['--nocapture']);assert.match(result.stdout,/1 passed; 0 failed/);t.diagnostic(result.stdout.trim());
});
