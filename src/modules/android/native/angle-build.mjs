import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import os from 'node:os';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {prepareAngleRgb565} from './angle-source.mjs';
import {prepareAngleFramebufferFlip} from './angle-flip-source.mjs';
import {prepareAngleVulkan} from './angle-vulkan-source.mjs';
import {prepareAngleIdentity} from './angle-identity-source.mjs';
import {prepareSourceTree,writeChanged} from './upstream/source-files.mjs';
import {nativePathFlags} from './upstream/native-paths.mjs';

const root=path.dirname(fileURLToPath(import.meta.url));
const pins=JSON.parse(fs.readFileSync(path.join(root,'angle-source.json')));
const options={};
for(let i=2;i<process.argv.length;i+=2){const key=process.argv[i],value=process.argv[i+1];if(!key?.startsWith('--')||!value)throw Error('Expected --option value pairs');options[key.slice(2)]=['backend','jobs'].includes(key.slice(2))?value:path.resolve(value);}
for(const key of ['source','dependencies','output','gn','ninja','clang','python'])if(!options[key])throw Error('Missing --'+key);
const backend=options.backend||'metal';
if(!['metal','metal-vulkan'].includes(backend))throw Error('Expected ANGLE backend metal or metal-vulkan');
const jobs=options.jobs===undefined?Math.min(4,os.availableParallelism()):Number(options.jobs);
if(!Number.isSafeInteger(jobs)||jobs<1||jobs>os.availableParallelism())throw Error('Invalid ANGLE build job count');
const dependencies=pins.dependencies.filter(dependency=>!dependency.backends||dependency.backends.includes(backend));
const read=(cmd,args)=>execFileSync(cmd,args,{encoding:'utf8'}).trim();
const run=(cmd,args,extra={})=>execFileSync(cmd,args,{stdio:'inherit',...extra});
const check=(directory,revision)=>{if(read('git',['-C',directory,'rev-parse','HEAD'])!==revision)throw Error('Unexpected source revision: '+directory);};
check(options.source,pins.revision);
for(const dependency of dependencies)check(path.join(options.dependencies,path.basename(dependency.directory)),dependency.commit);
fs.mkdirSync(options.output,{recursive:true});
const source=path.join(options.output,'source'),output=path.join(options.output,'build');
const copy=(from,to)=>fs.cpSync(from,to,{recursive:true,filter:file=>path.basename(file)!=='.git'});
prepareSourceTree(source,temporary=>{
  copy(options.source,temporary);
  for(const dependency of [...dependencies].sort((a,b)=>a.name.split('/').length-b.name.split('/').length))copy(path.join(options.dependencies,path.basename(dependency.directory)),path.join(temporary,dependency.name));
  prepareAngleIdentity(options.source,temporary,pins.revision);
  prepareAngleRgb565(options.source,temporary);
  prepareAngleFramebufferFlip(options.source,temporary);
  if(backend==='metal-vulkan')prepareAngleVulkan(options.source,temporary);
  writeChanged(path.join(temporary,'build/config/gclient_args.gni'),'checkout_angle_internal=false\ncheckout_angle_mesa=false\ncheckout_angle_restricted_traces=false\ngenerate_location_tags=false\n');
  // Apply the same path contract to original and adapted compilation units.
  // GN's relative source arguments alone do not cover absolute header paths.
  const compiler=path.join(temporary,'build/config/compiler/BUILD.gn');
  const original=fs.readFileSync(compiler,'utf8');
  const before='config("compiler") {\n  asmflags = []\n  cflags = []';
  if(original.split(before).length!==2)throw Error('ANGLE compiler configuration source changed');
  const portable=nativePathFlags({source,build:options.output,roots:[
    [options.source,'/src/angle'],[options.dependencies,'/src/angle-dependencies']]});
  writeChanged(compiler,original.replace(before,'config("compiler") {\n  asmflags = []\n  cflags = '+JSON.stringify(portable)));
});
fs.mkdirSync(output,{recursive:true});
const clangVersion=path.basename(read(path.join(options.clang,'bin/clang'),['--print-resource-dir']));
const args={is_debug:false,target_cpu:'arm64',is_component_build:false,angle_enable_metal:true,angle_enable_vulkan:false,angle_enable_gl:false,angle_enable_null:false,angle_build_tests:false,angle_build_all:false,build_angle_deqp_tests:false,angle_has_histograms:false,use_custom_libcxx:false,use_lld:false,clang_base_path:options.clang,clang_version:clangVersion,treat_warnings_as_errors:false,use_siso:false,enable_rust:false,mac_deployment_target:'13.0',angle_build_vulkan_system_info:false,angle_enable_wgpu:false};
if(backend==='metal-vulkan')Object.assign(args,{angle_enable_vulkan:true,angle_shared_libvulkan:true,angle_use_custom_libvulkan:false,angle_enable_swiftshader:false,angle_enable_vulkan_validation_layers:false,angle_enable_vulkan_api_dump_layer:false,angle_build_vulkan_system_info:true});
writeChanged(path.join(output,'args.gn'),Object.entries(args).map(([key,value])=>key+'='+JSON.stringify(value)).join('\n')+'\n');
const env={...process.env,ANGLE_UPSTREAM_HASH:pins.revision.slice(0,12),PATH:path.dirname(options.python)+path.delimiter+process.env.PATH};
run(options.gn,['gen',output,'--root='+source],{env});
run(options.ninja,['-C',output,'-j',String(jobs),'libEGL','libGLESv2'],{env});
const digest=data=>crypto.createHash('sha256').update(data).digest('hex');
const files=[];
const publication=fs.mkdtempSync(path.join(options.output,'.publication-'));
try{
  for(const name of ['libEGL.dylib','libGLESv2.dylib']){
    const file=path.join(publication,name);
    // Keep link products in the compiler cache. Published libraries exclude
    // Mach-O OSO records naming local object paths; exported symbols remain.
    run('/usr/bin/strip',['-S','-o',file,path.join(output,name)]);
    run('/usr/bin/codesign',['--force','--sign','-',file]);
    const data=fs.readFileSync(file);writeChanged(path.join(options.output,name),data);
    files.push({path:name,size:data.length,sha256:digest(data),architectures:['arm64'],minimumMacOS:'13.0'});
  }
}finally{fs.rmSync(publication,{recursive:true,force:true});}
const stock=JSON.parse(fs.readFileSync(path.join(root,'angle-binary.json')));
const notices=[...stock.notices];
prepareSourceTree(path.join(options.output,'notices'),temporary=>{
  for(const notice of stock.notices)writeChanged(path.join(temporary,notice.file),fs.readFileSync(path.join(root,'notices/angle',notice.file)));
  const suppliers=[['JsonCpp','third_party/jsoncpp/source/LICENSE','https://github.com/open-source-parsers/jsoncpp'],['RapidJSON','third_party/rapidjson/src/license.txt','https://github.com/Tencent/rapidjson']];
  if(backend==='metal-vulkan'){
    suppliers.push(['VulkanMemoryAllocator','third_party/vulkan_memory_allocator/LICENSE.txt','https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator']);
    for(const license of fs.readdirSync(path.join(source,'third_party/vulkan-headers/src/LICENSES')))
      suppliers.push(['Vulkan-Headers-'+license.replace(/\.[^.]+$/,''),'third_party/vulkan-headers/src/LICENSES/'+license,'https://github.com/KhronosGroup/Vulkan-Headers']);
  }
  for(const [name,file,url] of suppliers){const data=fs.readFileSync(path.join(source,file)),notice={name,url,file:name+'.txt',size:data.length,sha256:digest(data)};notices.push(notice);writeChanged(path.join(temporary,notice.file),data);}
});
const adaptations=['angle-source.mjs','angle-flip-source.mjs','angle-flip-source.json','angle-identity-source.mjs','angle-sync-source.mjs','angle-fence-completion.h','angle-current-source.mjs','angle-debug-source.mjs','angle-debug-callback.h','angle-blob-source.mjs','angle-blob-callback.h','angle-dither-source.mjs',...(backend==='metal-vulkan'?['angle-vulkan-source.mjs','angle-vulkan-image-source.mjs']:[])].map(file=>({file,sha256:digest(fs.readFileSync(path.join(root,file)))}));
const internalShaders=fs.readFileSync(path.join(output,'obj/src/libANGLE/renderer/metal/angle_metal_backend.ninja'),'utf8').includes('-DANGLE_METAL_HAS_PREBUILT_INTERNAL_SHADERS')?'embedded-metallib':'runtime-source';
const programBinaryVersion=fs.readFileSync(path.join(output,'gen/angle/ANGLEShaderProgramVersion.h'),'utf8').match(/^#define ANGLE_PROGRAM_VERSION "([a-f0-9]+)"$/m)?.[1];
if(!programBinaryVersion)throw Error('Missing generated ANGLE program binary version');
const manifest={schemaVersion:1,publisher:'MacLauncher',version:pins.revision.slice(0,12)+'-'+backend+'-12',angleRevision:pins.revision,programBinaryVersion,source:pins.repository,dependencies,adaptations,buildArguments:args,internalMetalShaders:internalShaders,files,notices};
writeChanged(path.join(options.output,'angle-manifest.json'),JSON.stringify(manifest,null,2)+'\n');
console.log('ANGLE '+backend+' provider: '+options.output);
