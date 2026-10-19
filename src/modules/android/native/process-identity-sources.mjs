import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

/** Translate the native stat ABI at the existing Klepton Darwin/Bionic boundary. */
export function prepareProcessIdentitySource(file) {
  let source=fs.readFileSync(file,'utf8');
  const gettid='int  klb_gettid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return (int)t; }';
  if(source.split(gettid).length!==2)throw Error('Klepton native thread identity boundary changed');
  source=source.replace(gettid,'int klb_gettid(void) { return klj_thread_id(); }');
  const old='b->st_uid = d->st_uid;             b->st_gid  = d->st_gid;';
  if(source.split(old).length!==2)throw Error('Klepton native file identity boundary changed');
  const header=path.join(path.dirname(fileURLToPath(import.meta.url)),'process-identity.h');
  const threads=path.join(path.dirname(header),'thread-identity.h');
  fs.writeFileSync(file,'#include '+JSON.stringify(header)+'\n#include '+JSON.stringify(threads)+'\n'+source.replace(old,
    'b->st_uid = klj_process_uid_from_host(d->st_uid); b->st_gid = klj_process_gid_from_host(d->st_gid);'));
}
