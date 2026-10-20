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
  ['    pub(crate) thread: *const crate::sys::Thread,',`    pub(crate) thread: *const crate::sys::Thread,
    // Owned allocation identity, independent of the architectural x18 slot.
    owned_scs_base: u64,`],
  ['        slot_ptr(slot::SCS).write(st.scs());',`        (*ctx).owned_scs_base = st.scs();
        slot_ptr(slot::SCS).write(st.scs());`],
  ['/// Set the guest shadow-call-stack pointer',`/// The current context's actual runtime-owned shadow-stack allocation.
/// Architectural x18 may instead address ordinary restored guest memory.
pub(crate) fn owned_scs_range() -> Option<std::ops::RangeInclusive<u64>> {
    let ctx = current_ctx();
    if ctx.is_null() { return None; }
    // SAFETY: current_ctx belongs to this thread until its existing unbind.
    let base = unsafe { (*ctx).owned_scs_base };
    (base != 0).then_some(base..=base + SCS_SIZE as u64)
}

/// Set the guest shadow-call-stack pointer`],
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
  [`fn shadow_stack() -> Vec<u8> {
    let scs = context::guest_scs();
    match vmmap::info_at(scs) {
        // SAFETY: the used part of this thread's shadow stack.
        Some(i) if i.start <= scs && scs < i.end => unsafe {
            std::slice::from_raw_parts(i.start as *const u8, (scs - i.start) as usize).to_vec()
        },
        _ => Vec::new(),
    }
}`,`fn shadow_stack() -> Option<Vec<u8>> {
    let owned = context::owned_scs_range()?;
    let scs = context::guest_scs();
    if !owned.contains(&scs) { return None; }
    // SAFETY: only used bytes of this thread's own live allocation. The end
    // pointer may equal its guard boundary; no byte at that pointer is read.
    Some(unsafe {
        std::slice::from_raw_parts(*owned.start() as *const u8,
                                  (scs - *owned.start()) as usize).to_vec()
    })
}`],
  ['    w.bytes(&shadow_stack());','    w.opt(shadow_stack(), |w, scs| w.bytes(&scs));'],
  ['    let scs = r.bytes();',`    let scs = r.opt(|r| r.bytes());
    let Some(owned_scs) = context::owned_scs_range() else {
        return "fork child: missing owned shadow stack".into();
    };
    if scs.as_ref().is_some_and(|bytes| bytes.len() as u64 > *owned_scs.end() - *owned_scs.start()) {
        return "fork child: shadow stack exceeds owned allocation".into();
    }`],
  [`    let base = context::guest_scs();
    // SAFETY: our fresh shadow stack is larger than any parent's in use.
    unsafe { std::ptr::copy_nonoverlapping(scs.as_ptr(), base as *mut u8, scs.len()) };
    context::set_guest_scs(base + scs.len() as u64);`,`    if let Some(scs) = scs {
        let base = *owned_scs.start();
        // SAFETY: the private fork payload was bounded above against this
        // context's actual allocation before any guest state was restored.
        unsafe { std::ptr::copy_nonoverlapping(scs.as_ptr(), base as *mut u8, scs.len()) };
        c.x[18] = base + scs.len() as u64;
    }
    // Non-owned x18 remains the original architectural value. Guest-owned
    // data or shadow-stack mappings were restored at their original address.
    context::set_guest_scs(c.x[18]);`],
 ]);
}
