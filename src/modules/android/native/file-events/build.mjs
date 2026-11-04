import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';
import {adaptFileEventSocketFlow} from './socket-flow.mjs';
const root=import.meta.dirname;
function replace(text,from,to){if(text.split(from).length!==2)throw Error('File event supplier boundary changed: '+from);return text.replace(from,to);}
export function buildFileEvents({checkout,output,jdk}) {
  const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));
  const source=path.join(output,'source');
  const paths=execFileSync('git',['-C',checkout,'ls-files','-z'],{encoding:'utf8'}).split('\0').filter(Boolean);
  exportSources(checkout,pins.libinotifyKqueue,paths,source);
  fs.copyFileSync(path.join(root,'darwin-config.h'),path.join(source,'config.h'));
  let header=fs.readFileSync(path.join(source,'worker.h'),'utf8');
  header=replace(header,'    WCMD_NONE = 0,','    WCMD_HOST_EVENT = -1,\n    WCMD_NONE = 0,');
  header=replace(header,'    union {','    union {\n        const klj_file_event *host_event;');
  fs.writeFileSync(path.join(source,'worker.h'),'#include '+JSON.stringify(path.join(root,'operations.h'))+'\n'+header);
  let controller=fs.readFileSync(path.join(source,'controller.c'),'utf8');
  fs.writeFileSync(path.join(source,'controller.c'),controller+'\n'+fs.readFileSync(path.join(root,'controller.inc'),'utf8'));
  let worker=fs.readFileSync(path.join(source,'worker-thread.c'),'utf8');
  worker=replace(worker,'void\nprocess_command (struct worker *wrk, struct worker_cmd *cmd)',fs.readFileSync(path.join(root,'worker.inc'),'utf8')+'\nvoid\nprocess_command (struct worker *wrk, struct worker_cmd *cmd)');
  worker=replace(worker,'    case WCMD_ADD:','    case WCMD_HOST_EVENT:\n        process_host_event(wrk, cmd->cmd.host_event);\n        cmd->retval = 0;\n        break;\n    case WCMD_ADD:');
  worker=adaptFileEventSocketFlow(worker);
  fs.writeFileSync(path.join(source,'worker-thread.c'),worker);
  let watches=fs.readFileSync(path.join(source,'inotify-watch.c'),'utf8');
  watches=watches.replaceAll('    iw->flags = flags;','    klj_file_event_interest_change(iw->flags, flags);\n    iw->flags = flags;');
  watches=replace(watches,'    free (iw);','    klj_file_event_interest_change(iw->flags, 0);\n    free (iw);');
  fs.writeFileSync(path.join(source,'inotify-watch.c'),'extern void klj_file_event_interest_change(unsigned, unsigned);\n'+watches);
  const makefile=fs.readFileSync(path.join(source,'Makefile.am'),'utf8');
  const family=makefile.match(/libinotify_la_SOURCES = ([\s\S]*?)\n\n/)[1].replaceAll('\\',' ').split(/\s+/).filter(file=>file.endsWith('.c'));
  const inputs=[...family.map(file=>path.join(source,file)),path.join(root,'operations.c'),path.join(root,'jdk.c')];
  const objects=[];
  for(const file of inputs){
    const object=path.join(output,path.basename(file)+'.o');
    execFileSync('clang',[...nativePathFlags({source,build:output,roots:[[jdk,'/src/jdk']]}),'-std=gnu11','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Wno-unused-parameter','-I'+source,'-I'+path.join(jdk,'include'),'-I'+path.join(jdk,'include/darwin'),'-c',file,'-o',object],{stdio:'inherit'});
    objects.push(object);
  }
  return {
    objects,
    include:source
  };
}
