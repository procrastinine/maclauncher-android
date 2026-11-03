import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {pathToFileURL} from 'node:url';
import {assembleArtSupplier,artSupplierComponents} from '../src/modules/android/art/build.mjs';
import {buildArtRunner} from '../src/modules/android/art/build-runner.mjs';
import {nativeBridgeTrees} from '../src/modules/android/art/source-archive.mjs';
import {arm32BuildProducts} from '../src/modules/android/native/arm32/components.mjs';

test('native-bridge source export uses the ARM32 source output independently of licence delivery',t=>{
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-bridge-source-records-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const products=arm32BuildProducts(root),revision='a'.repeat(40);fs.mkdirSync(products.sources);
 const write=(name,value)=>fs.writeFileSync(path.join(products.sources,name),JSON.stringify(value));
 write('bionic.json',{revision,files:['libc/source.c']});write('bionic-portable-families.json',{revision,jump:['libc/jump.c']});
 assert.equal(fs.existsSync(products.licenses),false);
 assert(nativeBridgeTrees({arm32Sources:products.sources},revision).includes('libc/source.c'));
 assert(nativeBridgeTrees({arm32Sources:products.sources},revision).includes('libc/jump.c'));
 write('bionic.json',{revision:'b'.repeat(40),files:[]});assert.throws(()=>nativeBridgeTrees({arm32Sources:products.sources},revision),/source record revision differs/);
 fs.unlinkSync(path.join(products.sources,'bionic.json'));assert.throws(()=>nativeBridgeTrees({arm32Sources:products.sources},revision),/ENOENT/);
});

test('prepared ART runner builds offline with its native continuation and static CPU',{timeout:600000},t=>{
 const file=process.env.ANDROID_ART_RUNNER_INPUTS;
 if(!file)return t.skip('Requires prepared AIM source and native runner inputs');
 const options=JSON.parse(fs.readFileSync(file,'utf8'));
 const result=buildArtRunner(options);
 for(const name of ['linuxRun','binder','lockHolder','ptyHolder']){
  execFileSync('codesign',['--verify','--strict',result[name]],{stdio:'pipe'});
  assert.equal(execFileSync('lipo',['-archs',result[name]],{encoding:'utf8'}).trim(),'arm64');
  const load=execFileSync('otool',['-l',result[name]],{encoding:'utf8'});
  assert.match(load,/cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform \d+\s+minos 13\.0\b/);
 }
 execFileSync(result.linuxRun,['--help'],{timeout:10000,stdio:'pipe'});
 assert(fs.readFileSync(path.join(result.runnerLicenses,'components.txt'),'utf8').includes('unicorn'));
});


