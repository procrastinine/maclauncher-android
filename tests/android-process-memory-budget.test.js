const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),{spawnSync}=require('node:child_process');
test('original Android Process memory queries share the configured native guest budget',t=>{
 const runtime=process.env.KLEPTON_NATIVE_RUNTIME,home=process.env.JAVA_HOME;
 if(process.platform!=='darwin'||!runtime||!home)return t.skip('Requires macOS, a built Android runtime and Java21');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-memory-budget-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const source=path.resolve(__dirname,'../src/modules/android/native'),fixture=path.join(__dirname,'fixtures/android-process-memory-budget'),installed=process.env.ANDROID_MEMORY_BUDGET_RUNTIME==='1';
 const classpath=[temporary,...['android-host.jar','android-framework.jar'].map(file=>path.resolve(runtime,file))].join(path.delimiter);
 const run=(command,args,extra={})=>{const r=spawnSync(command,args,{encoding:'utf8',timeout:20000,env:{...process.env,...extra}});assert.equal(r.error,undefined,r.error?.message);assert.equal(r.status,0,r.stdout+r.stderr);return r.stdout;};
 const library=path.join(temporary,'memory-budget.dylib');
 run('clang',['-arch','arm64','-mmacosx-version-min=13.0','-std=gnu11','-dynamiclib','-Wall','-Wextra','-I'+source,'-I'+path.join(home,'include'),'-I'+path.join(home,'include/darwin'),'-Wl,-undefined,dynamic_lookup',...installed?[]:[path.join(source,'framework-process.c')],path.join(fixture,'memory-budget.c'),path.resolve(runtime,'native/libklepton-jvm.dylib'),'-Wl,-rpath,'+path.resolve(runtime,'native'),'-o',library]);
 run(path.join(home,'bin/javac'),['--release','21','-cp',classpath,'-d',temporary,path.join(fixture,'MemoryBudgetProbe.java')]);
 for(const budget of ['384','768','2147483647']){
  const output=run(path.join(home,'bin/java'),['-Xcheck:jni','-cp',classpath,'MemoryBudgetProbe',path.resolve(runtime),library,...installed?['runtime']:[]],{KL_MEM_TOTAL_MB:budget,TMPDIR:temporary});
  assert.match(output,/PASS original Process\/ActivityManager, Bridge and proc share configured budget=[0-9]+ and live available memory/);t.diagnostic(output.trim());
 }
});
