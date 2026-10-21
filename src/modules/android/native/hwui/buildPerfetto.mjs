import fs from'node:fs';import path from'node:path';import{spawnSync}from'node:child_process';

export async function buildPerfetto(ctx) {
const root=ctx.work,output=path.join(root,'module/components/perfetto'),source=path.join(root,'module/source/perfetto');fs.mkdirSync(output,{recursive:true});
const graph=JSON.parse(fs.readFileSync(path.join(root,'perfetto-component-inputs.json'))),core=JSON.parse(fs.readFileSync(path.join(root,'core-object-inputs.json'))),pool={compiler:ctx.tools.clangxx};
const modules=JSON.parse(fs.readFileSync(new URL('./perfetto-blueprint.json',import.meta.url))).modules;
const defaults=modules.find(m=>m.properties.name==='perfetto_defaults').properties;
const generated=graph.generators.flatMap(g=>g.out.filter(f=>/\.(cc|cpp|c)$/.test(f)).map(f=>path.join(root,'module/gen/perfetto',f)));
const original=graph.sources.filter(f=>/\.(cc|cpp|c)$/.test(f)).map(f=>path.join(source,f));const sources=[...new Set([...original,...generated])];
const q=x=>"'"+String(x).replaceAll("'","'\\''")+"'";
const flags=[...core.flags,...core.cppDefaults,'-std=gnu++17',...defaults.cflags,'-I'+source,'-I'+path.join(source,'include'),'-I'+path.join(source,'include/perfetto/base/build_configs/android_tree'),'-I'+path.join(source,'src/profiling/memory/include')];
const ndk=path.dirname(pool.compiler),objects=sources.map((file,i)=>path.join(output,'objects',String(i).padStart(3,'0')+'.o'));fs.mkdirSync(path.join(output,'objects'),{recursive:true});
const ninja=['cxx = '+q(pool.compiler),'ar = '+q(path.join(ndk,'llvm-ar')),'flags = '+flags.map(q).join(' '),
 'rule cxx\n  command = $cxx $flags -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = PERFETTO $in',
 'rule ar\n  command = $ar rcs $out $in\n  description = ARCHIVE $out'];
sources.forEach((file,i)=>{if(!fs.existsSync(file))throw Error('Original/generated Perfetto input absent '+file);ninja.push('build '+objects[i]+': cxx '+file)});
const archive=path.join(output,'libperfetto_client_experimental.a');ninja.push('build '+archive+': ar '+objects.join(' '),'default '+archive);
fs.writeFileSync(path.join(output,'build.ninja'),ninja.join('\n')+'\n');fs.writeFileSync(path.join(output,'inputs.json'),JSON.stringify({sourceRevision:'94a71474cc396db2b0fcff6755b8f2d3f7e304d0',original,generated,sources,flags,archive},null,2));
if(ctx.build !== false){const ninja=ctx.tools.ninja;const r=spawnSync(ninja,['-C',output,'-j2','-k4'],{stdio:'inherit'});if(r.status!==0)throw Error("Original HWUI compilation failed: "+r.status);}else console.log({sources:sources.length,archive});

}
