import {includeDependencies} from './upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

/** Share the launcher's actual filesystem namespace before upstream fallbacks. */
export function prepareGuestPathSource(file){
  let source=fs.readFileSync(file,'utf8');
  const anchor='const char *kl_guest_path(const char *path, char *buf, size_t cap) {';
  if(source.split(anchor).length!==2)throw Error('Klepton guest path boundary changed');
  source=source.replace(anchor,anchor+`\n    if (path && !strncmp(path,"/proc/",6) &&
        (klj_proc_maps_self(path,0) || klj_proc_maps_self(path,(uint64_t)klj_process_id()))) {
        const char *root=klj_proc_maps_root();
        return root&&klj_proc_maps_native(root,buf,cap,(uint64_t)klj_process_id())?buf:path;
    }
    const char *owned = klj_mapped_guest_path(path, buf, cap);
    if (owned) return owned;`);
  const replace=(from,to)=>{if(source.split(from).length!==2)throw Error('Klepton proc owner changed: '+from);source=source.replace(from,to);};
  replace('static char g_procroot[512];','static char g_procroot[512];\nstatic pthread_once_t klj_proc_once=PTHREAD_ONCE_INIT;');
  replace('    static pthread_once_t once = PTHREAD_ONCE_INIT;\n    pthread_once(&once, proc_build);','    pthread_once(&klj_proc_once, proc_build);');
  source+='\nconst char *klj_proc_maps_root(void) {\n    pthread_once(&klj_proc_once,proc_build);\n    return g_procroot[0]?g_procroot:NULL;\n}\n';
  const root=path.dirname(fileURLToPath(import.meta.url));
  fs.writeFileSync(file,includeDependencies(source,['guest-paths.h','proc-maps.h','process-ids.h'].map(name=>'#include '+JSON.stringify(path.join(root,name))).join('\n')));
}
