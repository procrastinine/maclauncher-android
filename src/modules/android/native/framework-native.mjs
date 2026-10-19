import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';

/** Extract the publisher's signed macOS library from the verified AOSP artifact. */
export function extractFrameworkNative(archive, definition, output) {
  const record=definition.nativeRuntime;
  if(!record||record.archivePath!=='native/mac/aarch64/libandroid_runtime.dylib'||record.path!=='libandroid_runtime.dylib')
    throw Error('Android framework native artifact is not declared');
  const target=path.join(output,record.path),fd=fs.openSync(target,'wx',0o755);
  try{execFileSync('/usr/bin/unzip',['-p',archive,record.archivePath],{stdio:['ignore',fd,'inherit'],timeout:30000});}
  finally{fs.closeSync(fd);}
  const bytes=fs.readFileSync(target);
  if(bytes.length!==record.size||crypto.createHash('sha256').update(bytes).digest('hex')!==record.sha256){
    fs.unlinkSync(target);throw Error('Android framework native artifact differs from its recorded identity');
  }
  // Keep the supplied signature/bytes, including its original install name.
  execFileSync('/usr/bin/codesign',['--verify','--strict',target],{stdio:'inherit'});
  for(const data of record.data||[]){
    if(!/^icu\/icudt[0-9]+l\.dat$/.test(data.archivePath)||path.basename(data.archivePath)!==data.path)throw Error('Invalid Android native ICU data declaration');
    const file=path.join(output,data.path),descriptor=fs.openSync(file,'wx',0o644);
    try{execFileSync('/usr/bin/unzip',['-p',archive,data.archivePath],{stdio:['ignore',descriptor,'inherit'],timeout:30000});}finally{fs.closeSync(descriptor);}
    const bytes=fs.readFileSync(file);
    if(bytes.length!==data.size||crypto.createHash('sha256').update(bytes).digest('hex')!==data.sha256){fs.unlinkSync(file);throw Error('Android native ICU data differs from its recorded identity');}
  }
  return target;
}
