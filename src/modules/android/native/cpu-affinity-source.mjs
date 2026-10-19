import fs from 'node:fs';
import path from 'node:path';
const root=import.meta.dirname;
export function adaptCpuAffinity(source){
 const original=`int  klb_sched_getaffinity(int p, size_t sz, void *m) {
    (void)p; if (m && sz) memset(m, 0xFF, sz);       // pretend every CPU is available
    return 0;
}`;
 if(source.split(original).length!==2)throw Error('Native CPU affinity boundary changed');
 return source.replace(original,fs.readFileSync(path.join(root,'cpu-affinity.inc'),'utf8'));
}
