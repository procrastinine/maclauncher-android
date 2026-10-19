import fs from 'node:fs';
import {writeChanged} from './upstream/source-files.mjs';

/** The original AIM TP emitter serves both executors. The hosting executor
 * supplies its owned TP/shadow keys; ordinary AIM callers retain its defaults. */
export function prepareAimThreadPointerEmitter(file){
 let source=fs.readFileSync(file,'utf8');
 const replace=(from,to)=>{if(source.split(from).length!==2)throw Error('AIM thread-pointer emitter changed: '+from);source=source.replace(from,to);};
 const entry='pub fn stub_body(kind: Kind, rt: u32, ctr_el0: u32) -> Vec<u32> {';
 replace(entry,entry+'\n    stub_body_with_tp_slots(kind,rt,ctr_el0,slot::TP,slot::SCS)\n}\n\npub fn stub_body_with_tp_slots(kind: Kind, rt: u32, ctr_el0: u32, tp_slot: u32, shadow_slot: u32) -> Vec<u32> {');
 replace('        Kind::MrsTp => vec![MRS_TPIDRRO_EL0 | rt, ldr_uimm(rt, rt, slot::TP)],',`        Kind::MrsTp if rt == 18 => vec![
            stp_pre(16,17,SP,-16), MRS_TPIDRRO_EL0|16,
            ldr_uimm(17,16,tp_slot), str_uimm(17,16,shadow_slot), ldp_post(16,17,SP,16)],
        Kind::MrsTp => vec![MRS_TPIDRRO_EL0 | rt, ldr_uimm(rt, rt, tp_slot)],`);
 replace('        Kind::MsrTp => {',`        Kind::MsrTp if rt == 18 => vec![
            stp_pre(16,17,SP,-16), MRS_TPIDRRO_EL0|16,
            ldr_uimm(17,16,shadow_slot), str_uimm(17,16,tp_slot), ldp_post(16,17,SP,16)],
        Kind::MsrTp => {`);
 replace('str_uimm(rt, scratch, slot::TP)','str_uimm(rt, scratch, tp_slot)');
 writeChanged(file,source);
}