test('ordinary ART entry returns its fresh sources and preserves explicit archives without compiling fixtures',async t=>{
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-art-entry-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const write=(name,text)=>{const file=path.join(root,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,text);return file;};
 write('tools/build-art.mjs',fs.readFileSync(new URL('../tools/build-art.mjs',import.meta.url)));
 write('src/modules/android/art/build-elf.mjs',"export function buildArtElfs(){throw Error('Unexpected native build');}\n");
 write('src/modules/android/art/source.mjs',`import fs from 'node:fs';import path from 'node:path';export function prepareAimSource(options){fs.mkdirSync(options.output,{recursive:true});fs.writeFileSync(path.join(options.output,'prepared.json'),JSON.stringify(options));}`);
 write('src/modules/android/art/build.mjs',`import fs from 'node:fs';import path from 'node:path';export function buildArt(options){const prepared=JSON.parse(fs.readFileSync(path.join(options.host.aim,'prepared.json')));if(!prepared.mutableCode||options.runner.aim!==options.host.aim)throw Error('Wrong ART source');const runtime=path.join(options.output,'runtime'),cache=path.join(options.output,'build/compiler-cache');fs.mkdirSync(runtime,{recursive:true});fs.mkdirSync(path.dirname(cache),{recursive:true});if(!fs.existsSync(cache))fs.writeFileSync(cache,'reusable compiler cache');if(options.hwui){const registry=path.join(options.hwui.work||path.join(options.output,'build/hwui'),'module/sources/SOURCES.json');fs.mkdirSync(path.dirname(registry),{recursive:true});fs.writeFileSync(registry,'{}');}return {execution:'art',prepared};}`);
 write('src/modules/android/art/source-archive.mjs',`import fs from 'node:fs';import path from 'node:path';export function exportArtSourceArchive({output,sources}){if(!sources.aosp)throw Error('Missing original source');if(fs.existsSync(path.join(path.dirname(output),'fail-source-export')))throw Error('Deferred original source unavailable');fs.mkdirSync(path.dirname(output),{recursive:true});fs.writeFileSync(output,'complete-source');return output;}`);
 const inputs=write('inputs.json',JSON.stringify({output:path.join(root,'output'),host:{},java:{},bootClasspath:[],sources:{source:root,klepton:root,glesHeaders:root,aosp:root},inputs:{art:root}}));
 const {buildArtRuntime}=await import(pathToFileURL(path.join(root,'tools/build-art.mjs')));
 const result=await buildArtRuntime({inputs});assert.equal(result.execution,'art');assert.equal(result.root,path.join(root,'output/runtime'));assert.equal(fs.readFileSync(result.sourceArchive,'utf8'),'complete-source');
 fs.mkdirSync(result.root,{recursive:true});await assert.rejects(buildArtRuntime({inputs}),/supplier output already exists/);
 const explicit=write('explicit.tar.gz','caller-complete-source');write('inputs.json',JSON.stringify({output:path.join(root,'explicit-output'),host:{},java:{},bootClasspath:[],sources:{source:root,klepton:root,glesHeaders:root},inputs:{art:root},sourceArchive:explicit}));
 const retained=await buildArtRuntime({inputs});assert.equal(retained.sourceArchive,explicit);assert.equal(fs.readFileSync(explicit,'utf8'),'caller-complete-source');assert.equal(fs.existsSync(path.join(root,'explicit-output/source.tar.gz')),false);
 write('inputs.json',JSON.stringify({output:path.join(root,'rejected'),host:{},java:{},inputs:{art:root},bootClasspath:[],sources:{source:root,klepton:root,glesHeaders:root}}));
 await assert.rejects(buildArtRuntime({inputs}),/Missing original source/);assert.equal(fs.existsSync(path.join(root,'rejected/build/aim-source')),false);
 const deferredOutput=path.join(root,'deferred-output');write('deferred-output/fail-source-export','fail once');
 write('inputs.json',JSON.stringify({output:deferredOutput,host:{},java:{},bootClasspath:[],sources:{source:root,klepton:root,glesHeaders:root,aosp:root},inputs:{art:root},hwui:{sources:{},tools:{cargoHome:root}}}));
 await assert.rejects(buildArtRuntime({inputs}),/Deferred original source unavailable/);assert.equal(fs.existsSync(path.join(deferredOutput,'runtime')),false);assert.equal(fs.readFileSync(path.join(deferredOutput,'build/compiler-cache'),'utf8'),'reusable compiler cache');
 fs.rmSync(path.join(deferredOutput,'fail-source-export'));const retried=await buildArtRuntime({inputs});assert.equal(fs.readFileSync(retried.sourceArchive,'utf8'),'complete-source');assert(fs.statSync(retried.root).isDirectory());
 write('deferred-output/runtime/preserved','preexisting supplier');await assert.rejects(buildArtRuntime({inputs}),/supplier output already exists/);assert.equal(fs.readFileSync(path.join(retried.root,'preserved'),'utf8'),'preexisting supplier');
});

