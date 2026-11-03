const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os');

test('HWUI copies accompanying candidates without checking licence bytes or filtering toolchain text',async t=>{
 const {collectHwuiNotices}=await import('../src/modules/android/native/hwui/notices.mjs');
 const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'hwui-candidates-'));t.after(()=>fs.rmSync(tmp,{recursive:true,force:true}));
 const write=(name,text)=>{const file=path.join(tmp,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,text);return file;};
 const unit='/* Copyright original owner. */\nint original(){return 1;}\n';write('source/LICENSE','Changed accompanying text\n');write('source/unit.cpp',unit);
 const manifest=write('cargo/Cargo.toml','[workspace]\n'),lock=write('cargo/Cargo.lock','version = 4\n');
 const dependency=write('registry/dependency/LICENSE','Dependency text\n'),source='registry+https://github.com/rust-lang/crates.io-index',id=source+'#dependency@1.0.0';
 write('cargo/Cargo.lock','version = 4\n[[package]]\nname = \"dependency\"\nversion = \"1.0.0\"\nsource = \"'+source+'\"\nchecksum = \"'+'a'.repeat(64)+'\"\n');
 const meta={workspace_members:['workspace'],resolve:{nodes:[{id:'workspace',deps:[{pkg:id,dep_kinds:[{kind:null}]}]},{id,deps:[]}]},packages:[{id,name:'dependency',version:'1.0.0',source,manifest_path:path.join(path.dirname(dependency),'Cargo.toml')}]};
 const cargo=write('cargo-tool','#!'+process.execPath+'\nprocess.stdout.write('+JSON.stringify(JSON.stringify(meta))+');');fs.chmodSync(cargo,0o755);
 write('rust/COPYRIGHT-library.html','Copyright page');write('rust/licenses/MIT.txt','MIT text');write('rust/licenses/ISC.txt','ISC text');write('ndk/NOTICE','NDK text');
 const root=path.join(tmp,'source'),record={key:'fixture',project:'fixture/source',revision:'a'.repeat(40),noticeChecks:[{path:'LICENSE',output:'fixture/LICENSE',object:'obsolete'}]};
 const result=await collectHwuiNotices({work:tmp,module:tmp,tools:{cargo,cargoHome:tmp},sources:{fixture:root},originalSources:{fixture:{record,input:root}}},
  {output:path.join(tmp,'output'),cargo:{manifest,lock,features:[],lockSha256:'obsolete'},rust:{root:path.join(tmp,'rust')},ndk:{root:path.join(tmp,'ndk'),files:[{path:'NOTICE',sha256:'obsolete'}]}});
 assert.equal(fs.readFileSync(path.join(result.root,'fixture/LICENSE'),'utf8'),'Changed accompanying text\n');
 assert.equal(fs.existsSync(path.join(result.root,'sources/fixture/unit.cpp')),false);assert.equal(fs.readFileSync(path.join(root,'unit.cpp'),'utf8'),unit);assert.equal(JSON.parse(fs.readFileSync(path.join(result.sourceRecords,'SOURCES.json'))).registry[0].checksum,'a'.repeat(64));
 assert.equal(fs.readFileSync(path.join(result.root,'rust/dependency-1.0.0/LICENSE'),'utf8'),'Dependency text\n');
 assert.equal(fs.readFileSync(path.join(result.root,'rust-standard-library/licenses/ISC.txt'),'utf8'),'ISC text');
 assert.equal(fs.readFileSync(path.join(result.root,'ndk/NOTICE'),'utf8'),'NDK text');
 assert(result.records.every(row=>Object.keys(row).length===1&&typeof row.path==='string'));
});

test('missing accompanying sources and toolchain files warn and do not reject HWUI collection',async t=>{
 const {collectHwuiNotices}=await import('../src/modules/android/native/hwui/notices.mjs');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'hwui-missing-candidates-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const warnings=[];t.mock.method(console,'warn',message=>warnings.push(message));
 const result=await collectHwuiNotices({work:root,module:root},{output:path.join(root,'output'),rust:{root:path.join(root,'missing')},ndk:{root:path.join(root,'missing'),files:['NOTICE']}});
 assert.equal(result.root,path.join(root,'output'));assert(warnings.length>=3);assert(fs.existsSync(path.join(result.sourceRecords,'SOURCES.json')));assert.equal(fs.existsSync(path.join(result.root,'SOURCES.json')),false);
});

test('HWUI crate selection follows normal runtime edges and excludes host macros and build tools',async()=>{
 const {runtimeCargoPackages}=await import('../src/modules/android/native/hwui/notices.mjs');
 const edge=(pkg,kind=null)=>({pkg,dep_kinds:[{kind}]}),packages=['app','runtime','shared','build-tool','build-child','macro','macro-child','dev-tool'].map(id=>({id,targets:[{kind:[id==='macro'?'proc-macro':'lib']}]}));
 const meta={packages,workspace_members:['app'],resolve:{nodes:[
  {id:'app',deps:[edge('runtime'),edge('shared','build'),edge('build-tool','build'),edge('macro'),edge('dev-tool','dev')]},
  {id:'runtime',deps:[edge('shared')]},{id:'shared',deps:[]},{id:'build-tool',deps:[edge('build-child')]},{id:'build-child',deps:[]},
  {id:'macro',deps:[edge('macro-child')]},{id:'macro-child',deps:[]},{id:'dev-tool',deps:[]}]}};
 assert.deepEqual(runtimeCargoPackages(meta).map(pkg=>pkg.id),['app','runtime','shared']);
});
