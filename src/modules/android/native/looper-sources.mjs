import fs from 'node:fs';
import path from 'node:path';

/** Complete the existing Klepton poller's event and lifetime semantics. */
export function prepareLooperSource(klepton, build) {
  let source=fs.readFileSync(path.join(klepton,'runtime/guest/kl_ndk.c'),'utf8');
  const replace=(from,to)=>{if(source.split(from).length!==2)throw Error('Pinned Klepton looper source changed');source=source.replace(from,to);};
  // The complete original NDK module owns assets and configurations. Retain
  // only the prepared loose-file path supplied by the platform boundary.
  for(const [start,end,replacement]of [
    ['// ==================================================================== AAsset','// =================================================================== ASensor',
     'static char g_asset_root[4096]="assets";\nvoid kl_ndk_set_assets_dir(const char *dir){if(dir)snprintf(g_asset_root,sizeof g_asset_root,"%s",dir);}\n\n'],
    ['// ============================================================ AConfiguration','// ========================================================== ANativeActivity',''],
  ]){
    const first=source.indexOf(start),last=source.indexOf(end,first);
    if(first<0||last<first)throw Error('Pinned superseded NDK family changed');
    source=source.slice(0,first)+replacement+source.slice(last);
  }
  source=source.replace(/^\s*N\("A(?:Asset|Configuration)[^\n]*\n/gm,'');
  replace('enum { ALOOPER_EVENT_INPUT = 1 << 0 };','enum { ALOOPER_EVENT_INPUT=1, ALOOPER_EVENT_OUTPUT=2, ALOOPER_EVENT_ERROR=4, ALOOPER_EVENT_HANGUP=8, ALOOPER_EVENT_INVALID=16 };');
  replace('#define KL_LOOPER_MAX_FDS 16','#define KL_LOOPER_MAX_FDS 256');
  replace('int          fd, ident, events;','int          fd, ident, events;\n        uint64_t sequence;');
  replace('int             nfds;','int             nfds;\n    uint64_t sequence;');
  replace('static void kl_looper_key_init(void) { pthread_key_create(&g_looper_key, NULL); }',`static void kl_ALooper_release(kl_looper *l);
static void kl_looper_thread_exit(void *value) { kl_ALooper_release(value); }
static void kl_looper_key_init(void) { pthread_key_create(&g_looper_key, kl_looper_thread_exit); }`);
  replace('fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL, 0) | O_NONBLOCK);','fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL, 0) | O_NONBLOCK);\n    fcntl(p[0], F_SETFD, FD_CLOEXEC); fcntl(p[1], F_SETFD, FD_CLOEXEC);');
  replace('pthread_setspecific(g_looper_key, l);','if (pthread_setspecific(g_looper_key, l)) { close(p[0]); close(p[1]); pthread_mutex_destroy(&l->lock); free(l); return NULL; }');
  const releaseStart=source.indexOf('static void kl_ALooper_release(kl_looper *l) {'),releaseEnd=source.indexOf('\nstatic void kl_ALooper_wake',releaseStart);
  if(releaseStart<0||releaseEnd<0)throw Error('Pinned Klepton looper release changed');
  source=source.slice(0,releaseStart)+`static void kl_ALooper_release(kl_looper *l) {
    if(l && __atomic_sub_fetch(&l->refs,1,__ATOMIC_ACQ_REL)==0) {
        close(l->wake_r); close(l->wake_w); pthread_mutex_destroy(&l->lock); free(l);
    }
}
`+source.slice(releaseEnd);
  replace('if (!l) return -1;\n    pthread_mutex_lock(&l->lock);\n    for (int i = 0; i < l->nfds; i++)\n        if (l->fds[i].fd == fd) {          // re-registering replaces','if (!l || fd<0 || (events & ~31) || (!cb && ident<0)) return -1;\n    pthread_mutex_lock(&l->lock);\n    for (int i = 0; i < l->nfds; i++)\n        if (l->fds[i].fd == fd) {          // re-registering replaces');
  replace('l->fds[i].cb = cb; l->fds[i].data = data;','l->fds[i].cb = cb; l->fds[i].data = data; l->fds[i].sequence=++l->sequence;');
  replace('pthread_mutex_unlock(&l->lock);\n            return 1;\n        }\n    if (l->nfds == KL_LOOPER_MAX_FDS)','pthread_mutex_unlock(&l->lock);\n            kl_ALooper_wake(l); return 1;\n        }\n    if (l->nfds == KL_LOOPER_MAX_FDS)');
  replace('l->fds[l->nfds++] = (typeof(l->fds[0])){fd, ident, events, cb, data};\n    pthread_mutex_unlock(&l->lock);\n    return 1;', 'l->fds[l->nfds++] = (typeof(l->fds[0])){.fd=fd,.ident=ident,.events=events,.cb=cb,.data=data,.sequence=++l->sequence};\n    pthread_mutex_unlock(&l->lock);\n    kl_ALooper_wake(l); return 1;');
  replace('l->fds[i] = l->fds[--l->nfds];\n            pthread_mutex_unlock(&l->lock);\n            return 1;', 'l->fds[i] = l->fds[--l->nfds];\n            pthread_mutex_unlock(&l->lock);\n            kl_ALooper_wake(l); return 1;');
  replace('void         *data[KL_LOOPER_MAX_FDS + 1];','void         *data[KL_LOOPER_MAX_FDS + 1];\n    uint64_t sequence[KL_LOOPER_MAX_FDS + 1];');
  replace('(short)(l->fds[i].events & ALOOPER_EVENT_INPUT ? POLLIN : 0), 0}', '(short)(((l->fds[i].events & ALOOPER_EVENT_INPUT) ? POLLIN : 0) | ((l->fds[i].events & ALOOPER_EVENT_OUTPUT) ? POLLOUT : 0) | POLLERR | POLLHUP | POLLNVAL), 0}');
  replace('data[n]  = l->fds[i].data;','data[n]  = l->fds[i].data; sequence[n]=l->fds[i].sequence;');
  replace('int r = poll(pfd, (nfds_t)n, timeoutMillis);',
    'extern int klj_platform_poll(struct pollfd *, nfds_t, int);\n    int r = klj_platform_poll(pfd, (nfds_t)n, timeoutMillis);');
  replace('if (cb[i]) { cb[i](pfd[i].fd, pfd[i].revents, data[i]); return ALOOPER_POLL_CALLBACK; }', `int events=((pfd[i].revents&POLLIN)?ALOOPER_EVENT_INPUT:0)|((pfd[i].revents&POLLOUT)?ALOOPER_EVENT_OUTPUT:0)|((pfd[i].revents&POLLERR)?ALOOPER_EVENT_ERROR:0)|((pfd[i].revents&POLLHUP)?ALOOPER_EVENT_HANGUP:0)|((pfd[i].revents&POLLNVAL)?ALOOPER_EVENT_INVALID:0);
        int current=0; pthread_mutex_lock(&l->lock);
        for(int j=0;j<l->nfds;j++)if(l->fds[j].fd==pfd[i].fd && l->fds[j].sequence==sequence[i]){current=1;break;}
        pthread_mutex_unlock(&l->lock); if(!current)continue;
        if (cb[i]) {
            int keep=cb[i](pfd[i].fd,events,data[i]);
            if(!keep){pthread_mutex_lock(&l->lock);for(int j=0;j<l->nfds;j++)if(l->fds[j].fd==pfd[i].fd && l->fds[j].sequence==sequence[i]){l->fds[j]=l->fds[--l->nfds];break;}pthread_mutex_unlock(&l->lock);}
            return ALOOPER_POLL_CALLBACK;
        }`);
  replace('if (outEvents) *outEvents = pfd[i].revents;','if (outEvents) *outEvents = events;');
  // Input and other ready events are delivered before the frame phase.
  replace('    for (int i = 1; i < n; i++) {\n        if (!pfd[i].revents) continue;',
    '    extern int klj_ndk_choreographer_owns_fd(int);\n    for (int phase=0; phase<2; phase++) for (int i = 1; i < n; i++) {\n        if (!pfd[i].revents || klj_ndk_choreographer_owns_fd(pfd[i].fd)!=phase) continue;');
  const output=path.join(build,'ndk-looper-source.c');fs.writeFileSync(output,source);return output;
}
