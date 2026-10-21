const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os'),{spawnSync}=require('node:child_process');
test('Original ART process broker enforces dumpability for real peer reads and maps',{timeout:60000},async t=>{
 const runner=process.env.ANDROID_ART_LINUX_RUN,payload=process.env.ANDROID_ART_RUNTIME,image=process.env.ANDROID_ART_IMAGE,ndk=process.env.ANDROID_NDK_TOOLS;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!runner||!payload||!image||!ndk)return t.skip('Requires Apple Silicon, adapted runner, original image and Android compiler');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-art-process-policy-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const run=(file,args,options={})=>{const result=spawnSync(file,args,{encoding:'utf8',timeout:45000,maxBuffer:2<<20,...options});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result;};
 const native=path.resolve(__dirname,'../src/modules/android/native'),module=path.join(temporary,'identity.dylib');
 const stub=path.join(temporary,'extension.c');fs.writeFileSync(stub,'#include <stdint.h>\nint64_t maclauncher_art_host_call(uint32_t f,uint64_t a,uint64_t n,void *c){(void)f;(void)a;(void)n;(void)c;return -38;}\n');
 run('clang',['-dynamiclib','-arch','arm64','-mmacosx-version-min=13.0','-std=gnu11','-O2','-Wall','-Wextra','-Werror',path.join(native,'process-ids.c'),path.join(native,'thread-identity.c'),stub,'-o',module]);
 const executable=path.join(temporary,'probe');run(path.join(ndk,'aarch64-linux-android35-clang'),['-O2','-Wall','-Wextra','-Werror',path.join(__dirname,'fixtures/android-native/art-process-policy.c'),'-o',executable]);
 const root=path.resolve(payload,'art'),manifest=JSON.parse(fs.readFileSync(path.join(root,'runtime.json')));
 const components=Object.fromEntries(Object.entries(manifest.components).map(([name,file])=>[name,path.resolve(root,file)]));
 const state=path.join(temporary,'state');fs.mkdirSync(state);const ids=path.join(temporary,'ids');fs.mkdirSync(ids,{mode:0o700});
 // The original bootstrap owns the complete image layout. This executable
 // needs only Bionic, not a launcher or a Java/ART framework overlay.
 let text=run(components.bootstrap,['layout',path.resolve(image),state]).stdout;
 text+='ro\t/data/local/tmp/policy\t'+executable+'\n';
 const rows=text.trim().split('\n').map(line=>line.split('\t'));
 for(const parent of ['/data','/data/local','/data/local/tmp']){
  const owner=rows.filter(row=>row.length===3&&(row[1]==='/'||parent===row[1]||parent.startsWith(row[1]+'/'))).sort((a,b)=>b[1].length-a[1].length)[0];
  assert.ok(owner,'original mount owner');
  const physical=path.join(owner[2],path.posix.relative(owner[1],parent));
  if(!fs.existsSync(physical)){
   const empty=path.join(temporary,'mounts',parent);fs.mkdirSync(empty,{recursive:true});
   text+='ro\t'+parent+'\t'+empty+'\n';rows.push(['ro',parent,empty]);
  }
 }
 const map=path.join(state,'policy-path-map');fs.writeFileSync(map,text,{mode:0o600});
 const env={...process.env,ANDROID_ROOT:'/system',ANDROID_DATA:'/data',KL_ANDROID_ID_DIRECTORY:ids,TMPDIR:temporary+'/'};delete env.CLASSPATH;
 const result=run(path.resolve(runner),['--inherit-env','--runtime',state,'--host-module',module,'--path-map',map,'--no-cache','/data/local/tmp/policy'],{env});
 t.diagnostic(result.stdout.trim());assert.match(result.stdout,/PASS original ART prctl/);
});