test('ART source export preserves locked AOSP and notices while omitting unused tests and unrelated files',async t=>{
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-art-archive-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const write=(name,text)=>{const file=path.join(root,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,text);return file;};
 const original=path.resolve(import.meta.dirname,'..');
 for(const name of ['src/modules/android/art/source-archive.mjs','src/modules/android/art/source-hwui.mjs','src/modules/android/native/hwui/inputs.mjs','src/modules/android/native/hwui/sources.json','src/modules/android/native/hwui/Cargo.lock','src/modules/android/native/build-sources.mjs','src/modules/android/native/upstream/source-export.mjs','src/modules/android/native/upstream/source-files.mjs','src/modules/android/native/upstream/licenses.mjs','src/modules/shared/runtime/runtime-content.js'])write(name,fs.readFileSync(path.join(original,name)));
 for(const name of ['README.md','LICENSE','package.json','.gitmodules','tools/build.mjs','tools/build-art.mjs','tools/check-sources.mjs','src/modules/android/profile.js','src/modules/android/apk.js','src/modules/android/binary-manifest.js','src/modules/nitroplus/runtime/portable-media.json'])write(name,'original production input\n');
 write('package.json','{}\n');
 write('src/modules/android/native/original.c','owned implementation\n');write('src/modules/android/native/original_test.c','unused test\n');write('integration-notes.json','unrelated local build notes\n');
 const {createHash}=await import('node:crypto'),hash=bytes=>createHash('sha256').update(bytes).digest('hex');
 const runtime={'runtime.cc':'original ART source\n','runtime_test.cc':'original unused test\n'};
 const treeHash=hash(Object.entries(runtime).sort().map(([name,bytes])=>hash(bytes)+'  ./'+name+'\n').join(''));
 for(const [name,bytes]of Object.entries(runtime))write('aosp/art/runtime/'+name,bytes);
 fs.symlinkSync('runtime.cc',path.join(root,'aosp/art/runtime/original-link.h'));
 const header='original bionic declaration\n';write('aosp/bionic/libc/include/header.h',header);write('aosp/unselected-note.json','unselected data');
 const lock='AOSP_TAG=android-16.0.0_r1\nSOURCE_TREES=(\n "platform/art|runtime|'+treeHash+'"\n)\nSOURCE_FILES=(\n "platform/bionic|libc/include/header.h|'+hash(header)+'"\n)\n';
 const plugin='original JVMTI source\n';write('aosp/art/openjdkjvmti/plugin.cc',plugin);
 const pluginHash=hash(hash(plugin)+'  ./plugin.cc\n');
 write('src/modules/android/art/adapters/art-android-sources.lock',lock.replace('SOURCE_TREES=(\n','SOURCE_TREES=(\n "platform/art|openjdkjvmti|'+pluginHash+'"\n'));

 const projects=[];
 for(const [id,contents]of Object.entries({aim:{'patches/art-android/sources.lock':lock,'licensing/third-party/art-NOTICE':'original ART notice\n','src/original.rs':'original Rust\n'},klepton:{'runtime/original.c':'original host source\n'},'android-gles-headers':{'include/EGL/egl.h':'original header\n','scripts/gl.xml':'original registry\n'},unicorn:{'src/original.c':'original CPU source\n'},'aosp-native':{'libs/binder/Parcel.cpp':'original Binder source\n'}})){
  const checkout=path.join(root,'checkouts',id);for(const [name,bytes]of Object.entries({...contents,LICENSE:'required license\n','tests/unused.c':'unused upstream test\n','src/build.ninja':'generated graph\n'}))write(path.relative(root,path.join(checkout,name)),bytes);
  execFileSync('git',['-C',checkout,'init','-q']);execFileSync('git',['-C',checkout,'add','.']);execFileSync('git',['-C',checkout,'-c','user.name=Source test','-c','user.email=test@example.invalid','commit','-qm','Pinned source']);const revision=execFileSync('git',['-C',checkout,'rev-parse','HEAD'],{encoding:'utf8'}).trim();projects.push({id,path:'src/external/'+id,repository:'https://example.invalid/'+id,revision,licensePaths:['LICENSE']});
 }
 const xsdc=path.join(root,'checkouts/xsdc');write('checkouts/xsdc/src/Generator.java','original source generator\n');execFileSync('git',['-C',xsdc,'init','-q']);execFileSync('git',['-C',xsdc,'add','.']);execFileSync('git',['-C',xsdc,'-c','user.name=Source test','-c','user.email=test@example.invalid','commit','-qm','Pinned generator']);const xsdcRevision=execFileSync('git',['-C',xsdc,'rev-parse','HEAD'],{encoding:'utf8'}).trim();write('checkouts/aim/upstream/android16-xsdc.lock','XSDC_REVISION='+xsdcRevision+'\n');execFileSync('git',['-C',path.join(root,'checkouts/aim'),'add','.']);execFileSync('git',['-C',path.join(root,'checkouts/aim'),'-c','user.name=Source test','-c','user.email=test@example.invalid','commit','-qm','Pin generator']);projects.find(row=>row.id==='aim').revision=execFileSync('git',['-C',path.join(root,'checkouts/aim'),'rev-parse','HEAD'],{encoding:'utf8'}).trim();
 write('upstreams.json',JSON.stringify({projects}));
 const {exportArtSourceArchive}=await import(pathToFileURL(path.join(root,'src/modules/android/art/source-archive.mjs')));
 const options={output:path.join(root,'out/source.tar.gz'),sources:{source:path.join(root,'checkouts/aim'),klepton:path.join(root,'checkouts/klepton'),glesHeaders:path.join(root,'checkouts/android-gles-headers'),aosp:path.join(root,'aosp'),xsdc:path.join(root,'checkouts/xsdc')},unicorn:path.join(root,'checkouts/unicorn'),sourceCheckouts:{'aosp-native':path.join(root,'checkouts/aosp-native')}};
 assert.equal(exportArtSourceArchive(options),options.output);
 const first=fs.readFileSync(options.output),time=fs.statSync(options.output).mtimeMs;await new Promise(resolve=>setTimeout(resolve,1100));assert.equal(exportArtSourceArchive(options),options.output);assert.deepEqual(fs.readFileSync(options.output),first);assert.equal(fs.statSync(options.output).mtimeMs,time,'An unchanged retry retains the generated archive.');
 const extracted=path.join(root,'extracted');fs.mkdirSync(extracted);execFileSync('/usr/bin/tar',['-xf',options.output,'-C',extracted]);
 assert.equal(fs.readFileSync(path.join(extracted,'upstream/aim-aosp/art/openjdkjvmti/plugin.cc'),'utf8'),plugin);
 assert.equal(fs.readFileSync(path.join(extracted,'upstream/aim-aosp/art/runtime/runtime.cc'),'utf8'),runtime['runtime.cc']);assert.equal(fs.existsSync(path.join(extracted,'upstream/aim-aosp/art/runtime/runtime_test.cc')),false);assert.equal(fs.existsSync(path.join(extracted,'upstream/aim-aosp/unselected-note.json')),false);assert.equal(fs.existsSync(path.join(extracted,'integration-notes.json')),false);assert.equal(fs.existsSync(path.join(extracted,'src/modules/android/native/original_test.c')),false);
 assert.equal(fs.readlinkSync(path.join(extracted,'upstream/aim-aosp/art/runtime/original-link.h')),'runtime.cc');
 const names=execFileSync('/usr/bin/tar',['-tf',path.join(extracted,'upstream/aim.tar')],{encoding:'utf8'});assert.match(names,/licensing\/third-party\/art-NOTICE/);assert.match(names,/LICENSE/);assert.doesNotMatch(names,/tests\/|build\.ninja/);assert.equal(fs.readFileSync(path.join(extracted,'source-inputs.json'),'utf8').includes(root),false);
 write('aosp/art/runtime/runtime.cc','changed original source');assert.throws(()=>exportArtSourceArchive({...options,output:path.join(root,'out/changed.tar.gz')}),/differs from its source lock/);assert.equal(fs.existsSync(path.join(root,'out/changed.tar.gz')),false);
 assert.throws(()=>exportArtSourceArchive({...options,output:path.join(root,'out/missing.tar.gz'),sources:{...options.sources,aosp:undefined}}),/sources.aosp/);
});

