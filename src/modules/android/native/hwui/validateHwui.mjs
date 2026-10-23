import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
export function validateHwui(ctx,product) {
 const bin=path.dirname(ctx.tools.clangxx),original=path.join(ctx.image.root,'system/lib64/libhwui.so');
 const tool=(name,args)=>execFileSync(ctx.tools[name]||path.join(bin,name.replace(/^llvm/,'llvm-').toLowerCase()),args,{encoding:'utf8',maxBuffer:32e6});
 const nm=ctx.tools.llvmNm||path.join(bin,'llvm-nm'),readelf=ctx.tools.llvmReadelf||path.join(bin,'llvm-readelf');
 const run=(p,args)=>execFileSync(p,args,{encoding:'utf8',maxBuffer:32e6});
 const exports=p=>run(nm,['--dynamic','--defined-only','--format=posix',p]).trim().split('\n').map(line=>line.split(/\s+/)[0]).filter(Boolean).sort();
 const dynamic=p=>run(readelf,['-d',p]);const needed=p=>[...dynamic(p).matchAll(/\(NEEDED\).*\[([^\]]+)\]/g)].map(m=>m[1]).sort();
 const expected=exports(original),actual=exports(product.path),missing=expected.filter(v=>!actual.includes(v)),extra=actual.filter(v=>!expected.includes(v));
 if(missing.length||extra.length)throw Error('Complete HWUI exports differ from the declared original image ABI: '+JSON.stringify({missing,extra}));
 const originalNeeded=needed(original),actualNeeded=needed(product.path);if(JSON.stringify(originalNeeded)!==JSON.stringify(actualNeeded))throw Error('Complete HWUI shared-library dependencies differ from the declared image');
 const info=run(readelf,['-h','-l','-n',product.path]),loads=info.split('\n').filter(line=>/^\s+LOAD\s/.test(line));
 if(!/Machine:\s+AArch64/.test(info)||!/Type:\s+DYN/.test(info)||loads.length<3||loads.some(line=>!/0x4000\s*$/.test(line)||/W.*E|E.*W/.test(line))||!info.includes('GNU_RELRO')||/\((?:RPATH|RUNPATH)\)/.test(dynamic(product.path)))throw Error('Complete HWUI ELF layout differs from its API36 ARM64 image ABI');
 const bytes=fs.readFileSync(product.path);
 const result={schema:1,...product,sha256:crypto.createHash('sha256').update(bytes).digest('hex'),size:bytes.length,buildId:info.match(/Build ID: ([a-f0-9]+)/)?.[1],minimumAndroidAPI:36,exports:actual.length,needed:actualNeeded,originalABI:true,pageAlignment:16384};
 fs.writeFileSync(path.join(ctx.link,'validation.json'),JSON.stringify(result,null,2)+'\n');fs.writeFileSync(path.join(ctx.work,'runtime-files.json'),JSON.stringify({files:[{path:'guest/libhwui.so',source:product.path,size:result.size,mode:420,sha256:result.sha256}]},null,2)+'\n');return result;
}
