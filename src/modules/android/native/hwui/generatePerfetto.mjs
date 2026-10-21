import fs from'node:fs';import path from'node:path';import{execFileSync}from'node:child_process';

export async function generatePerfetto(ctx) {
const root=ctx.work,source=path.join(root,'module/source/perfetto'),gen=path.join(root,'module/gen/perfetto/external/perfetto');
const p=JSON.parse(fs.readFileSync(path.join(root,'perfetto-component-inputs.json'))),all=JSON.parse(fs.readFileSync(new URL('./perfetto-blueprint.json',import.meta.url))).modules;
const modules=new Map(all.map(m=>[m.properties.name,m]));
function expand(items=[]){return [...new Set(items.flatMap(file=>{if(!file.startsWith(':'))return[file];const m=modules.get(file.slice(1));if(!m||m.type!=='filegroup')throw Error('Missing original filegroup '+file);return expand(m.properties.srcs);} ))];}
fs.mkdirSync(gen,{recursive:true});const protoc=ctx.tools.protoc,tools=path.join(root,'tools/perfetto-generators-build'),done=new Set(),results=[];
for(const g of p.generators){
 if(g.name==='perfetto_src_base_version_gen_h'){
  const file=path.join(root,'module/gen/include/perfetto_version.gen.h');
  execFileSync(ctx.tools.python,[path.join(source,'tools/write_version_header.py'),'--no_git','--changelog='+path.join(source,'CHANGELOG'),'--cpp_out='+file]);
  results.push({name:g.name,outputs:[file],success:fs.existsSync(file)});continue;
 }
 const plugin=g.tools.find(t=>t!=='aprotoc'),options=g.cmd.match(/--plugin_out=([^: ]*):/)?.[1];
 if(!plugin||!options)throw Error('Unsupported original Perfetto generator: '+g.name);
 const files=expand(g.srcs).filter(file=>file.endsWith('.proto'));
 const key=JSON.stringify([plugin,options,files]);
 const outputs=g.out.map(file=>path.join(root,'module/gen/perfetto',file));
 if(!done.has(key)){
  execFileSync(protoc,['--proto_path='+source,'--plugin=protoc-gen-plugin='+ctx.tools.perfettoPlugins[plugin],'--plugin_out='+options+':'+gen,...files],{cwd:source,maxBuffer:4e6});done.add(key);
 }
 if(outputs.some(file=>!fs.existsSync(file)))throw Error('Original generator missed output for '+g.name);
 results.push({name:g.name,plugin,options,inputs:files.length,outputs:outputs.length,success:true});
}
fs.writeFileSync(path.join(root,'perfetto-generation-result.json'),JSON.stringify({sourceRevision:'94a71474cc396db2b0fcff6755b8f2d3f7e304d0',protoc,executed:done.size,results},null,2));
console.log(JSON.stringify({rules:results.length,distinctPluginInvocations:done.size,success:results.every(r=>r.success)}));

}
