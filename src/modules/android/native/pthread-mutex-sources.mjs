import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const directory=path.dirname(fileURLToPath(import.meta.url));

/** Replace only the Bionic-to-Darwin mutex ownership boundary. */
export function prepareMutexOwnership(source) {
 const replace=(from,to)=>{if(source.split(from).length!==2)throw Error('Pinned mutex source changed: '+from);source=source.replace(from,to);};
 const region=(start,end,replacement)=>{const first=source.indexOf(start),last=source.indexOf(end,first);if(first<0||last<0)throw Error('Pinned mutex region changed: '+start);source=source.slice(0,first)+replacement+'\n'+source.slice(last);};
 region('// ---------- mutexes: address-keyed map ----------','// ---------- condition variables:',
  '#include '+JSON.stringify(path.join(directory,'pthread-mutex-map.h'))+'\n#define MTX_MAP_SIZE 262144 /* Existing condition registry; not a mutex capacity. */\n');
 region('// The host mutex behind an entry. Separate from the lookup','static pthread_rwlock_t *rwl','');
 region('int klb_pthread_mutex_init(void *m,','static int thread_is_alive(',fs.readFileSync(path.join(directory,'pthread-mutex-calls.inc'),'utf8'));
 replace('    if (atomic_fetch_add(&said, 1) >= 8) return;',
  '    if (atomic_load(&said) >= 8 || atomic_fetch_add(&said, 1) >= 8) return;');
 replace('    fprintf(stderr, " [held %d deep]", atomic_load(&mtx_entry_for(m)->depth));\n    void *now = (void *)mtx_host(mtx_entry_for(m));',
  '    mtx_entry *entry = mtx_entry_for(m);\n    fprintf(stderr, " [held %d deep]", entry ? atomic_load(&entry->depth) : 0);\n    void *now = entry ? (void *)mtx_host(entry) : NULL;\n    mtx_entry_release(entry);');
 replace('if (g) { mtx_entry *me = mtx_entry_for(g);\n                     owner',
  'if (g) { mtx_entry *me = mtx_entry_for(g);\n                     if (!me) continue;\n                     owner');
 replace('depth = atomic_load(&me->depth); }','depth = atomic_load(&me->depth); mtx_entry_release(me); }');
 const reportStart=source.indexOf('    for (uint32_t i = 0; i < MTX_MAP_SIZE; i++) {',source.indexOf('void kl_pthread_report'));
 const reportEnd=source.indexOf('    if (!shown)',reportStart);
 if(reportStart<0||reportEnd<0)throw Error('Pinned mutex reporting changed');
 let report=source.slice(reportStart,reportEnd);
 report=report.replace('    for (uint32_t i = 0; i < MTX_MAP_SIZE; i++) {',
  '    pthread_once(&g_mtx_once, mtx_registry_init);\n    for (unsigned i = 0; i < MTX_BUCKET_COUNT; i++) {\n        mtx_bucket *bucket = &g_mtx_buckets[i];\n        if (pthread_mutex_trylock(&bucket->lock)) continue;\n        for (mtx_entry *entry = bucket->head; entry; entry = entry->next) {')
  .replaceAll('&g_mtx_map[i].','&entry->')
  .replace('        shown++;\n    }','        shown++;\n        }\n        pthread_mutex_unlock(&bucket->lock);\n    }');
 source=source.slice(0,reportStart)+report+source.slice(reportEnd);
 // The ordinary condition wait owns a mutex reference for its complete wait.
 const waitStart=source.indexOf('int klb_pthread_cond_wait(void *c,'),waitEnd=source.indexOf('int klb_pthread_cond_timedwait(',waitStart);
 let wait=source.slice(waitStart,waitEnd);
 wait=wait.replace('    mtx_entry *e = mtx_entry_for(m);','    mtx_entry *e = mtx_entry_for(m);\n    if (!e) return px(m ? errno : EINVAL);')
  .replace('    atomic_store(&e->owner, NULL);','    int depth = mtx_wait_suspend(e);')
  .replace('    int raw = pthread_cond_wait(cnd_host(ce), mtx_host(e));',
   '    int raw;\n    struct mtx_cond_cleanup cleanup = {ce, e, depth};\n    pthread_cleanup_push(mtx_cond_cancelled, &cleanup);\n    raw = pthread_cond_wait(cnd_host(ce), mtx_host(e));\n    pthread_cleanup_pop(0);')
  .replace('    atomic_store(&e->locksite, __builtin_return_address(0));\n    atomic_store(&e->owner, (void *)pthread_self());',
   '    mtx_wait_resume(e, depth, raw == 0, __builtin_return_address(0));')
  .replace('    return r;','    mtx_entry_release(e);\n    return r;');
 source=source.slice(0,waitStart)+`struct mtx_cond_cleanup { cnd_entry *condition; mtx_entry *mutex; int depth; };
static void mtx_cond_cancelled(void *opaque) {
    struct mtx_cond_cleanup *state = opaque;
    atomic_fetch_sub(&state->condition->waiters, 1);
    cnd_sleep_leave();
    mtx_wait_resume(state->mutex, state->depth, 1, NULL);
    mtx_entry_release(state->mutex);
}
`+wait+source.slice(waitEnd);
 return source;
}
