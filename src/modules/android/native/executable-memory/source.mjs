import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../upstream/source-files.mjs';
import {prepareAimThreadPointerEmitter} from '../thread-pointer-source.mjs';
import {prepareAshmemCapability} from '../../art/ashmem-access-source.mjs';

/** Adds the native ABI to an owned original AIM projection. The original JIT
 * allocator, migration and instruction-store implementation stay authoritative. */
export function prepareExecutableMemoryApi(aim){
 prepareAshmemCapability(aim);
 // Retain the original FIONREAD branches on stable Rust. An if-let match
 // guard requires an unstable language feature; the nested match has the
 // same evaluation order, result and error propagation.
 const ioctl=path.join(aim,'crates/aim-linux-abi/src/sys/fs.rs');
 let ioctlSource=fs.readFileSync(ioctl,'utf8');
 const guarded=`                    None if let Some(r) = super::net::fionread(fd) => match r {
                        Ok(n) => n,
                        Err(error) => return error,
                    },
                    None => {
                        let mut n = 0i32;
                        if libc::ioctl(fd, libc::FIONREAD, &mut n) < 0 {
                            return -(errno::last() as i64);
                        }
                        n
                    }`;
 const stable=`                    None => match super::net::fionread(fd) {
                        Some(Ok(n)) => n,
                        Some(Err(error)) => return error,
                        None => {
                            let mut n = 0i32;
                            if libc::ioctl(fd, libc::FIONREAD, &mut n) < 0 {
                                return -(errno::last() as i64);
                            }
                            n
                        }
                    }`;
 if(ioctlSource.split(guarded).length!==2)throw Error('AIM pending-byte source boundary changed');
 writeChanged(ioctl,ioctlSource.replace(guarded,stable));
 const root=path.join(aim,'crates/aim-linux-abi/src/sys');
 const file=path.join(root,'mod.rs'),source=fs.readFileSync(file,'utf8'),needle='mod jit;';
 if(source.split(needle).length!==2)throw Error('AIM executable-memory owner changed');
 copyChanged(path.join(import.meta.dirname,'aim-api.rs'),path.join(root,'guest-executable-memory.rs'));
 writeChanged(file,source.replace(needle,needle+'\n#[path = "guest-executable-memory.rs"]\npub mod guest_executable_memory;'));
 // Keep AIM's TP instruction emitter authoritative. A co-resident executor
 // allocates its own key instead of claiming AIM's process-wide fixed slots.
 prepareAimThreadPointerEmitter(path.join(root,'../a64.rs'));
 // The physical kernel tag separates guest JIT pages from a co-resident VM's
 // JIT cache. The original allocator and migration algorithm stay unchanged.
 const jitFile=path.join(root,'jit.rs'),jit=fs.readFileSync(jitFile,'utf8');
 const allocation='libc::MAP_PRIVATE | libc::MAP_ANON | libc::MAP_JIT,\n            -1,';
 if(jit.split(allocation).length!==2)throw Error('AIM JIT allocation boundary changed');
 let adapted=jit.replace(allocation,'libc::MAP_PRIVATE | libc::MAP_ANON | libc::MAP_JIT,\n            libc::VM_MAKE_TAG(libc::VM_MEMORY_APPLICATION_SPECIFIC_1 as u8) as i32,');
 const entry='pub fn fault(ctx: *mut GuestContext, m: &mut DarwinMcontext) -> bool {';
 if(adapted.split(entry).length!==2)throw Error('AIM JIT fault boundary changed');
 adapted=adapted.replace(entry,entry+'\n    fault_with_owner(ctx, m, |_| true)\n}\n\npub fn fault_with_owner(ctx: *mut GuestContext, m: &mut DarwinMcontext, owns: impl Fn(u64) -> bool) -> bool {');
 const query='    let Some(region) = jit_region(m.far) else {';
 if(adapted.split(query).length!==2)throw Error('AIM JIT ownership boundary changed');
 adapted=adapted.replace(query,'    if !owns(m.far) { return false; }\n'+query);
 const migrate='pub fn protect_around(addr: u64, len: u64, prot: i32) -> i64 {';
 if(adapted.split(migrate).length!==2)throw Error('AIM JIT migration boundary changed');
 adapted=adapted.replace(migrate,`${migrate}
    protect_around_prepared(addr,len,prot,None,None)
}
pub fn protect_around_prepared(addr:u64,len:u64,prot:i32,
    prepare:Option<unsafe extern "C" fn(usize,*mut libc::c_void,usize)->i64>,
    finish:Option<unsafe extern "C" fn(usize,*mut libc::c_void,usize,i32)>)->i64 {
    // A preparation error must precede every live mapping change. Snapshot the
    // complete mapped request and publish it with the original single remap.
    if prepare.is_some() && prot & libc::PROT_EXEC != 0 {
        let end=addr+len;let mut at=addr;let mut keep=false;
        while at<end {
            let Some(info)=vmmap::info_at(at) else{return -(ENOMEM as i64);};
            if info.start>at{return -(ENOMEM as i64);}
            keep|=!info.empty;at=info.end.min(end);
        }
        return unjit(addr,len,prot,keep,prepare,finish);
    }`);
 const dispatch='        let r = if i.prot as i32 & RWX == RWX {\n            unjit(cur, hi - cur, prot, !i.empty)';
 if(adapted.split(dispatch).length!==2)throw Error('AIM JIT migration dispatch changed');
 adapted=adapted.replace(dispatch,`        let r = if i.prot as i32 & RWX == RWX ||
            (prepare.is_some() && prot & libc::PROT_EXEC != 0 &&
             (i.prot & libc::PROT_EXEC as u32 == 0 || i.prot & libc::PROT_WRITE as u32 != 0)) {
            unjit(cur,hi-cur,prot,!i.empty,prepare,finish)`);
 const unjit='fn unjit(lo: u64, n: u64, prot: i32, keep: bool) -> i64 {';
 if(adapted.split(unjit).length!==2)throw Error('AIM JIT staging boundary changed');
 adapted=adapted.replace(unjit,`fn unjit(lo:u64,n:u64,prot:i32,keep:bool,
    prepare:Option<unsafe extern "C" fn(usize,*mut libc::c_void,usize)->i64>,
    finish:Option<unsafe extern "C" fn(usize,*mut libc::c_void,usize,i32)>)->i64 {`);
 const protect='        libc::mprotect(tmp, n as usize, prot);\n    }\n    let (mut at, mut cur, mut max) = (lo, 0, 0);';
 if(adapted.split(protect).length!==2)throw Error('AIM JIT staging protection changed');
 adapted=adapted.replace(protect,`    }
    if keep && prot & libc::PROT_EXEC != 0 && let Some(prepare)=prepare {
        let result=unsafe{prepare(lo as usize,tmp,n as usize)};
        if result<0 {unsafe{if let Some(finish)=finish{finish(lo as usize,tmp,n as usize,0);}libc::munmap(tmp,n as usize);}return result;}
    }
    if unsafe{libc::mprotect(tmp,n as usize,prot)}!=0 {
        let result=errno::check(-1);unsafe{if let Some(finish)=finish{finish(lo as usize,tmp,n as usize,0);}libc::munmap(tmp,n as usize);}return result;
    }
    let (mut at, mut cur, mut max) = (lo, 0, 0);`);
 const copied='    unsafe {\n        if keep {\n            std::ptr::copy_nonoverlapping(lo as *const u8, tmp as *mut u8, n as usize);\n        }\n    }';
 if(adapted.split(copied).length!==2)throw Error('AIM JIT snapshot boundary changed');
 adapted=adapted.replace(copied,`    if keep {
        let result=read_publication_snapshot(lo,n,tmp);
        if result<0 {unsafe{libc::munmap(tmp,n as usize);}return result;}
    }`);
 const published='    unsafe { libc::munmap(tmp, n as usize) };';
 if(adapted.split(published).length!==2)throw Error('AIM JIT publication completion changed');
 adapted=adapted.replace(published,'    unsafe { if let Some(finish)=finish{finish(lo as usize,tmp,n as usize,i32::from(kr==0));}libc::munmap(tmp,n as usize) };');
 adapted+=`
// A COW alias preserves source permissions, including ordinary PROT_NONE
// pages. JIT aliases already have read permission; the original pages never
// acquire temporary read/write permissions during failed preparation.
fn read_publication_snapshot(lo:u64,n:u64,tmp:*mut libc::c_void)->i64 {
    let(mut alias,mut current,mut maximum)=(0,0,0);
    let result=unsafe{vm::mach_vm_remap(vm::task(),&mut alias,n,0,libc::VM_FLAGS_ANYWHERE,
        vm::task(),lo,1,&mut current,&mut maximum,vm::VM_INHERIT_COPY)};
    if result!=0{return -(ENOMEM as i64);}
    let end=alias+n;let mut at=alias;let mut error=0;
    while at<end {
        let Some(info)=vmmap::info_at(at) else{error=-(ENOMEM as i64);break;};
        if info.start>at{error=-(ENOMEM as i64);break;}
        let hi=info.end.min(end);
        if info.prot & libc::PROT_READ as u32==0 {
            error=errno::check(unsafe{libc::mprotect(at as *mut _,(hi-at)as usize,libc::PROT_READ)}as i64);
            if error<0{break;}
        }
        at=hi;
    }
    if error==0{unsafe{std::ptr::copy_nonoverlapping(alias as *const u8,tmp.cast::<u8>(),n as usize);}}
    unsafe{libc::munmap(alias as *mut _,n as usize);}error
}
`;
 writeChanged(jitFile,adapted);
 const frameFile=path.join(root,'sigframe.rs');let frame=fs.readFileSync(frameFile,'utf8');
 const size='const FRAME_SIZE: u64 =';
 if(frame.split(size).length!==2)throw Error('AIM signal-frame extent changed');
 frame=frame.replace(size,'pub const FRAME_SIZE: u64 =');
 const context='const UC: u64 =';
 if(frame.split(context).length!==2)throw Error('AIM signal-context offset changed');
 frame=frame.replace(context,'pub const UC: u64 =');
 const placement='    let record = sp.checked_sub(16).ok_or(crate::errno::EFAULT)? & !15;';
 if(frame.split(placement).length!==2)throw Error('AIM signal-frame placement changed');
 // Original guest copies retain their pager/handler owner. The co-resident
 // executor supplies native stack storage and must not install AIM handlers.
 frame=frame.replace(placement,`    unsafe { setup_frame(cpu, d, &saved_alt, sp, super::user_memory::write_exact) }
}

pub unsafe fn setup_on_stack(cpu: &Cpu, d: &Delivery, saved_alt: &AltStack, sp: u64) -> Result<Entry, i32> {
    unsafe { setup_frame(cpu,d,saved_alt,sp,|address,bytes|{
        std::ptr::copy_nonoverlapping(bytes.as_ptr(),address as *mut u8,bytes.len());Ok(())
    }) }
}
unsafe fn setup_frame(cpu:&Cpu,d:&Delivery,saved_alt:&AltStack,sp:u64,
    write:impl Fn(u64,&[u8])->Result<(),i32>)->Result<Entry,i32> {
${placement}`);
 const write='    super::user_memory::write_exact(f, &bytes)?;';
 if(frame.split(write).length!==2)throw Error('AIM signal-frame copy boundary changed');
 frame=frame.replace(write,'    write(f, &bytes)?;');
 const read='    let bytes = super::user_memory::read_exact(f, FRAME_SIZE as usize).ok()?;';
 if(frame.split(read).length!==2)throw Error('AIM signal-frame snapshot boundary changed');
 frame=frame.replace(read,`${read}
    unsafe { restore_bytes(&bytes) }
}
pub unsafe fn restore_native(f:u64)->Option<(Cpu,u64,AltStack)>{
    if f&15!=0{return None;}
    unsafe{restore_bytes(std::slice::from_raw_parts(f as *const u8,FRAME_SIZE as usize))}
}
unsafe fn restore_bytes(bytes:&[u8])->Option<(Cpu,u64,AltStack)>{`);
 frame+=`
// Native disposition chaining keeps the caller's Linux ucontext in place.
// The original layout constants and FPSIMD record walker remain authoritative.
pub unsafe fn chained_context_fault(uc: u64) -> (u64, u32) {
    let f=uc-UC;
    let mut esr=0;let mut p=f+MC_RESERVED;let end=p+RESERVED_SIZE;
    unsafe {
        while p+8<=end {
            let magic=get::<u32>(p);let size=get::<u32>(p+4) as u64;
            if magic==0 || size<16 || size%16!=0 || p+size>end { break; }
            if magic==ESR_MAGIC && size==ESR_SIZE as u64 { esr=get::<u64>(p+8) as u32; }
            p+=size;
        }
        (get(MC_FAULT+f),esr)
    }
}
pub unsafe fn update_chained_context(uc: u64,cpu: &Cpu,mask: u64) {
    let f=uc-UC;
    unsafe {
        put(UC_SIGMASK+f,mask);
        for (i,r) in cpu.x.iter().enumerate(){put(MC_REGS+f+i as u64*8,*r);}
        put(MC_SP+f,cpu.sp);put(MC_PC+f,cpu.pc);put(MC_PSTATE+f,cpu.pstate);
        let mut p=MC_RESERVED+f;let end=p+RESERVED_SIZE;
        while p+8<=end {
            let magic=get::<u32>(p);let size=get::<u32>(p+4) as u64;
            if magic==0 || size<16 || size%16!=0 || p+size>end { break; }
            if magic==FPSIMD_MAGIC && size==FPSIMD_SIZE as u64 {
                put(p+8,cpu.fpsr);put(p+12,cpu.fpcr);
                std::ptr::copy_nonoverlapping(cpu.v.as_ptr().cast::<u8>(),(p+16) as *mut u8,512);
            }
            p+=size;
        }
    }
}
`;
 writeChanged(frameFile,frame);
}
