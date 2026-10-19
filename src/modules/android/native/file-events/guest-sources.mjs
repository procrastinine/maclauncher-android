import {includeDependencies} from '../upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';
const root=import.meta.dirname;
const replace=(text,from,to)=>{if(text.split(from).length!==2)throw Error('Guest file boundary changed: '+from);return text.replace(from,to);};
export function prepareFileEventSources({libc,slink,shim,varargs,output}) {
  const header='#include '+JSON.stringify(path.join(root,'imports.h'))+'\n#include <sys/inotify.h>\n';
  let source=fs.readFileSync(slink,'utf8');
  const start=source.indexOf('// ---- inotify ----'),end=source.indexOf('// ---- clone ----',start);
  if(start<0||end<0)throw Error('Klepton inotify boundary changed');
  source=source.slice(0,start)+`// Linux inotify uses the shared Darwin descriptor/watch owner.
int klb_inotify_init(void) { return inotify_init(); }
int klb_inotify_init1(int flags) {
    if (flags & ~(IN_CLOEXEC | IN_NONBLOCK)) { errno = EINVAL; return -1; }
    return inotify_init1(flags);
}
int klb_inotify_add_watch(int fd,const char *path,uint32_t mask) {
    char storage[4096]; const char *actual = kl_guest_path(path,storage,sizeof(storage));
    return inotify_add_watch(fd,actual,mask);
}
int klb_inotify_rm_watch(int fd,int watch) { return inotify_rm_watch(fd,watch); }

`+source.slice(end);
  fs.writeFileSync(slink,includeDependencies(source,header));
  source=fs.readFileSync(libc,'utf8');
  source=replace(source,'FILE *klb_fopen(const char *path, const char *mode) {','static FILE *klj_original_fopen(const char *path, const char *mode) {');
  source=replace(source,'void kl_fs_trace_open(const char *path, int flags, int fd) {','void kl_fs_trace_open(const char *path, int flags, int fd) {\n    if (fd >= 0) klj_file_opened(fd);');
  // Keep all Klepton namespace/OBB/stdio semantics. Report only the actual
  // successful operation, after the original owner has completed it.
  source+=`\nFILE *klb_fopen(const char *path,const char *mode) {
    FILE *file=klj_original_fopen(path,mode);
    if(file)klj_file_opened(fileno(file));
    return file;
}
`;
  fs.writeFileSync(libc,includeDependencies(source,header+'FILE *klb_fopen(const char *,const char *);'));
  source=fs.readFileSync(shim,'utf8');
  fs.writeFileSync(shim,includeDependencies(source,header));
  if(varargs){
    const file=path.join(output,'file-varargs.c');
    fs.writeFileSync(file,includeDependencies(fs.readFileSync(varargs,'utf8'),header));
    return file;
  }
}