test('HWUI source export narrows legacy trees and authenticates selected registry archives',async t=>{
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-hwui-export-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const write=(name,text)=>{const file=path.join(root,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,text);return file;};
 const original=path.resolve(import.meta.dirname,'..');for(const name of ['src/modules/android/art/source-hwui.mjs','src/modules/android/native/hwui/inputs.mjs','src/modules/android/native/upstream/source-export.mjs','src/modules/android/native/upstream/source-files.mjs','src/modules/android/native/upstream/licenses.mjs','src/modules/shared/runtime/runtime-content.js'])write(name,fs.readFileSync(path.join(original,name)));
 const checkout=path.join(root,'original');write('original/core/jni/include/required.h','original declaration\n');write('original/unrelated/unused.txt','unrelated original source\n');write('original/NOTICE','required original notice\n');
 execFileSync('git',['-C',checkout,'init','-q']);execFileSync('git',['-C',checkout,'add','.']);execFileSync('git',['-C',checkout,'-c','user.name=Source test','-c','user.email=test@example.invalid','commit','-qm','Pinned source']);
 const git=args=>execFileSync('git',['-C',checkout,...args],{encoding:'utf8'}).trim(),revision=git(['rev-parse','HEAD']);
 git(['sparse-checkout','set','--no-cone','/NOTICE']);assert.equal(fs.existsSync(path.join(checkout,'core/jni/include/required.h')),false);
 const sourceCatalog={sources:[{key:'frameworkLegacy',project:'platform/frameworks/base',repository:'https://example.invalid/framework',revision,licensePaths:['NOTICE','ALTERNATIVE-LICENSE'],checks:[{path:'.',type:'tree',object:git(['rev-parse','HEAD^{tree}']),mode:16384}]}],includes:[{source:'frameworkLegacy',path:'core/jni/include'}]};
 const extraSources={};
 for(const fixture of [
  {key:'libhardware',tree:'.',license:'NOTICE',files:{'include_all/hardware/hardware.h':'original hardware header\n'},links:{'include/hardware/hardware.h':'../../include_all/hardware/hardware.h'}},
  {key:'native',tree:'include',license:'NOTICE',files:{'include/required.h':'original native header\n'},links:{'include/layerproto':'../services/surfaceflinger/layerproto/include/layerproto','include/private/binder':'../../libs/binder/include/private/binder'}},
  {key:'skia',tree:'src',license:'LICENSE',files:{'src/active.cpp':'original Android Skia source\n'},links:{'src/ports/fontations/Cargo.toml':'../../../bazel/external/fontations/Cargo.toml'}}
 ]){
  const directory=path.join(root,'original-'+fixture.key);for(const [name,bytes]of Object.entries({...fixture.files,[fixture.license]:'original license\n'}))write(path.relative(root,path.join(directory,name)),bytes);
  for(const [name,target]of Object.entries(fixture.links)){const file=path.join(directory,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.symlinkSync(target,file);}
  const gitHere=args=>execFileSync('git',['-C',directory,...args],{encoding:'utf8'}).trim();gitHere(['init','-q']);gitHere(['add','.']);gitHere(['-c','user.name=Source test','-c','user.email=test@example.invalid','commit','-qm','Pinned original source']);
  sourceCatalog.sources.push({key:fixture.key,project:fixture.key,repository:'https://example.invalid/'+fixture.key,revision:gitHere(['rev-parse','HEAD']),licensePaths:[fixture.license],checks:[{path:fixture.tree,type:'tree',object:gitHere(['rev-parse',fixture.tree==='.'?'HEAD^{tree}':'HEAD:'+fixture.tree]),mode:16384}]});
  if(fixture.key==='libhardware')sourceCatalog.includes.push({source:'libhardware',path:'include'});extraSources[fixture.key]=directory;
 }
 write('src/modules/android/native/hwui/sources.json',JSON.stringify(sourceCatalog));
 write('crate/mini-1.0/src/lib.rs','pub fn original() {}\n');write('crate/mini-1.0/tests/unused.rs','unused test\n');write('crate/mini-1.0/LICENSE','original crate license\n');
 const cache=path.join(root,'cache');fs.mkdirSync(cache);const crate=path.join(cache,'mini-1.0.crate');execFileSync('/usr/bin/tar',['-czf',crate,'-C',path.join(root,'crate'),'mini-1.0']);
 const {createHash}=await import('node:crypto'),hash=bytes=>createHash('sha256').update(bytes).digest('hex'),checksum=hash(fs.readFileSync(crate)),source='registry+https://github.com/rust-lang/crates.io-index';
 write('src/modules/android/native/hwui/Cargo.lock','version = 4\n\n[[package]]\nname = "mini"\nversion = "1.0"\nsource = "'+source+'"\nchecksum = "'+checksum+'"\n');
 const {exportHwuiSources}=await import(pathToFileURL(path.join(root,'src/modules/android/art/source-hwui.mjs')));
 const options={sources:{frameworkLegacy:checkout,...extraSources},registry:[{name:'mini',version:'1.0',source}],registryCache:cache},output=path.join(root,'published'),before=process.env.GIT_NO_LAZY_FETCH;
 const result=exportHwuiSources(options,output);assert.equal(process.env.GIT_NO_LAZY_FETCH,before);
 assert.equal(fs.readFileSync(path.join(output,'sources/frameworkLegacy/core/jni/include/required.h'),'utf8'),'original declaration\n');assert.equal(fs.existsSync(path.join(output,'sources/frameworkLegacy/unrelated')),false);assert.equal(fs.readFileSync(path.join(output,'sources/frameworkLegacy/NOTICE'),'utf8'),'required original notice\n');
 const hardwareHeader=path.join(output,'sources/libhardware/include/hardware/hardware.h');assert(fs.lstatSync(hardwareHeader).isSymbolicLink());assert.equal(fs.readFileSync(hardwareHeader,'utf8'),'original hardware header\n');
 for(const name of ['native/include/layerproto','native/include/private/binder','skia/src/ports/fontations/Cargo.toml'])assert.equal(fs.lstatSync(path.join(output,'sources',name),{throwIfNoEntry:false}),undefined);
 const dependency=result.registry[0];assert.equal(fs.readFileSync(path.join(output,dependency.archive)).readUInt32LE(4),0);assert.equal(dependency.checksum,checksum);assert.equal(dependency.sha256,hash(fs.readFileSync(path.join(output,dependency.archive))));assert.equal(JSON.stringify(result).includes(root),false);
 const names=execFileSync('/usr/bin/tar',['-tzf',path.join(output,dependency.archive)],{encoding:'utf8'});assert.match(names,/src\/lib\.rs/);assert.match(names,/LICENSE/);assert.doesNotMatch(names,/tests\//);
 const next=exportHwuiSources(options,path.join(root,'retry'));assert.deepEqual(next,result);
 fs.symlinkSync('../tests/unused.rs',path.join(root,'crate/mini-1.0/src/dangling.rs'));execFileSync('/usr/bin/tar',['-czf',crate,'-C',path.join(root,'crate'),'mini-1.0']);
 const nextChecksum=hash(fs.readFileSync(crate));write('src/modules/android/native/hwui/Cargo.lock',fs.readFileSync(path.join(root,'src/modules/android/native/hwui/Cargo.lock'),'utf8').replace(checksum,nextChecksum));
 const broken=await import(pathToFileURL(path.join(root,'src/modules/android/art/source-hwui.mjs')).href+'?dangling');assert.throws(()=>broken.exportHwuiSources(options,path.join(root,'dangling')),/alias has no included target/);
 fs.writeFileSync(crate,'changed registry archive');assert.throws(()=>exportHwuiSources(options,path.join(root,'changed')),/Authenticated HWUI registry source is unavailable/);assert.equal(process.env.GIT_NO_LAZY_FETCH,before);
});
