import {copyOptional} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from '../binder-rpc/build.mjs';
import {readModuleList} from '../upstream/blueprint.mjs';

const nativeRoot=path.resolve(import.meta.dirname,'..');
const pin=JSON.parse(fs.readFileSync(path.join(nativeRoot,'source-pins.json'))).aospStatsd;

/** Preserve the complete original service-statsd module and its source groups.
 * The supplied framework already owns framework-statsd, StatsHelper, the AIDL
 * interfaces and modules-utils-build; none are compiled a second time. */
export function prepareStatsService({source,output}) {
  const exported=path.join(output,'source');
  fs.rmSync(exported,{recursive:true,force:true});
  exportSources(source,pin,['service'],exported);
  const directory=path.join(exported,'service');
  const files=new Set(),active=new Set();
  function sources(module){
    if(active.has(module))throw Error('Circular StatsD source group: '+module);
    active.add(module);
    for(const declaration of readModuleList(directory,module,'srcs')){
      if(declaration.startsWith(':'))sources(declaration.slice(1));
      else for(const relative of fs.globSync(declaration,{cwd:directory})){
        const file=path.join(directory,relative);
        if(fs.statSync(file).isFile())files.add(file);
      }
    }
    active.delete(module);
  }
  sources('service-statsd');
  const selected=[...files].sort();
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  const provenance={project:'platform/packages/modules/StatsD',url:'https://android.googlesource.com/platform/packages/modules/StatsD',revision:pin,module:'service-statsd',license:'Apache-2.0',files:selected.map(file=>path.relative(exported,file))};
  copyOptional(path.join(nativeRoot,'notices/aosp-framework/LICENSE'),path.join(notices,'LICENSE'));
  return {files:selected,notices};
}
