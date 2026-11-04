import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** Use the original credentials and ptrace owners with the shared live lease. */
export function prepareProcessPolicy(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{const file=path.join(root,name);let source=fs.readFileSync(file,'utf8');
  for(const[from,to]of changes){if(source.split(from).length!==2)throw Error('Original process-policy boundary changed: '+name+' '+from.slice(0,60));source=source.replace(from,to);}
  writeChanged(file,source);
 };
 edit('sys/misc.rs',[
  ['    DUMPABLE.load(Relaxed) == 1','    crate::process_identity::get_dumpable().unwrap_or_else(||DUMPABLE.load(Relaxed) as i64) == 1'],
  ['    DUMPABLE.store(1, Relaxed);','    assert_eq!(set_dumpable(1),0,"Guest exec policy could not be published");'],
  ['        w.u64(v.load(Relaxed));','        w.u64(if std::ptr::eq(v,&DUMPABLE){dumpable() as u64}else{v.load(Relaxed)});'],
  ['        v.store(r.u64(), Relaxed);','        let value=r.u64();\n        if std::ptr::eq(v,&DUMPABLE){assert_eq!(set_dumpable(value),0,"Guest fork policy could not be restored");}else{v.store(value,Relaxed);}'],
  ['pub fn prctl(a: [u64; 6]) -> i64 {',`pub(super) fn set_dumpable(value:u64)->i64{
    if value>1{return -(EINVAL as i64);}
    if let Some(result)=crate::process_identity::set_dumpable(value as i32){if result<0{return result;}}
    DUMPABLE.store(value,Relaxed);0
}

pub fn prctl(a: [u64; 6]) -> i64 {`],
  ['        PR_SET_DUMPABLE if a[1] <= 1 => {\n            DUMPABLE.store(a[1], Relaxed);\n            0\n        }\n        PR_GET_DUMPABLE => DUMPABLE.load(Relaxed) as i64,','        PR_SET_DUMPABLE => set_dumpable(a[1]),\n        PR_GET_DUMPABLE => crate::process_identity::get_dumpable().unwrap_or_else(||DUMPABLE.load(Relaxed) as i64),'],
 ]);
 const credentialHelper=`/// Changes of effective/filesystem ids clear dumpability before publishing the
/// new credentials. An authority failure rolls the credential change back.
fn with_credentials(f:impl FnOnce(&mut Identity)->i64)->i64{
    with(|id|{
        let before=id.clone();let result=f(id);
        if result<0{*id=before;return result;}
        if [id.uid[1],id.uid[3],id.gid[1],id.gid[3]] != [before.uid[1],before.uid[3],before.gid[1],before.gid[3]]{
            let policy=super::misc::set_dumpable(0);
            if policy<0{*id=before;return policy;}
        }
        result
    })
}

/// Validate the actual native connection peer, never a privilege bit supplied
/// in its packet. Credential and namespace checks remain independent of the
/// host sandbox and task-port restrictions.
pub(super) fn may_inspect_from(peer:i32,fs:bool)->bool{
    if peer==unsafe{libc::getpid()}{return true;}
    if !super::pidns::contains(peer){return false;}
    let Some(caller)=identity_of(peer)else{return false;};
    let target=current();
    let (uid,gid)=if fs{(caller.uid[3],caller.gid[3])}else{(caller.uid[0],caller.gid[0])};
    let privileged=caller.capable(CAP_SYS_PTRACE);
    (privileged||(target.uid[..3].iter().all(|&v|v==uid)&&target.gid[..3].iter().all(|&v|v==gid)))
        &&(super::misc::dumpable()||privileged)
}

`;
 edit('sys/cred.rs',[
  ['/// A copy of this process\'s identity.',credentialHelper+'/// A copy of this process\'s identity.'],
  ['pub fn may_ptrace(pid: i32, fs: bool) -> bool {\n    let t = target(pid);',`pub fn may_ptrace(pid: i32, fs: bool) -> bool {
    if pid==unsafe{libc::getpid()}{return true;}
    let dumpable=crate::process_identity::dumpable_of_host(pid).unwrap_or(true);
    let t = target(pid);`],
  ['            || (t.uid[..3].iter().all(|&x| x == u) && t.gid[..3].iter().all(|&x| x == g))','            || (dumpable && t.uid[..3].iter().all(|&x| x == u) && t.gid[..3].iter().all(|&x| x == g))'],
 ]);
 // Only the four id-changing syscalls use the transactional credential helper.
 const credFile=path.join(root,'sys/cred.rs');let cred=fs.readFileSync(credFile,'utf8');
 for(const name of ['setid','setreid','setresid','setfsid']){
  const start=cred.indexOf('pub fn '+name+'('),end=cred.indexOf('\n}\n',start)+3;
  if(start<0||end<3)throw Error('Original credential setter changed: '+name);
  const before=cred.slice(start,end);if(before.split('    with(|id| {').length!==2)throw Error('Original credential transaction changed: '+name);
  cred=cred.slice(0,start)+before.replace('    with(|id| {','    with_credentials(|id| {')+cred.slice(end);
 }
 writeChanged(credFile,cred);
 // process_vm's caller has already resolved a thread to its physical owner.
 // Resolve the stock namespace's special init ID here exactly once as well.
 edit('sys/mem.rs',[
  ['    let owner = super::thread::owner(pid);',`    let owner = if pid==1&&!crate::process_identity::enabled(){
        match super::pidns::host_pid(pid){Ok(host)=>host,Err(error)=>return error}
    }else{super::thread::owner(pid)};`],
 ]);
 edit('sys/ptrace/mod.rs',[
  ['    let pid = match pidns::syscall_pid(pid) {\n        Ok(pid) => pid,\n        Err(error) => return error,\n    };\n    if let Err(e) = pidns::check(pid) {','    if let Err(e) = pidns::check(pid) {'],
 ]);
 edit('sys/ptrace/agent.rs',[
  ['    match op {\n        CGROUP_POLICY => {',`    if matches!(op,SEIZE|PEEK|VM|MEM_OPEN|MEM_IO|MAPS|SMAPS|FDS|FD_LINK)
        && !super::super::cred::may_inspect_from(peer,matches!(op,MAPS|SMAPS|FDS|FD_LINK)){
        return err(EPERM);
    }
    match op {
        CGROUP_POLICY => {`],
  ['            let (tid, options, privileged) = (r.i32(), r.u64(), r.bool());','            let (tid, options, _claimed_privilege) = (r.i32(), r.u64(), r.bool());'],
  ['            if g.traced.contains_key(&tid) || !(misc::dumpable() || privileged) {','            if g.traced.contains_key(&tid) {'],
  ['            if !(misc::dumpable() || r.bool()) {\n                return err(crate::errno::EACCES);\n            }','            let _claimed_privilege=r.bool();'],
  ['use crate::sys::{fdtab, misc, procfs, smaps, thread};','use crate::sys::{fdtab, procfs, smaps, thread};'],
 ]);
 edit('sys/procfs.rs',[
  ['    Some(match rest {\n        "" => {',`    if !me && matches!(rest,"auxv"|"environ"|"exe"|"cwd"|"root") && !super::cred::may_ptrace(p,true){return None;}
    Some(match rest {
        "" => {`],
 ]);
}
