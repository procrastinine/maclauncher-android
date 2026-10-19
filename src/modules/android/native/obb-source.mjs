import {includeDependencies} from './upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';

function replace(source,before,after){
  if(source.split(before).length!==2)throw Error('Klepton directory owner changed: '+before.slice(0,90));
  return source.replace(before,after);
}

// Preserve Klepton's indexed OBB entries and merged real-directory iteration.
// Its live registry grows with actual streams and retires each node on close.
export function prepareObbSource(upstream,build){
  let source=fs.readFileSync(path.join(upstream,'runtime/guest/kl_obbmap.c'),'utf8');
  source=replace(source,'d->real = NULL;   // exhausted (closedir handles the actual close)',
    '// Keep the exhausted stream owned until closedir or rewinddir.');
  source=replace(source,'typedef struct {\n    DIR   *real;',
    'typedef struct obb_dir {\n    struct obb_dir *next;\n    DIR   *real;');
  source=replace(source,'static obb_dir *g_dirs[64];','static obb_dir *g_dirs;');
  const allocation=`    obb_dir *d = malloc(sizeof *d);
    *d = tmp; d->real = real; d->i = 0; d->magic = OBB_DIR_MAGIC;
    pthread_mutex_lock(&g_dir_lock);
    for (unsigned k = 0; k < sizeof g_dirs / sizeof g_dirs[0]; k++)
        if (!g_dirs[k]) { g_dirs[k] = d; break; }
    pthread_mutex_unlock(&g_dir_lock);
    return d;`;
  source=replace(source,allocation,`    obb_dir *d = malloc(sizeof *d);
    if (!d) {
        if (real) closedir(real);
        for (unsigned k = 0; k < tmp.n; ++k) free(tmp.names[k]);
        free(tmp.names); free(tmp.types); errno = ENOMEM;
        return (void *)-1;
    }
    *d = tmp; d->real = real; d->i = 0; d->magic = OBB_DIR_MAGIC;
    pthread_mutex_lock(&g_dir_lock);
    d->next = g_dirs; g_dirs = d;
    pthread_mutex_unlock(&g_dir_lock);
    return d;`);
  source=replace(source,`    for (unsigned k = 0; k < sizeof g_dirs / sizeof g_dirs[0]; k++)
        if (g_dirs[k] == h) { yes = 1; break; }`,
    '    for (obb_dir *current = g_dirs; current; current = current->next)\n        if (current == h) { yes = 1; break; }');
  source=replace(source,`    for (unsigned k = 0; k < sizeof g_dirs / sizeof g_dirs[0]; k++)
        if (g_dirs[k] == h) { g_dirs[k] = NULL; break; }`,
    '    for (obb_dir **link = &g_dirs; *link; link = &(*link)->next)\n        if (*link == h) { *link = d->next; break; }');
  const extra=`
int klj_obb_asset_window(const char *name,int64_t *offset,int64_t *length) {
    int source=kl_obbmap_open(name,O_RDONLY);
    if(source<0)return -1;
    int fd=source,windowed=0;
    pthread_mutex_lock(&g_vfd_lock);
    obb_vfd *window=vfd_find(source);
    if(window){
        *offset=window->base;*length=window->len;windowed=1;
        fd=fcntl(source,F_DUPFD_CLOEXEC,0);
    }
    pthread_mutex_unlock(&g_vfd_lock);
    if(windowed){int saved=errno,handled=0;kl_obbmap_close(source,&handled);errno=saved;return fd;}
    struct stat info;
    if(fstat(fd,&info)){int saved=errno;close(fd);errno=saved;return -1;}
    if(!S_ISREG(info.st_mode)){close(fd);errno=EINVAL;return -1;}
    *offset=0;*length=info.st_size;return fd;
}
int klj_directory_fd(void *handle) {
    if (!handle) { errno=EBADF; return -1; }
    pthread_mutex_lock(&g_dir_lock);
    for (obb_dir *current=g_dirs;current;current=current->next) {
        if (current==handle) {
            int fd=current->real ? dirfd(current->real) : -1;
            pthread_mutex_unlock(&g_dir_lock);
            if(fd<0)errno=EBADF;
            return fd;
        }
    }
    pthread_mutex_unlock(&g_dir_lock);
    return dirfd((DIR *)handle);
}
int klj_obb_directory_rewind(void *handle) {
    if (!is_our_dir(handle)) { errno=EBADF; return -1; }
    obb_dir *directory=handle;
    if(directory->real)rewinddir(directory->real);
    directory->i=0;return 0;
}
`;
  const target=path.join(build,'obb-owned.c');fs.writeFileSync(target,source+extra);return target;
}

// Guest DIR is opaque. Give every stream one translated result buffer while
// retaining the original namespace, OBB discovery, tracing and scandir code.
export function prepareDirectoryCallers(libc){
  let source=fs.readFileSync(libc,'utf8');
  const anchor='    void *syn = kl_obbmap_opendir(_p);';
  source=replace(source,anchor,anchor+'\n    if (syn == (void *)-1) return NULL;');
  source=replace(source,'if (syn) { kl_fs_trace("opendir", _p, "obb", 0); return syn; }',
    'if (syn) { kl_fs_trace("opendir", _p, "obb", 0); return klj_directory_create(syn, 1); }');
  source=replace(source,'kl_fs_trace("opendir", _p, NULL, !d);\n    return d;',
    'kl_fs_trace("opendir", _p, NULL, !d);\n    return klj_directory_create(d, 0);');
  for(const [name,signature,body]of [
    ['klb_closedir','int klb_closedir(void *d)','return klj_directory_close(d);'],
    ['klb_readdir','void *klb_readdir(void *d)','klj_dirent *entry = klj_directory_read(d); if (entry) kl_fs_trace("readdir", entry->d_name, NULL, 0); return entry;']]){
    const start=source.indexOf(name+'(void *d)'),line=source.lastIndexOf('\n',start)+1,end=source.indexOf('\n}',start);
    if(start<0||end<start)throw Error('Klepton directory operation changed: '+name);
    source=source.slice(0,line)+signature+' { '+body+' }'+source.slice(end+2);
  }
  fs.writeFileSync(libc,includeDependencies(source,'#include '+JSON.stringify(path.join(import.meta.dirname,'directory-bridge.h'))));
}
