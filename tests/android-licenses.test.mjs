import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {addLicense,includeLicenses} from '../src/modules/android/native/upstream/licenses.mjs';
import {assembleArtSupplier,artSupplierComponents} from '../src/modules/android/art/build.mjs';
import {stageArtRunnerLicenses} from '../src/modules/android/art/build-runner.mjs';
import {collectHwuiLicenses,runtimeCargoPackages} from '../src/modules/android/native/hwui/licenses.mjs';
function fixture(t){
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-licenses-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const write=(name,bytes)=>{const file=path.join(root,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,bytes);return file;};
 return {root,write};
}
test('one package stores whitespace-equivalent texts once, names collisions, and recreates its index',t=>{
 const {root,write}=fixture(t),out=path.join(root,'package');
 const first=write('one','A licence\nwith terms\n'),same=write('two',' A\tlicence with terms\r\n'),other=write('three','Different terms');
 addLicense(out,'one',first,'MIT');addLicense(out,'two',same,'Other');addLicense(out,'three',other,'MIT');
 assert.deepEqual(fs.readdirSync(path.join(out,'licenses')).sort(),['MIT-2.txt','MIT.txt','components.txt']);
 assert.equal(fs.readFileSync(path.join(out,'licenses/components.txt'),'utf8'),'one: MIT.txt\ntwo: MIT.txt\nthree: MIT-2.txt\n');
 assert.equal(fs.readFileSync(path.join(out,'licenses/MIT.txt'),'utf8'),fs.readFileSync(first,'utf8'));
 // A fresh process reconstructs the map from the existing payload without adding another text.
 execFileSync(process.execPath,['--input-type=module','-e',`import {addLicense} from ${JSON.stringify(new URL('../src/modules/android/native/upstream/licenses.mjs',import.meta.url).href)};addLicense(${JSON.stringify(out)},'restart',${JSON.stringify(same)},'again');`]);
 assert.equal(fs.readdirSync(path.join(out,'licenses')).length,3);
 fs.rmSync(path.join(out,'licenses'),{recursive:true});addLicense(out,'recreated',same,'MIT');
 assert.deepEqual(fs.readdirSync(path.join(out,'licenses')).sort(),['MIT.txt','components.txt']);
 assert.equal(fs.readFileSync(path.join(out,'licenses/components.txt'),'utf8'),'recreated: MIT.txt\n');
});
test('producer indices merge into one flat package and unavailable texts only warn',t=>{
 const {root,write}=fixture(t),one=path.join(root,'one'),two=path.join(root,'two'),out=path.join(root,'out'),warnings=[];
 t.mock.method(console,'log',message=>warnings.push(message));
 addLicense(one,'A',write('a','same text'),'MIT');addLicense(two,'B',write('b',' same\ntext '),'Apache-2.0');
 addLicense(two,'C',write('c','different'),'MIT');includeLicenses(out,path.join(one,'licenses'));includeLicenses(out,path.join(two,'licenses'));
 includeLicenses(out,path.join(out,'licenses'));
 assert.deepEqual(fs.readdirSync(path.join(out,'licenses')).sort(),['MIT-2.txt','MIT.txt','components.txt']);
 assert.equal(fs.readFileSync(path.join(out,'licenses/components.txt'),'utf8'),'A: MIT.txt\nB: MIT.txt\nC: MIT-2.txt\n');
 addLicense(out,'missing',path.join(root,'absent'));addLicense(out,'directory',root);includeLicenses(out,undefined);
 assert.equal(warnings.length,3);assert(warnings.every(message=>message.startsWith('::warning::')));
});
test('ART assembly merges embedded and runner licences without manifest notice rows',t=>{
 const {root,write}=fixture(t),original=path.join(root,'input');fs.mkdirSync(original);
 const image=path.join(original,'image');fs.mkdirSync(image);write('input/image/identity','image');
 const component=write('input/component','component'),text=write('LICENSE','Shared licence\n');
 const inputs=Object.fromEntries(Object.keys(artSupplierComponents()).map(key=>[key,['image','art','halRoot','halManifests'].includes(key)?image:component]));
 const runner=path.join(root,'runner'),art=path.join(root,'art');fs.mkdirSync(art);inputs.art=art;
 addLicense(runner,'runner',text,'MIT');addLicense(art,'ART',text,'Other');inputs.runnerLicenses=path.join(runner,'licenses');
 inputs.lockHolder=write('lock','lock');inputs.ptyHolder=write('pty','pty');
 const output=path.join(root,'runtime'),manifest=assembleArtSupplier({output,inputs,bootClasspath:[],sourceVersion:'0.1.0-test'});
 assert.equal(Object.hasOwn(manifest,'notices'),false);assert.equal(Object.hasOwn(manifest.components,'runnerLicenses'),false);
 assert.equal(fs.existsSync(path.join(output,'art/licenses')),false);
 const texts=fs.readdirSync(path.join(output,'licenses')).filter(file=>file!=='components.txt');
 assert.equal(texts.length,2); // shared text plus the Android image's own licence
 const index=fs.readFileSync(path.join(output,'licenses/components.txt'),'utf8');assert.match(index,/ART: Other.txt/);assert.match(index,/runner: Other.txt/);
 assert.equal(fs.readFileSync(path.join(output,'host/aim-lock-holder'),'utf8'),'lock');
 const compact=path.join(root,'compact'),{image:unused,...retained}=inputs;
 const next=assembleArtSupplier({output:compact,inputs:retained,bootClasspath:[],sourceVersion:'0.1.0-test',baseSupplier:{root:output,manifest},includeImage:false,originalImage:{archiveSha256:manifest.android.imageSha256,content:manifest.platformSupplier.image}});
 assert.deepEqual(next.platformSupplier.image,manifest.platformSupplier.image);assert.equal(fs.existsSync(path.join(compact,'image')),false);
 assert.equal(fs.readdirSync(path.join(compact,'licenses')).length,3);
});
test('runner stages original licence texts and CPU associations without source-record copies',t=>{
 const {root,write}=fixture(t),aim=path.join(root,'aim'),cpu=path.join(root,'cpu'),output=path.join(root,'out');
 for(const name of ['LICENSE','NOTICE','LICENSING.md','THIRD_PARTY_NOTICES.md','licensing/OPENJDK.md','licensing/rust-dependencies.txt','licensing/third-party/art-openjdkjvm-LICENSE'])write('aim/'+name,name+' text');
 write('aim/licensing/upstream-sources.json','not licence text');addLicense(cpu,'Unicorn',write('cpu-license','CPU text'));
 const directory=stageArtRunnerLicenses({aim,cpuLicenses:path.join(cpu,'licenses'),output});
 assert.equal(directory,path.join(output,'licenses'));assert(fs.readdirSync(directory,{withFileTypes:true}).every(row=>row.isFile()));
 const index=fs.readFileSync(path.join(directory,'components.txt'),'utf8');assert.match(index,/Unicorn:/);assert.match(index,/AIM\/art-openjdkjvm-LICENSE:/);
 assert.equal(fs.existsSync(path.join(output,'aim')),false);
 assert(fs.readdirSync(directory).every(file=>!fs.readFileSync(path.join(directory,file),'utf8').includes('not licence text')));
});
test('HWUI uses known crate licences and exports source records separately',async t=>{
 const {root,write}=fixture(t),source='registry+https://github.com/rust-lang/crates.io-index';
 const manifest=write('cargo/Cargo.toml','[workspace]\n'),lock=write('cargo/Cargo.lock','version = 4\n[[package]]\nname = "dependency"\nversion = "1.0.0"\nsource = "'+source+'"\nchecksum = "'+'a'.repeat(64)+'"\n');
 const library=write('registry/LICENSE','Library terms'),meta={workspace_members:['app'],resolve:{nodes:[{id:'app',deps:[{pkg:'dependency',dep_kinds:[{kind:null}]}]},{id:'dependency',deps:[]}]},packages:[{id:'dependency',name:'dependency',version:'1.0.0',license:'MIT',source,manifest_path:path.join(path.dirname(library),'Cargo.toml')}]};
 const cargo=write('cargo-tool','#!'+process.execPath+'\nprocess.stdout.write('+JSON.stringify(JSON.stringify(meta))+');');fs.chmodSync(cargo,0o755);
 write('source/LICENSE','Source terms');write('rust/COPYRIGHT-library.html','Copyright page');write('rust/licenses/MIT.txt','Library\n terms');write('ndk/NOTICE','NDK terms');
 const record={key:'framework',project:'fixture/framework',revision:'b'.repeat(40),licenseFiles:['LICENSE']};
 const result=await collectHwuiLicenses({module:root,tools:{cargo,cargoHome:root},sources:{framework:path.join(root,'source')},originalSources:{framework:{record,input:path.join(root,'source')}}},{output:path.join(root,'output'),cargo:{manifest,lock,features:[]},rust:{root:path.join(root,'rust')},ndk:{root:path.join(root,'ndk'),files:['NOTICE']}});
 assert.equal(JSON.parse(fs.readFileSync(path.join(result.sourceRecords,'SOURCES.json'))).registry[0].checksum,'a'.repeat(64));
 assert.equal(fs.existsSync(path.join(result.root,'SOURCES.json')),false);assert.equal(fs.existsSync(path.join(result.root,'Cargo.lock')),false);
 const index=fs.readFileSync(path.join(result.root,'components.txt'),'utf8');assert.match(index,/dependency-1.0.0: MIT.txt/);assert.match(index,/rust-standard-library: MIT.txt/);
 assert(fs.readdirSync(result.root,{withFileTypes:true}).every(entry=>entry.isFile()));
 const edge=(pkg,kind=null)=>({pkg,dep_kinds:[{kind}]}),packages=['app','runtime','build','macro'].map(id=>({id,targets:[{kind:[id==='macro'?'proc-macro':'lib']}]}));
 assert.deepEqual(runtimeCargoPackages({packages,workspace_members:['app'],resolve:{nodes:[{id:'app',deps:[edge('runtime'),edge('build','build'),edge('macro')]},{id:'runtime',deps:[]},{id:'build',deps:[]},{id:'macro',deps:[]}]}}).map(pkg=>pkg.id),['app','runtime']);
});
