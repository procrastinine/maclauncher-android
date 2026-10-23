import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {pathToFileURL, fileURLToPath} from 'node:url';
const [original,adapterFile,header,rustHelper,output]=process.argv.slice(2);
if(!output)throw Error('AIM source, adapter, header, Rust helper and output required');
const adapter=await import(pathToFileURL(path.resolve(adapterFile)));
const fixture=name=>fileURLToPath(new URL(name,import.meta.url));
fs.mkdirSync(output,{recursive:true});
const objc=path.join(output,'completion-objc');
execFileSync('xcrun',['clang++','-std=c++20','-fobjc-arc','-fblocks','-framework','Foundation',
 '-I'+path.dirname(header),fixture('android-fence-completion.mm'),'-o',objc],{stdio:'inherit'});
execFileSync(objc,[],{stdio:'inherit'});
const source=path.join(output,'crate/src');fs.mkdirSync(source,{recursive:true});
fs.copyFileSync(path.join(original,'crates/aim-sync-file/src/lib.rs'),path.join(source,'lib.rs'));
fs.writeFileSync(path.join(source,'metal.rs'),adapter.adaptMetalFenceCompletion(
 fs.readFileSync(path.join(original,'crates/aim-sync-file/src/metal.rs'),'utf8'))+
 '\ninclude!('+JSON.stringify(fixture('android-fence-completion.rs'))+');\n');
fs.copyFileSync(rustHelper,path.join(source,'maclauncher-fence-completion.rs'));
fs.writeFileSync(path.join(source,'../Cargo.toml'),'[package]\nname="maclauncher-fence-completion-control"\nversion="0.1.0"\nedition="2024"\n[dependencies]\naim-hostcall={path='+JSON.stringify(path.join(original,'crates/aim-hostcall'))+'}\nlibc={path='+JSON.stringify(process.env.ANDROID_RUST_LIBC_SOURCE)+'}\n');
execFileSync(process.env.CARGO||'cargo',['test','--offline','--manifest-path',path.join(source,'../Cargo.toml'),
 '--target-dir',path.join(output,'target'),'completion_cpu_','--','--nocapture'],{stdio:'inherit'});
