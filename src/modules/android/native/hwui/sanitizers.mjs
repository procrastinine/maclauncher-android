import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
/** Preserve the selected original Blueprint sanitizer policy across defaults. */
export function mergeSanitizers(target={},source={}) {
 const next={...target};for(const [key,value] of Object.entries(source)){if(Array.isArray(value))next[key]=[...(next[key]||[]),...value];else if(value&&typeof value==='object')next[key]=mergeSanitizers(next[key],value);else next[key]=value;}return next;
}
export function sanitizerFlags(policy={},ctx) {
 if(policy.never)return [];
 const supported=new Set(['integer_overflow','misc_undefined','diag','never']);
 for(const key of Object.keys(policy))if(!supported.has(key)&&policy[key])throw Error('The pinned HWUI build requires an additional original sanitizer policy: '+key);
 if(Object.values(policy.diag||{}).some(Boolean))throw Error('The pinned HWUI sanitizer diagnostic runtime differs');
 const enabled=[...(policy.misc_undefined||[]),...(policy.integer_overflow?['unsigned-integer-overflow','signed-integer-overflow']:[])];
 if(!enabled.length)return [];
 const flags=['-fsanitize='+[...new Set(enabled)].join(','),'-fsanitize-trap=all','-fsanitize-minimal-runtime','-fno-sanitize-trap=integer,undefined','-fno-sanitize-recover=integer,undefined'];
 if(policy.integer_overflow){const file=path.join(ctx.sources.soong,'cc/config/integer_overflow_blocklist.txt');if(!fs.statSync(file).isFile())throw Error('Original integer-overflow configuration is absent');flags.push('-fsanitize-ignorelist='+file);}
 return flags;
}
export function minimalSanitizerRuntime(ctx) {
 const file=execFileSync(ctx.tools.clangxx,['--target=aarch64-linux-android35','--print-file-name=libclang_rt.ubsan_minimal-aarch64-android.a'],{encoding:'utf8'}).trim();
 if(!path.isAbsolute(file)||!fs.statSync(file).isFile())throw Error('The declared Android compiler has no original minimal UBSan runtime');return file;
}
