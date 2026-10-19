import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** Linux x18 is the existing virtual SCS/register slot. Darwin's physical
 * register never belongs in a guest syscall context or Linux signal frame. */
export function prepareRegisterContexts(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{
  const file=path.join(root,name);let source=fs.readFileSync(file,'utf8');
  for(const[before,after,count=1]of changes){
   if(source.split(before).length!==count+1)throw Error('AIM register-context boundary changed: '+name);
   source=source.replaceAll(before,after);
  }
  writeChanged(file,source);
 };
 edit('context.rs',[
  ['    ctx = const slot::CTX,','    ctx = const slot::CTX,\n    guest_x18 = const slot::SCS,'],
  ['        c.x = [0; 31];','        c.x = [0; 31];\n        c.x[18] = guest_scs();'],
 ]);
 edit('trampoline.S',[
  ['    stp     x18, x19, [x16, #144]',`    str     x19, [x16, #152]
    mrs     x0, tpidrro_el0
    ldr     x0, [x0, #{guest_x18}]
    str     x0, [x16, #144]`],
  ['    ldp     x0, x1, [x16, #0]',`    ldr     x0, [x16, #144]
    mrs     x1, tpidrro_el0
    str     x0, [x1, #{guest_x18}]
    ldp     x0, x1, [x16, #0]`,3],
  ['    ldp     x18, x19, [x16, #144]','    ldr     x19, [x16, #152]',3],
 ]);
 edit('sys/sigframe.rs',[
  ['        x[..29].copy_from_slice(&m.x);','        x[..29].copy_from_slice(&m.x);\n        x[18] = crate::context::guest_scs();'],
  ['        m.x.copy_from_slice(&self.x[..29]);',`        m.x[..18].copy_from_slice(&self.x[..18]);
        m.x[19..].copy_from_slice(&self.x[19..29]);
        crate::context::set_guest_scs(self.x[18]);`],
 ]);
 edit('sys/fork/spawn.rs',[
  ['    context::set_guest_scs(base + scs.len() as u64);',`    context::set_guest_scs(base + scs.len() as u64);
    if !scs.is_empty() { c.x[18] = context::guest_scs(); }`],
 ]);
}
