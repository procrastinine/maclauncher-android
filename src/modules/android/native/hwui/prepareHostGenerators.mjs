import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {parseBlueprint} from '../upstream/blueprint.mjs';

export async function prepareHostGenerators(ctx) {

const root=ctx.work,source=path.join(root,'module/source');
const parser=ctx.tools.blueprintParser;
const output=path.join(root,'tools/generators'),generated=path.join(output,'generated');
fs.mkdirSync(generated,{recursive:true});
const protoc=ctx.tools.protoc;
const standard=ctx.tools.protocInclude;
const sysprop=path.join(source,'sysprop');
execFileSync(protoc,['-I'+sysprop,'--cpp_out='+generated,path.join(sysprop,'sysprop.proto')]);

const protoLogging=path.join(source,'proto_logging');
const mappings=['-Iframeworks/proto_logging='+protoLogging,'-I'+standard];
const atom='frameworks/proto_logging/stats/atoms.proto';
const dependency=path.join(generated,'atoms.d');
execFileSync(protoc,[...mappings,'--cpp_out='+generated,'--dependency_out='+dependency,atom]);
// The original compiler exports its parsed import closure in Make syntax.
// All upstream logical paths are whitespace-free, checked before reuse.
const dependencies=fs.readFileSync(dependency,'utf8').replaceAll('\\\n','').split(':').slice(1).join(':').trim().split(/\s+/).filter(Boolean);
// Original libstats_proto_host also registers extension descriptors.  The
// requested hwui generator consumes the original coregraphics extension group;
// these are not imports of atoms.proto and must be retained explicitly.
const coregraphics=fs.globSync('stats/atoms/coregraphics/*.proto',{cwd:protoLogging}).map(p=>'frameworks/proto_logging/'+p);
for(const file of coregraphics){
 const output=path.join(generated,path.basename(file)+'.d');
 execFileSync(protoc,[...mappings,'--cpp_out='+generated,'--dependency_out='+output,file]);
 dependencies.push(...fs.readFileSync(output,'utf8').replaceAll('\\\n','').split(':').slice(1).join(':').trim().split(/\s+/).filter(Boolean));
}
if(dependencies.some(file=>!file.endsWith('.proto')||file.includes('\\')))throw Error('Unexpected protoc dependency path');
const logical=dependencies.map(file=>{
 if(path.isAbsolute(file)){
  if(file.startsWith(protoLogging+path.sep))return 'frameworks/proto_logging/'+path.relative(protoLogging,file).split(path.sep).join('/');
  if(file.startsWith(standard+path.sep))return path.relative(standard,file).split(path.sep).join('/');
  throw Error('Unexpected source owner in protoc import closure: '+file);
 }
 return file;
});
const atomSources=[...new Set(logical.filter(f=>!f.startsWith('google/protobuf/')))];
execFileSync(protoc,[...mappings,'--cpp_out='+generated,...atomSources]);

const modules=(file)=>parseBlueprint(fs.readFileSync(file,'utf8'),{file,parser}).modules;
const choose=(list,name)=>{const found=list.filter(m=>m.properties.name===name);if(found.length!==1)throw Error('Missing original module '+name);return found[0].properties;};
const syspropModules=modules(path.join(sysprop,'Android.bp'));
const syspropFiles=[...choose(syspropModules,'sysprop-defaults').srcs,...choose(syspropModules,'sysprop_cpp').srcs].filter(f=>f.endsWith('.cpp')).map(f=>path.join(sysprop,f));
syspropFiles.push(path.join(generated,'sysprop.pb.cc'));
const serializer=path.join(source,'core/property_service/libpropertyinfoserializer');
const propertyParser=path.join(source,'core/property_service/libpropertyinfoparser');
for(const [dir,name]of [[serializer,'libpropertyinfoserializer'],[propertyParser,'libpropertyinfoparser']])
 for(const file of choose(modules(path.join(dir,'Android.bp')),name).srcs)syspropFiles.push(path.join(dir,file));

const statsDir=path.join(protoLogging,'stats/stats_log_api_gen');
const statsModules=modules(path.join(statsDir,'Android.bp'));
const statsFiles=[...choose(statsModules,'stats-log-api-gen-defaults').srcs,...choose(statsModules,'stats-log-api-gen').srcs].map(f=>path.join(statsDir,f));
const generatedAtoms=atomSources.map(file=>path.join(generated,file.replace(/\.proto$/,'.pb.cc')));
const protobuf=path.join(source,'protobuf');
const foundation=ctx.tools.foundationHost;
const fmt=path.join(ctx.sources.fmt,'include');
const hostProtobuf=ctx.tools.hostProtobufRoot;
const includes=[generated,path.join(source,'base/include'),path.join(protobuf,'src'),path.join(sysprop,'include'),
 path.join(serializer,'include'),path.join(propertyParser,'include'),statsDir];
const quote=text=>'"'+text.replaceAll('\\','/').replaceAll('"','\\"')+'"';
const cmake=[
 'cmake_minimum_required(VERSION 3.16)','project(hwui_generators LANGUAGES CXX)','set(CMAKE_CXX_STANDARD 20)',
 'add_compile_options(-Wno-deprecated-declarations)',
 'include_directories('+includes.map(quote).join(' ')+')',
 'add_executable(sysprop_cpp '+syspropFiles.map(quote).join(' ')+')',
 'add_library(stats_proto OBJECT '+generatedAtoms.map(quote).join(' ')+')',
 'add_executable(stats-log-api-gen '+statsFiles.map(quote).join(' ')+')',
 ...['sysprop_cpp','stats-log-api-gen'].map(name=>'target_include_directories('+name+' PRIVATE '+quote(fmt)+')'),
 'target_compile_definitions(stats-log-api-gen PRIVATE WITH_VENDOR JAVA_INCLUDE_SRCS_DIR="java/" CC_INCLUDE_HDRS_DIR="cc_hdrs/" CC_INCLUDE_SRCS_DIR="cc_srcs/")',
 'target_link_libraries(sysprop_cpp PRIVATE '+[foundation,path.join(hostProtobuf,'libprotobuf.a')].map(quote).join(' ')+' z)',
 'target_link_libraries(stats-log-api-gen PRIVATE stats_proto '+[foundation,path.join(hostProtobuf,'libprotoc.a'),path.join(hostProtobuf,'libprotobuf.a')].map(quote).join(' ')+' z)',
 ];
fs.writeFileSync(path.join(output,'CMakeLists.txt'),cmake.join('\n')+'\n');
fs.writeFileSync(path.join(root,'generator-inputs.json'),JSON.stringify({syspropFiles,statsFiles,atomSources,coregraphics,foundation,protoc,scope:'Complete original host tool code, original compiler-derived base atom import closure and original coregraphics extension group for hwui; object linkage preserves extension registration. No runtime APIs or schemas removed.'},null,2)+'\n');
console.log(JSON.stringify({syspropSources:syspropFiles.length,statsSources:statsFiles.length,atomProtoSources:atomSources.length}));

}
