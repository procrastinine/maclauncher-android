const test=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),os=require('node:os'),path=require('node:path'),crypto=require('node:crypto');
const {execFileSync}=require('node:child_process');
const {pathToFileURL}=require('node:url');
const {writeZipArchive}=require('./helpers/zip-fixture');
const digest=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
function fixture(t){
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-platform-delivery-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const native=path.join(root,'native'),directory=path.join(root,'input'),output=path.join(root,'runtime');
 fs.mkdirSync(path.join(native,'notices/platform-packages'),{recursive:true});fs.mkdirSync(directory);
 const notice=Buffer.from('Original component notice\n');fs.writeFileSync(path.join(native,'notices/platform-packages/NOTICE'),notice);
 const apk=path.join(directory,'Provider.apk');writeZipArchive(apk,[{name:'AndroidManifest.xml',data:fs.readFileSync(require('./helpers/manifest-fixtures').file(t,'android-framework-services/manifests/providers'))},{name:'classes.dex',data:Buffer.from('unchanged original code')}]);
 const bytes=fs.readFileSync(apk),definition={schemaVersion:1,androidApi:36,files:[{path:'Provider.apk',destination:'packages/Provider/Provider.apk',packageName:'owned.providers',size:bytes.length,sha256:digest(bytes)}],notices:[{path:'NOTICE',size:notice.length,sha256:digest(notice)}]};
 const write=()=>fs.writeFileSync(path.join(native,'platform-packages-binary.json'),JSON.stringify(definition));write();
 return {root,native,directory,output,apk,bytes,definition,write};
}
test('Component delivery preserves original APK bytes, manifest declarations and notices without an image',async t=>{
 const f=fixture(t),{platformPackageInputs,stagePlatformPackages}=await import('../src/modules/android/native/platform-package-delivery.mjs');
 const input=platformPackageInputs(f.native,f.directory),result=stagePlatformPackages({input,output:f.output});
 assert.deepEqual(input.catalog.packages,[{source:'packages/Provider/Provider.apk',packageName:'owned.providers',sharedUserId:null,authorities:['same','same','first','same','second','same'],resolvedAuthorities:false,sha256:digest(f.bytes),size:f.bytes.length}]);
 assert.deepEqual(fs.readFileSync(path.join(result.platform,input.catalog.packages[0].source)),f.bytes);
 assert.deepEqual(JSON.parse(fs.readFileSync(result.catalog)),input.catalog);
 assert.deepEqual(JSON.parse(fs.readFileSync(path.join(result.notices,'provenance.json'))),f.definition);
 assert.equal(fs.readFileSync(path.join(result.notices,'NOTICE'),'utf8'),'Original component notice\n');
 assert.deepEqual(fs.readdirSync(f.output).sort(),['notices','platform']);
 assert.deepEqual(fs.readdirSync(path.join(f.output,'platform/packages')),['Provider']);
});
test('Component inputs reject a wrong manifest identity, changed bytes and escaping file references',async t=>{
 const f=fixture(t),{platformPackageInputs}=await import('../src/modules/android/native/platform-package-delivery.mjs');
 f.definition.files[0].packageName='other.package';f.write();assert.throws(()=>platformPackageInputs(f.native,f.directory),/manifest does not match/);
 f.definition.files[0].packageName='owned.providers';f.definition.files[0].destination='packages/../outside.apk';f.write();assert.throws(()=>platformPackageInputs(f.native,f.directory),/Invalid platform package destination/);
 f.definition.files[0].destination='packages/Provider/Provider.apk';f.write();fs.appendFileSync(f.apk,'changed');assert.throws(()=>platformPackageInputs(f.native,f.directory),/does not match its recorded release/);
 fs.writeFileSync(f.apk,f.bytes);const outside=path.join(f.root,'outside.apk');fs.renameSync(f.apk,outside);fs.symlinkSync(outside,f.apk);assert.throws(()=>platformPackageInputs(f.native,f.directory),/leaves its directory/);
});
test('The ordinary producer snapshot can inspect and stage APKs using only its fixed helper closure',async t=>{
 const f=fixture(t),native=path.resolve(__dirname,'../src/modules/android/native');
 for(const name of ['platform-package-delivery.mjs','platform-packages.mjs','binary-inputs.mjs'])fs.copyFileSync(path.join(native,name),path.join(f.native,name));
 fs.writeFileSync(path.join(f.root,'profile.js'),'module.exports={};\n');
 const source=path.join(f.root,'snapshot');
 const {snapshotSources,snapshotBuildCompanions,sourceInventory}=await import('../src/modules/android/native/build-sources.mjs');
 snapshotSources(f.native,source);
 const companions=snapshotBuildCompanions({repository:path.resolve(__dirname,'..'),destination:path.join(source,'build-tools')});
 const captured=sourceInventory(companions.root);
 for(const name of ['apk.js','binary-manifest.js']){
  const relative='src/modules/android/'+name,record=captured.find(row=>row.path===relative);
  assert.equal(record.sha256,digest(fs.readFileSync(path.resolve(__dirname,'../'+relative))));
 }
 assert(captured.some(row=>row.path==='src/modules/shared/runtime/zip.js'));
 // Invalidate the source-side generators after capture. The child must use
 // the fixed projection, including APK/manifest/ZIP helpers, from outside the
 // checkout and without a launcher/module registry or Electron dependency.
 for(const name of ['platform-package-delivery.mjs','platform-packages.mjs','binary-inputs.mjs'])fs.writeFileSync(path.join(f.native,name),'throw Error("Uncaptured source was loaded");\n');
 const module=pathToFileURL(path.join(source,'native/platform-package-delivery.mjs')).href;
 const script='import {platformPackageInputs,stagePlatformPackages} from '+JSON.stringify(module)+';const input=platformPackageInputs('+JSON.stringify(path.join(source,'native'))+','+JSON.stringify(f.directory)+');console.log(JSON.stringify(stagePlatformPackages({input,output:'+JSON.stringify(f.output)+'})));';
 const result=JSON.parse(execFileSync(process.execPath,['--input-type=module','-e',script],{cwd:os.tmpdir(),encoding:'utf8',timeout:10000}));
 assert.equal(result.packages[0].packageName,'owned.providers');
 assert.deepEqual(fs.readFileSync(path.join(result.platform,result.packages[0].source)),f.bytes);
});
test('Recorded SettingsProvider component stages offline with its original system UID and settings authority',async t=>{
 const apk=process.env.ANDROID_SETTINGS_PROVIDER_APK;if(!apk)return t.skip('Requires the recorded original SettingsProvider APK');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-original-settings-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const directory=path.join(root,'input'),output=path.join(root,'runtime');fs.mkdirSync(directory);
 fs.copyFileSync(apk,path.join(directory,'SettingsProvider.apk'),fs.constants.COPYFILE_FICLONE);
 const {platformPackageInputs,stagePlatformPackages}=await import('../src/modules/android/native/platform-package-delivery.mjs');
 const input=platformPackageInputs(path.resolve(__dirname,'../src/modules/android/native'),directory);
 const result=stagePlatformPackages({input,output});
 assert.deepEqual(input.catalog.packages,[{source:'packages/SettingsProvider/SettingsProvider.apk',packageName:'com.android.providers.settings',sharedUserId:'android.uid.system',authorities:['settings'],resolvedAuthorities:true,sha256:'1117b80968f3f88f2ea36c4b6a555ecd8b8000042b3ea65acd9d7be9838c183a',size:561460}]);
 assert.equal(digest(fs.readFileSync(path.join(result.platform,input.catalog.packages[0].source))),input.catalog.packages[0].sha256);
 assert.deepEqual(fs.readdirSync(path.join(output,'platform/packages')),['SettingsProvider']);
 assert.equal(fs.existsSync(path.join(output,'art')),false);
});
