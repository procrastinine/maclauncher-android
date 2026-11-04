import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {prepareCoreApi} from '../src/modules/android/art/core-api.mjs';
import {compileArtSources} from '../src/modules/android/art/build-java.mjs';

test('ART core compilation retains hidden sibling classes in original boot packages',async t=>{
 const api=process.env.ANDROID_ART_API_JAR,coreSystemModules=process.env.ANDROID_ART_CORE_JAR;
 if(!api||!coreSystemModules)return t.skip('Requires the original Android framework and core declaration archives');
 const jdk=process.env.ANDROID_JAVA_HOME||execFileSync('/usr/libexec/java_home',['-v','21'],{encoding:'utf8'}).trim();
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-core-declarations-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const core=await prepareCoreApi({output:path.join(root,'core'),jdk,coreSystemModules});
 const source=path.join(root,'HiddenCoreProbe.java'),classes=path.join(root,'classes');fs.mkdirSync(classes);
 fs.writeFileSync(source,`import dalvik.system.SocketTagger;
import android.system.StructCapUserHeader;
class HiddenCoreProbe {
 SocketTagger tagger=new SocketTagger(){
  @Override public void tag(java.io.FileDescriptor descriptor){}
  @Override public void untag(java.io.FileDescriptor descriptor){}
 };
 StructCapUserHeader capabilities;
}
`);
 compileArtSources({jdk,core,api,sharedClasses:classes,classes,sources:[source]});
 assert(fs.statSync(path.join(classes,'HiddenCoreProbe.class')).isFile());
});
