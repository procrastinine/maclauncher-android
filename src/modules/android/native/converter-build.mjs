import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {cachedBuildStep} from './upstream/build-step.mjs';
import {javaSources,javaToolInputs,javaToolEnvironment} from './framework-build.mjs';
import {adaptTranslator,adaptConverter,adaptInitRepointPlan} from './converter-source.mjs';

export async function buildConverter({root,build,output,jdk,checkout,apksig,asmClasspath}){
  const converter=path.join(checkout,'build/dex2jvm.jar'),java=path.join(jdk,'bin/java'),javac=path.join(jdk,'bin/javac'),jar=path.join(jdk,'bin/jar');
  const upstream=path.join(checkout,'src/main/java/io/github/kksimp/dex2jvm');
  const classes=path.join(build,'converter'),generated=path.join(build,'converter-sources');
  const archive=path.join(build,'converter-payload/converter-overlay.jar'),apiHeaders=path.join(build,'android-api-headers.jar');
  const sources=javaSources(path.join(root,'converter')),portability=javaSources(path.join(root,'portability'));
  const names=['GuestRepoint','Translator','DexConverter','InitRepointPlan'];
  const run=(command,args,extra={})=>execFileSync(command,args,{stdio:'inherit',...extra});
  await cachedBuildStep({cache:path.join(build,'converter.reuse.json'),
    inputs:[fileURLToPath(import.meta.url),fileURLToPath(new URL('./converter-source.mjs',import.meta.url)),
      fileURLToPath(new URL('./framework-build.mjs',import.meta.url)),...javaToolInputs(jdk),...sources,...portability,
      ...names.map(name=>path.join(upstream,name+'.java')),converter,apksig,...asmClasspath.split(path.delimiter)],
    trees:[classes,generated],files:[archive,apiHeaders],key:{release:21,asmClasspath,environment:javaToolEnvironment()},build(){
      const original=fs.readFileSync(path.join(upstream,'GuestRepoint.java'),'utf8'),anchor='        String exitTarget = Options.exitRedirect;';
      if(original.split(anchor).length!==2)throw Error('Converter native-load insertion point changed');
      fs.writeFileSync(path.join(generated,'GuestRepoint.java'),original.replace(anchor,'        Target nativeTarget = NativeLoadRepoint.lookup(isStatic, isVirtual, owner, name, desc);\n        if (nativeTarget != null) return nativeTarget;\n'+anchor));
      for(const [name,adapt] of [['Translator',adaptTranslator],['DexConverter',adaptConverter],['InitRepointPlan',adaptInitRepointPlan]])
        fs.writeFileSync(path.join(generated,name+'.java'),adapt(fs.readFileSync(path.join(upstream,name+'.java'),'utf8')));
      run(javac,['--release','21','-cp',[converter,apksig,asmClasspath].join(path.delimiter),'-d',classes,
        ...names.map(name=>path.join(generated,name+'.java')),...portability,...sources]);
      // ASM stays in conversion subprocesses; it never enters the game JVM.
      for(const file of asmClasspath.split(path.delimiter))run(jar,['--extract','--file',file,'org/objectweb/asm'],{cwd:classes});
      run(jar,['--create','--file',archive,'--date','1980-01-01T00:00:02Z','-C',classes,'.']);
      run(java,['-cp',classes+path.delimiter+converter,'io.github.kksimp.dex2jvm.ExportApiHeaders',apiHeaders]);
    }});
  fs.copyFileSync(converter,path.join(output,'dex2jvm.jar'),fs.constants.COPYFILE_FICLONE);
  fs.copyFileSync(archive,path.join(output,'converter-overlay.jar'),fs.constants.COPYFILE_FICLONE);
  return {apiHeaders};
}
