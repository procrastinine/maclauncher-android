import {copyOptional} from './upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {exportSources} from './binder-rpc/build.mjs';
import {readModuleList} from './upstream/blueprint.mjs';
import {cachedBuildStep} from './upstream/build-step.mjs';
import {javaToolInputs,javaToolEnvironment} from './framework-build.mjs';

/** Compile the original supplemental implementations against the supplied full
 * framework. Source groups remain owned by their original Blueprint files;
 * neither SDK API stubs nor copies of framework-owned dependencies are added. */
export async function buildFrameworkModules({root,build,jdk,framework,apacheHttpSource,statsdSource}){
  const pinsFile=path.join(root,'source-pins.json'),pins=JSON.parse(fs.readFileSync(pinsFile));
  const suppliers=[
    {name:'org.apache.http.legacy',source:apacheHttpSource,pin:pins.aospApacheHttp,
      project:'platform/external/apache-http',trees:['Android.bp','src','android/src','NOTICE'],directory:'.'},
    {name:'service-statsd',source:statsdSource,pin:pins.aospStatsd,
      project:'platform/packages/modules/StatsD',trees:['service'],directory:'service'},
  ];
  const archives=[],notices=[];
  for(const supplier of suppliers){
    const directory=path.join(build,'framework-modules',supplier.name),source=path.join(directory,'source');
    exportSources(supplier.source,supplier.pin,supplier.trees,source);
    const moduleDirectory=path.join(source,supplier.directory),declarations=new Set(),active=new Set(),files=new Set();
    function collect(name){
      if(active.has(name))throw Error('Circular original framework source group: '+name);
      active.add(name);declarations.add(path.join(moduleDirectory,'Android.bp'));
      for(const input of readModuleList(moduleDirectory,name,'srcs')){
        if(input.startsWith(':'))collect(input.slice(1));
        else for(const relative of fs.globSync(input,{cwd:moduleDirectory})){
          const file=path.join(moduleDirectory,relative);if(fs.statSync(file).isFile())files.add(file);
        }
      }
      active.delete(name);
    }
    collect(supplier.name);
    const selected=[...files].sort(),classes=path.join(directory,'classes'),archive=path.join(directory,supplier.name+'.jar');
    const notice=path.join(directory,'notices'),license=path.join(root,'notices/aosp-framework/LICENSE');
    await cachedBuildStep({cache:path.join(directory,'reuse.json'),
      inputs:[fileURLToPath(import.meta.url),pinsFile,...javaToolInputs(jdk),framework.input.file,
        ...declarations,...selected],
      trees:[classes],files:[archive],
      key:{name:supplier.name,revision:supplier.pin,release:21,files:selected,environment:javaToolEnvironment()},build(){
        execFileSync(path.join(jdk,'bin/javac'),['--release','21','-cp',framework.input.file,'-d',classes,...selected],{stdio:'inherit'});
        execFileSync(path.join(jdk,'bin/jar'),['--create','--file',archive,'--date','1980-01-01T00:00:02Z','-C',classes,'.'],{stdio:'inherit'});
        copyOptional(license,path.join(notice,'LICENSE'));
        if(supplier.name==='org.apache.http.legacy')copyOptional(path.join(source,'NOTICE'),path.join(notice,'NOTICE'));
      }});
    archives.push(archive);notices.push({name:supplier.name,path:notice});
  }
  return {archives,notices};
}
