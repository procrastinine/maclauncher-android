import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

/** Embed the same bundled host suffixes consumed by the launcher URL policy. */
export function prepareNetworkPolicy(build){
  const root=path.dirname(fileURLToPath(import.meta.url));
  const hosts=JSON.parse(fs.readFileSync(path.join(root,'resources/ad-hosts.json'),'utf8'));
  if(!Array.isArray(hosts)||hosts.some(host=>typeof host!=='string'||!/^([a-z0-9-]+\.)+[a-z0-9-]+$/.test(host))||new Set(hosts).size!==hosts.length)
    throw new Error('Invalid Android advertising host catalog');
  hosts.sort();
  fs.writeFileSync(path.join(build,'ad-hosts.h'),'// Generated from the shared Android advertising host catalog.\nstatic const char *const klj_ad_hosts[]={\n'+hosts.map(host=>'    '+JSON.stringify(host)).join(',\n')+'\n};\n');
}
