import fs from 'node:fs';import path from 'node:path';import{execFileSync}from'node:child_process';
export function expandPolicy({output:out,source}) {
fs.mkdirSync(out,{recursive:true});
const go=fs.readFileSync(source+'/build/soong/policy.go','utf8');
const order=[...go.slice(go.indexOf('var policyConfOrder'),go.indexOf('\n}',go.indexOf('var policyConfOrder'))).matchAll(/"([^"]+)"/g)].map(m=>m[1]);
if(!order.length)throw Error('Original policy build order missing');
const files=[];
for(const group of order){
 if(group==='flagging_macros'){files.push(source+'/flagging/flagging_macros');continue;}
 const names=new Set(group.split('|').flatMap(p=>p==='*.te'?['public','private'].flatMap(dir=>fs.readdirSync(source+'/'+dir).filter(n=>n.endsWith('.te'))):[p]));
 for(const name of [...names].sort())for(const dir of ['public','private']){const p=source+'/'+dir+'/'+name;if(fs.existsSync(p))files.push(p);}
}
const definitions={mls_num_sens:1,mls_num_cats:1024,target_arch:'arm64',target_with_asan:false,target_with_dexpreopt:false,target_with_native_coverage:false,target_build_variant:'user',target_full_treble:true,target_compatible_property:true,target_treble_sysprop_neverallow:true,target_enforce_sysprop_owner:true,target_exclude_build_test:true,target_requires_insecure_execmem_for_swiftshader:false,target_enforce_debugfs_restriction:true,target_recovery:false,target_board_api_level:202504};
// This platform has no Android VM/device services. Keep their original release
// policy branches disabled; their Android public APIs are retained separately.
const flags=new Set();for(const file of [...files,source+'/private/service_contexts'])for(const m of fs.readFileSync(file,'utf8').matchAll(/is_flag_(?:enabled|disabled)\(\s*(RELEASE_\w+)/g))flags.add(m[1]);
for(const flag of flags)definitions['target_flag_'+flag]=false;
const args=['--fatal-warnings',...Object.entries(definitions).map(([k,v])=>'-D'+k+'='+v),...files];
const policy=execFileSync('/usr/bin/m4',args,{encoding:'utf8',maxBuffer:16*1024*1024});
fs.writeFileSync(out+'/policy-expanded.conf',policy);
const service=execFileSync('/usr/bin/m4',['--fatal-warnings',...Object.entries(definitions).map(([k,v])=>'-D'+k+'='+v),source+'/flagging/flagging_macros',source+'/private/service_contexts'],{encoding:'utf8'});fs.writeFileSync(out+'/service-contexts-expanded',service);
fs.writeFileSync(out+'/policy-inputs.json',JSON.stringify({source,files,definitions},null,2)+'\n');
return {files,definitions};
}
