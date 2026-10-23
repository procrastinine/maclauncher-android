import fs from 'node:fs';import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';
/** Add original shared-object execution after the optional initial/writer owners. */
export function prepareSharedCode(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src/sys');const pending=new Map();
 const edit=(name,changes)=>{const file=path.join(root,name);let text=pending.get(file)??fs.readFileSync(file,'utf8');for(const[from,to,count=1]of changes){if(text.split(from).length!==count+1)throw Error('Original shared code boundary changed: '+name+' '+from.slice(0,90));text=text.split(from).join(to);}pending.set(file,text);};
 if(!fs.readFileSync(path.join(root,'mod.rs'),'utf8').includes('pub(crate) mod native_writer;'))throw Error('Shared code requires the native writer owner');
 edit('mod.rs',[[ 'mod jit;','mod jit;\n#[path="shared-code.rs"]\npub(crate) mod shared_code;' ]]);
 edit('mutable-memory.rs',[
  ['#[cfg(test)]\nmod tests {','include!("shared-memory.rs");\n\n#[cfg(test)]\nmod tests {'],
  ['    if unsafe{libc::mprotect(start as*mut _,length as usize,protection&R)}!=0',`    if protection&W!=0{
        for(&at,region)in owner.ranges.range(..end){if region.end<=start{continue;}let alias=region.backing.address+region.offset+at.max(start)-at;
            if super::vmmap::info_at(alias).is_none_or(|map|map.start>alias||map.prot&libc::PROT_WRITE as u32==0){return Some(Err(std::io::Error::from_raw_os_error(libc::EACCES)));}
        }
    }
    if unsafe{libc::mprotect(start as*mut _,length as usize,protection&R)}!=0`],
  ['let alias=vm::alias(start,length,None,R|W).expect("original mutable fork alias");owner.ranges.insert(start,Region{end,offset:0,protection,generation:NEXT.fetch_add(1,Ordering::AcqRel),backing:Arc::new(Backing::new(alias,length,false))});','let backing=original_alias(start,length,protection).expect("original mutable fork alias");owner.ranges.insert(start,Region{end,offset:0,protection,generation:NEXT.fetch_add(1,Ordering::AcqRel),backing});'],
 ]);
 edit('memfd.rs',[[ '/// mprotect adding PROT_EXEC over',`pub(super) fn check_shared_execution(fd:i32)->Result<(),i64>{
    if memfd_stat(fd).is_some()&&read_seals(fd)&F_SEAL_EXEC!=0{return Err(-(libc::EACCES as i64));}Ok(())
}

/// mprotect adding PROT_EXEC over` ]]);
 edit('mem.rs',[
  ['fn mmap_kernel(a: [u64; 6]) -> i64 {','fn mmap_kernel(a: [u64; 6]) -> i64 {\n    if let Some(result)=super::shared_code::mmap(a,mmap_kernel){return result;}'],
  ['    if let Some(result)=mutable_protect(a){return result;}','    if let Some(result)=super::shared_code::mprotect(a,mprotect_owned){return result;}\n    if let Some(result)=mutable_protect(a){return result;}'],
  ['        super::verity_pager::validate_protection(start,length,protection)?;','        super::verity_pager::validate_protection(start,length,protection)?;\n        if super::shared_code::shared(start,length){return super::shared_code::enroll(start,length,protection);}'],
  ['        if cursor<lo{let result=discard_private_kernel(cursor,lo-cursor);if result<0{return result;}}','        if cursor<lo{let result=discard_private_kernel(cursor,lo-cursor);if result<0{return result;}}\n        if super::shared_code::shared(lo,hi-lo){cursor=hi;continue;}'],
  ['    let prot = r.prot as u64 & (PROT_READ | PROT_WRITE | PROT_EXEC);','    let prot=if super::shared_code::genuine(&r){super::mutable_memory::protection(like,r.prot as i32)as u64}else{r.prot as u64}&(PROT_READ|PROT_WRITE|PROT_EXEC);'],
 ]);
 // Original shared pagers can fail after a truncate. Their actual CPU
 // accesses use the existing scoped recovery path; private RAM stays direct.
 for(const name of ['mutable-code.rs','native-writer.rs'])edit(name,[[
  'permissions:lease.protection as u32,flags:0',
  'permissions:lease.protection as u32,flags:if lease.private_origin{0}else if super::super::shared_code::shared(address,1){2}else{0}'
 ]]);
 edit('fork/spawn.rs',[
  ['        at = i.end;\n        let start = i.start.max(arena::LO);\n        let len = i.end.min(arena::HI) - start;',
   '        let start=i.start.max(arena::LO).max(at);\n        let end=super::super::shared_code::fork_end(start,i.end.min(arena::HI));\n        let len=end-start;at=end;'],
  ['    let perms: &[i32] = match backing {',
   '    // Source only an owned shared entry from its original retained alias.\n    // The child still maps the original public protection and sharing.\n    let lease=if backing==Backing::Share{super::super::shared_code::fork_source(start,len)}else{None};\n    let(start,max_prot)=if let Some(lease)=&lease{let alias=lease.address+start-lease.start;let info=vmmap::info_at(alias).ok_or(-(ENOMEM as i64))?;(alias,max_prot&info.max_prot as i32)}else{(start,max_prot)};\n    let perms: &[i32] = match backing {'],
  ['    let pager=super::super::verity_pager::ForkSnapshot::capture()',
   '    let mutable_snapshot=super::super::shared_code::fork_guard();\n    let pager=super::super::verity_pager::ForkSnapshot::capture()'],
  ['    state::save(&mut w,&mount_text);',
   '    state::save(&mut w,&mount_text);\n    drop(mutable_snapshot);'],
 ]);
 for(const name of ['shared-code','shared-memory'])copyChanged(path.join(import.meta.dirname,'native',name+'.rs'),path.join(root,name+'.rs'));
 for(const[file,text]of pending)writeChanged(file,text);
}
