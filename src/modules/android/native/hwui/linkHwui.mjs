import {minimalSanitizerRuntime} from './sanitizers.mjs';
import fs from'node:fs';import path from'node:path';import crypto from'node:crypto';import{spawnSync}from'node:child_process';

export async function linkHwui(ctx) {
const root=ctx.work,output=path.join(root,'module/link'),core=JSON.parse(fs.readFileSync(path.join(root,'core-object-inputs.json'))),pool={compiler:ctx.tools.clangxx};fs.mkdirSync(output,{recursive:true});
const ndk=path.dirname(pool.compiler),hwui=core.records.find(r=>r.name==='libhwui').objects,skia=core.records.find(r=>r.name==='libskia').objects;
const staticRoot=path.join(root,'module/components/native-static');const statics=fs.readdirSync(staticRoot).filter(f=>f.endsWith('.a')).map(f=>path.join(staticRoot,f));
const whole=[path.join(root,'module/components/crabbyavif/cargo-target/aarch64-linux-android/release/libcrabbyavif.a')];
const normal=[minimalSanitizerRuntime(ctx),...statics,path.join(root,'module/components/perfetto/libperfetto_client_experimental.a'),path.join(root,'module/components/generated/libhwui_generated.a'),path.join(root,'module/components/crabbyavif/libyuv.a')];
const shared=JSON.parse(fs.readFileSync(path.join(output,'shared-inputs.json'))).files.map(f=>path.join(output,'shared',f.name));
const required=[...hwui,...skia,...whole,...normal,...shared];const absent=required.filter(p=>!fs.existsSync(p));if(absent.length)throw Error('Complete original module inputs absent: '+JSON.stringify(absent));
const product=path.join(output,'libhwui.unstripped.so'),versionScript=path.join(root,'module/source/framework/libs/hwui/libhwui.map.txt');
// Platform source is compiled with current-platform declarations.  Link uses
// the installed NDK's actual API35 startup objects; the supplier remains tied
// to the complete API36 original system-image private ABI.
const args=['--target=aarch64-linux-android35','-shared','-nostdlib++','-Wl,-soname,libhwui.so','-Wl,-z,defs','-Wl,--no-undefined-version','-Wl,--build-id=sha1','-Wl,-z,max-page-size=16384','-Wl,--gc-sections','-Wl,--icf=safe','-Wl,-z,noexecstack','-Wl,-z,relro','-Wl,-z,now','-Wl,--version-script='+versionScript,
 ...hwui,...skia,'-Wl,--whole-archive',...whole,'-Wl,--no-whole-archive','-Wl,--start-group',...normal,'-Wl,--end-group','-Wl,--no-as-needed',...shared,'-Wl,--as-needed','-L'+path.join(output,'shared'),'-o',product];
const rsp=path.join(output,'arguments.rsp');fs.writeFileSync(rsp,args.map(a=>'"'+a.replaceAll('\\','\\\\').replaceAll('"','\\"')+'"').join('\n')+'\n');
const result=spawnSync(pool.compiler,['@'+rsp],{encoding:'utf8',maxBuffer:16e6});fs.writeFileSync(path.join(output,'link.log'),(result.stdout||'')+(result.stderr||''));
fs.writeFileSync(path.join(output,'link-result.json'),JSON.stringify({compiler:pool.compiler,args,hwuiObjects:hwui.length,skiaObjects:skia.length,whole,normal,shared,status:result.status,signal:result.signal,product:result.status===0?product:null,error:result.error?.message},null,2));
console.log(JSON.stringify({status:result.status,signal:result.signal,stderr:result.stderr,product:result.status===0?product:null}));if(result.status!==0)throw Error('Complete HWUI link failed: '+(result.stderr||result.error?.message||result.status));
if(result.status===0){const final=path.join(output,'libhwui.so');fs.copyFileSync(product,final);const strip=spawnSync(path.join(ndk,'llvm-strip'),['--strip-unneeded',final],{encoding:'utf8'});if(strip.status!==0)throw Error(strip.stderr);const b=fs.readFileSync(final);fs.writeFileSync(path.join(output,'product.json'),JSON.stringify({path:final,size:b.length,sha256:crypto.createHash('sha256').update(b).digest('hex'),unstripped:product},null,2));}

return JSON.parse(fs.readFileSync(path.join(output,'product.json')));

}
