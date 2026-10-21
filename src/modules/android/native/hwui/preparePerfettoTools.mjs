import fs from 'node:fs';import path from 'node:path';

export async function preparePerfettoTools(ctx) {
const root=ctx.work,source=path.join(root,'module/source/perfetto'),out=path.join(root,'tools/perfetto-generators');
const all=JSON.parse(fs.readFileSync(new URL('./perfetto-blueprint.json',import.meta.url))).modules;const mods=new Map(all.map(m=>[m.properties.name,m]));
const get=name=>{const m=mods.get(name);if(!m)throw Error('Missing original module '+name);return m.properties;};
function expand(items=[],seen=new Set()){return [...new Set(items.flatMap(p=>{if(!p.startsWith(':'))return [p];const name=p.slice(1);if(seen.has(name))throw Error('Recursive module '+name);const m=mods.get(name);if(!m)throw Error('Missing original filegroup '+name);if(m.type!=='filegroup')throw Error('Not a filegroup: '+name);return expand(m.properties.srcs,new Set([...seen,name]));}))];}
fs.mkdirSync(out,{recursive:true});
const plugins=['perfetto_src_protozero_protoc_plugin_cppgen_plugin','protozero_plugin','ipc_plugin'];
const common=expand(get(plugins[0]).srcs.filter(p=>p.startsWith(':'))).filter(p=>/\.(cc|cpp|c)$/.test(p));
const q=p=>'"'+p.replaceAll('\\','/').replaceAll('"','\\"')+'"';
const protobuf=path.join(root,'module/source/protobuf'),hostProtobuf=ctx.tools.hostProtobufRoot;
const includes=[source,path.join(source,'include'),path.join(source,'include/perfetto/base/build_configs/android_tree'),path.join(source,'src/profiling/memory/include'),path.join(protobuf,'src')];
const lines=['cmake_minimum_required(VERSION 3.16)','project(perfetto_generators LANGUAGES CXX)','set(CMAKE_CXX_STANDARD 17)',
 'add_compile_options(-O2 -Wno-deprecated-declarations)',
 'include_directories('+includes.map(q).join(' ')+')',
 'add_library(perfetto_plugin_base STATIC '+common.map(p=>q(path.join(source,p))).join(' ')+')'];
for(const name of plugins){const m=get(name),files=m.srcs.filter(p=>!p.startsWith(':')&&/\.(cc|cpp|c)$/.test(p));lines.push('add_executable('+name+' '+files.map(p=>q(path.join(source,p))).join(' ')+')',
 'target_compile_definitions('+name+' PRIVATE GOOGLE_PROTOBUF_NO_RTTI GOOGLE_PROTOBUF_NO_STATIC_INITIALIZER)',
 'target_link_libraries('+name+' PRIVATE perfetto_plugin_base '+[path.join(hostProtobuf,'libprotoc.a'),path.join(hostProtobuf,'libprotobuf.a')].map(q).join(' ')+' z)');}
fs.writeFileSync(path.join(out,'CMakeLists.txt'),lines.join('\n')+'\n');
const target=get('libperfetto_client_experimental'),files=expand(target.srcs.filter(p=>!p.startsWith(':')||mods.get(p.slice(1))?.type==='filegroup'));
const generators=[...new Set([...target.srcs.filter(p=>p.startsWith(':')&&mods.get(p.slice(1))?.type==='genrule').map(p=>p.slice(1)),...target.generated_headers])];
fs.writeFileSync(path.join(root,'perfetto-component-inputs.json'),JSON.stringify({plugins,pluginBase:common,target,sources:files,generators:generators.map(get)},null,2)+'\n');
console.log(JSON.stringify({plugins,pluginBase:common.length,sources:files.filter(p=>/\.(cc|cpp|c)$/.test(p)).length,generators:generators.length}));

}
