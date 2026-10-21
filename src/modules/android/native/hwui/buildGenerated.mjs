import fs from'node:fs';import path from'node:path';import{spawnSync}from'node:child_process';

export async function buildGenerated(ctx) {
const root=ctx.work,output=path.join(root,'module/components/generated'),core=JSON.parse(fs.readFileSync(path.join(root,'core-object-inputs.json'))),pool={compiler:ctx.tools.clangxx};fs.mkdirSync(path.join(output,'objects'),{recursive:true});
const generation=JSON.parse(fs.readFileSync(path.join(root,'release-flags-generation.json')));
const sources=generation.results.flatMap(p=>p.cpp.map(file=>path.join(root,'module/gen/aconfig',p.name,file)));
sources.push(...['src/HWUIProperties.sysprop.cpp','src/statslog_hwui.cpp','src/protos/graphicsstats.pb.cc'].map(f=>path.join(root,'module/gen',f)));
const q=x=>"'"+String(x).replaceAll("'","'\\''")+"'",flags=[...core.flags,...core.cppDefaults,'-std=gnu++2b','-fvisibility=hidden'];
const objects=sources.map((p,i)=>path.join(output,'objects',i+'.o'));const ndk=path.dirname(pool.compiler);
const lines=['cxx = '+q(pool.compiler),'ar = '+q(path.join(ndk,'llvm-ar')),'flags = '+flags.map(q).join(' '),
 'rule cxx\n  command = $cxx $flags -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = GENCC $in',
 'rule ar\n  command = $ar rcs $out $in\n  description = ARCHIVE $out'];
sources.forEach((file,i)=>{if(!fs.existsSync(file))throw Error('Missing original generated input '+file);lines.push('build '+objects[i]+': cxx '+file);});const archive=path.join(output,'libhwui_generated.a');lines.push('build '+archive+': ar '+objects.join(' '),'default '+archive);
fs.writeFileSync(path.join(output,'build.ninja'),lines.join('\n')+'\n');fs.writeFileSync(path.join(output,'inputs.json'),JSON.stringify({sources,flags,archive},null,2));
if(ctx.build !== false){const r=spawnSync(ctx.tools.ninja,['-C',output,'-j2','-k4'],{stdio:'inherit'});if(r.status!==0)throw Error("Original HWUI compilation failed: "+r.status);}

}
