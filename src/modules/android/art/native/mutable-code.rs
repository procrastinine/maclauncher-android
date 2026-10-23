// Fault continuation for original mutable code. Ordinary code still executes
// directly. Original-code fetch and native-writer continuation are distinct:
// a native writer can be interrupted with a veneer scratch frame still live.
use super::sigframe::DarwinMcontext;
use crate::context::GuestContext;
pub(crate) fn available()->bool{cfg!(maclauncher_precise)}
pub(crate) fn fault(ctx:*mut GuestContext,m:&mut DarwinMcontext)->bool{
    #[cfg(maclauncher_precise)]{return unsafe{engine::fault(ctx,m)};}
    #[cfg(not(maclauncher_precise))]{let _=(ctx,m);false}
}
pub(crate) fn native_fault(signal:i32,code:i32,address:u64,mask:*const std::ffi::c_void)->bool{
    #[cfg(maclauncher_precise)]{return unsafe{engine::native_fault(signal,code,address,mask)};}
    #[cfg(not(maclauncher_precise))]{let _=(signal,code,address,mask);false}
}
#[allow(dead_code)]
pub(crate) enum MappingMutation{Shared(super::verity_pager::VmMutation),Exclusive(super::verity_pager::VmExclusive)}
pub(crate) fn mapping_mutation()->MappingMutation{if available(){MappingMutation::Exclusive(super::verity_pager::exclusive_mutation())}else{MappingMutation::Shared(super::verity_pager::mutation())}}
pub(crate) fn resume_dispatch(){
    #[cfg(maclauncher_precise)]engine::resume_dispatch();
}
#[cfg(maclauncher_precise)]
mod engine {
    use super::*;
    use crate::{context,errno,sys};
    use super::super::{mutable_memory as memory,verity_pager::{self,Access}};
    use std::{cell::{Cell,RefCell,UnsafeCell},ffi::c_void,sync::atomic::{AtomicBool,AtomicU64,Ordering}};
    const R:i32=libc::PROT_READ;const W:i32=libc::PROT_WRITE;const X:i32=libc::PROT_EXEC;
    use super::super::precise_ffi::*;
    #[derive(Default)]
    struct Memory {leases:Vec<memory::Lease>,pending:Option<(u64,i32)>,wrote:bool,regions:Vec<super::super::vmmap::Region>}
    impl Memory {
        // This classification is valid only within the caller's held VM epoch.
        // Unique anonymous RW/non-X objects cannot alias executable source.
        // SM_EMPTY has no backing object yet; its first write creates private
        // zero-filled memory. Shared, COW, file and unknown objects still stop.
        fn private_data(&mut self,base:u64,length:u64)->bool{
            let Some(end)=base.checked_add(length)else{return false;};
            if length==0||memory::overlaps(base,length){return false;}
            let eligible=|region:&super::super::vmmap::Region|{
                use super::super::vmmap::{SM_EMPTY,SM_PRIVATE};
                let usage=region.usage;
                let private=usage.share_mode==SM_PRIVATE&&usage.ref_count==1;
                let empty=usage.share_mode==SM_EMPTY&&usage.ref_count==0&&usage.resident==0&&usage.swapped_out==0;
                region.prot==(R|W)as u32&&!region.shared&&region.file.is_none()&&(private||empty)
            };
            if let Some(region)=self.regions.iter().find(|region|region.start<=base&&region.end>=end){return eligible(region);}
            let Some(region)=super::super::vmmap::region_at(base)else{return false;};
            let result=region.start<=base&&region.end>=end&&eligible(&region);self.regions.push(region);result
        }
    }
    struct Cpu{handle:*mut c_void,memory:Box<Memory>}
    impl Drop for Cpu{fn drop(&mut self){unsafe{kl_a64_precise_destroy(self.handle);}}}
    struct Fault{state:State,raw:bool}
    struct FaultSlot{busy:AtomicBool,value:UnsafeCell<Option<Fault>>}
    thread_local!{
        static FAULT:FaultSlot=const{FaultSlot{busy:AtomicBool::new(false),value:UnsafeCell::new(None)}};
        static CPU:RefCell<Option<Cpu>>=const{RefCell::new(None)};
        static DEPTH:Cell<usize>=const{Cell::new(0)};
        static DISPATCH:Cell<usize>=const{Cell::new(0)};
    }
    pub(super) unsafe fn native_fault(signal:i32,code:i32,address:u64,mask:*const c_void)->bool{unsafe{kl_a64_precise_native_fault(signal,code,address,mask)!=0}}
    static STEP:AtomicU64=AtomicU64::new(1);
    fn access(value:i32)->Access{if value&X!=0{Access::Execute}else if value&W!=0{Access::Write}else{Access::Read}}
    unsafe extern "C" fn map(opaque:*mut c_void,address:u64,length:usize,required:u32,result:*mut Mapping)->i32{
        let owner=unsafe{&mut*opaque.cast::<Memory>()};let required=required as i32;let _=length;
        if let Some(lease)=memory::mapping(address,1,required){
            let page=address&!4095;let end=(page+4096).min(lease.start+lease.length);
            unsafe{*result=Mapping{address:page,length:end-page,host:(lease.address+page-lease.start)as*mut _,permissions:lease.protection as u32,flags:8};}
            owner.leases.push(lease);return 0;
        }
        if memory::covers(address,1){return -errno::EFAULT;}
        let Some((start,stop,protection,_))=crate::patch::vm::region(address)else{return -errno::EFAULT;};
        if start>address||stop<=address||protection&required!=required{
            if verity_pager::fault_intent(address,access(required)).is_some(){owner.pending=Some((address,required));return -errno::EAGAIN;}
            return -errno::EFAULT;
        }
        // Only the accessed original page is admitted. Actual VM mutation is
        // excluded by the caller's epoch. Retained views are inert between
        // intervals and must reacquire this authority before their next use.
        let page=address&!4095;let end=(page+4096).min(stop);
        if page<start{return -errno::EFAULT;}
        let flags=if protection&7==7&&sys::arena::contains(page,end-page){1}else{2|if protection==(R|W)&&owner.private_data(page,end-page){4}else{0}};
        unsafe{*result=Mapping{address:page,length:end-page,host:page as*mut _,permissions:protection as u32,flags};}0
    }
    // The precise owner retains no lease across its public return. Reacquire
    // the complete original view under the next VM/source transaction before
    // comparing bytes or using a cached translation. Changed bounds, backing,
    // permissions or ownership are rejected by the precise owner itself.
    unsafe extern "C" fn revalidate(opaque:*mut c_void,address:u64,length:usize,result:*mut Mapping)->i32{
        let error=unsafe{map(opaque,address,length,0,result)};
        if error!=0{return error;}
        let current=unsafe{&*result};
        if current.address!=address||current.length!=length as u64{return -errno::EFAULT;}0
    }
    unsafe extern "C" fn write(opaque:*mut c_void,address:u64,length:usize){
        let owner=unsafe{&mut*opaque.cast::<Memory>()};owner.wrote|=memory::overlaps(address,length as u64);
    }
    unsafe extern "C" fn counter(_: *mut c_void)->u64{aim_hostcall::clock::monotonic_ns()as u64}
    fn with_cpu<T>(operation:impl FnOnce(&mut Cpu)->T)->T{
        CPU.with(|slot|{
            let mut slot=slot.borrow_mut();let cpu=slot.get_or_insert_with(||{
                let mut memory=Box::<Memory>::default();let config=Config{owner:(&mut*memory)as*mut _ as*mut _,map,counter,ctr:crate::a64::host_ctr_el0(),frequency:1_000_000_000,dczid:4,write};
                let handle=unsafe{kl_a64_precise_create(config)};assert!(!handle.is_null(),"original A64 precise component");
                assert_eq!(unsafe{kl_a64_precise_set_owned_cache(handle,Some(revalidate))},0,"original A64 source revalidation");Cpu{handle,memory}
            });operation(cpu)
        })
    }
    fn clear_exclusive(){with_cpu(|cpu|assert_eq!(unsafe{kl_a64_precise_clear_exclusive(cpu.handle)},0,"original A64 exclusive reset"));}
    impl State {
        fn from_mc(m:&DarwinMcontext,raw:bool)->Self{
            let mut x=[0;31];x[..29].copy_from_slice(&m.x);x[29]=m.fp;x[30]=m.lr;if !raw{x[18]=context::guest_scs();}
            let native_tp:u64;unsafe{std::arch::asm!("mrs {}, tpidrro_el0",out(reg)native_tp,options(nomem,nostack,preserves_flags));}
            Self{x,sp:m.sp,pc:m.pc,v:m.v.map(|v|[v as u64,(v>>64)as u64]),tp:context::guest_tp(),readonly_tp:if raw{native_tp}else{0},nzcv:m.cpsr&0xf0000000,fpcr:m.fpcr,fpsr:m.fpsr}
        }
        unsafe fn from_context(ctx:*mut GuestContext)->Self{
            let c=unsafe{&*ctx};Self{x:c.x,sp:c.sp,pc:c.resume_pc(),v:c.v.map(|v|[v as u64,(v>>64)as u64]),tp:context::guest_tp(),readonly_tp:0,nzcv:c.nzcv as u32,fpcr:c.fpcr as u32,fpsr:c.fpsr as u32}
        }
        unsafe fn to_context(&self,ctx:*mut GuestContext,raw:bool){
            let c=unsafe{&mut*ctx};c.x=self.x;if raw{c.x[18]=context::guest_scs();}else{context::set_guest_scs(self.x[18]);context::set_guest_tp(self.tp);}
            c.pc=self.pc;c.stub_ret=0;c.sp=self.sp;c.v=self.v.map(|v|(v[0]as u128)|((v[1]as u128)<<64));c.nzcv=self.nzcv as u64;c.fpcr=self.fpcr as u64;c.fpsr=self.fpsr as u64;
        }
    }
    pub(super) unsafe fn fault(ctx:*mut GuestContext,m:&mut DarwinMcontext)->bool{
        let Some(ctx)=(unsafe{ctx.as_mut()})else{return false;};if ctx.in_host!=0||context::region(m.pc)!=context::Region::Other{return false;}
        let ec=m.esr>>26;let raw=matches!(ec,0x24|0x25)&&m.esr&(1<<6)!=0;
        let execute=matches!(ec,0x20|0x21);
        if !(raw&&memory::intent(m.far,W)||execute&&memory::intent(m.pc,X)){return false;}
        let state=State::from_mc(m,raw);
        if !FAULT.with(|slot|{if slot.busy.swap(true,Ordering::SeqCst){return false;}unsafe{*slot.value.get()=Some(Fault{state,raw});}true}){return false;}
        unsafe{state.to_context(ctx,raw);}ctx.in_host=1;m.pc=finish as*const()as u64;m.sp=ctx.host_sp;m.fp=0;m.lr=0;true
    }
    struct Resume;
    pub(super) fn resume_dispatch(){
        if DISPATCH.with(Cell::get)!=0&&DISPATCH.with(Cell::get)==DEPTH.with(Cell::get){std::panic::resume_unwind(Box::new(Resume));}
    }
    struct Frame(usize);impl Frame{fn enter()->Self{Self(DEPTH.with(|depth|{let old=depth.get();depth.set(old+1);old}))}}
    impl Drop for Frame{fn drop(&mut self){DEPTH.with(|depth|depth.set(self.0));}}
    struct Dispatch(usize);impl Dispatch{fn enter()->Self{Self(DISPATCH.with(|depth|{let old=depth.get();depth.set(DEPTH.with(Cell::get));old}))}}
    impl Drop for Dispatch{fn drop(&mut self){DISPATCH.with(|depth|depth.set(self.0));}}
    unsafe fn native(ctx:*mut GuestContext,state:&State,raw:bool,frame:Frame)->!{
        clear_exclusive();unsafe{state.to_context(ctx,raw);}drop(frame);context::resume_native_trap()
    }
    extern "C" fn finish()->!{
        let Fault{mut state,mut raw}=FAULT.with(|slot|{let value=unsafe{(*slot.value.get()).take().expect("retained mutable code fault")};slot.busy.store(false,Ordering::SeqCst);value});
        let ctx=context::current_ctx();let frame=Frame::enter();clear_exclusive();
        loop{
            if raw&&memory::intent(state.pc,X){raw=false;state.x[18]=context::guest_scs();state.tp=context::guest_tp();state.readonly_tp=0;}
            if !raw&&!memory::intent(state.pc,X){unsafe{native(ctx,&state,false,frame)}}
            if raw&&!sys::arena::contains(state.pc,4)&&!crate::patch::is_veneer_pc(state.pc){unsafe{native(ctx,&state,true,frame)}}
            let (result,instruction,wrote,pending)={
                let _epoch=verity_pager::mutation();let _transaction=memory::transaction();
                with_cpu(|cpu|{
                    cpu.memory.wrote=false;cpu.memory.pending=None;cpu.memory.leases.clear();cpu.memory.regions.clear();
                    // The caller holds both VM and source transaction leases.
                    // Keep ordinary original instructions in one bounded interval;
                    // the precise owner stops after a possible executable-source store,
                    // and at SVC, exceptions, range exit or the instruction budget.
                    let result=unsafe{if !raw&&let Some(lease)=memory::mapping(state.pc,1,X){
                        kl_a64_precise_run_owned(cpu.handle,&mut state,STEP.fetch_add(1,Ordering::Relaxed),lease.start,lease.start+lease.length,4096)
                    }else{kl_a64_precise_run(cpu.handle,&mut state,STEP.fetch_add(1,Ordering::Relaxed))}};
                    let instruction=unsafe{kl_a64_precise_instruction(cpu.handle)};let wrote=cpu.memory.wrote;let pending=cpu.memory.pending.take();cpu.memory.regions.clear();cpu.memory.leases.clear();(result,instruction,wrote,pending)
                })
            };
            if let Some((address,required))=pending{
                if verity_pager::prepare_user_range(address,1,access(required)).is_ok(){continue;}
            }
            match result.kind {
                0=>{if raw&&wrote{unsafe{native(ctx,&state,true,frame)}}},
                1=>{
                    if result.detail!=0{unsafe{state.to_context(ctx,raw);}clear_exclusive();sys::shadow_fault(ctx,4,1,result.address);state=unsafe{State::from_context(ctx)};raw=false;continue;}
                    unsafe{state.to_context(ctx,raw);(*ctx).orig_x0=state.x[0];}clear_exclusive();
                    if state.x[8]==aim_hostcall::SYSCALL_NR{
                        // Preserve the real host-call callback owner without
                        // holding a Rust borrow of GuestContext across JNI.
                        let previous=unsafe{(*ctx).callback_sp};unsafe{(*ctx).callback_sp=state.sp;}
                        let value=crate::hostcall::call(state.x[0],state.x[1],state.x[2],state.x[3]);
                        unsafe{(*ctx).callback_sp=previous;}state.x[0]=value as u64;if !raw{state.tp=context::guest_tp();state.x[18]=context::guest_scs();}
                    }else{
                        let dispatched=std::panic::catch_unwind(std::panic::AssertUnwindSafe(||{let _dispatch=Dispatch::enter();unsafe{sys::dispatch(&mut*ctx);}}));
                        if let Err(value)=dispatched{if !value.is::<Resume>(){std::panic::resume_unwind(value);}}
                        state=unsafe{State::from_context(ctx)};raw=false;
                    }
                },
                3 if raw=>{
                    if instruction&0xffe0001f==0xd4200000&&let Some(target)=crate::patch::far_continuation(result.address,((instruction>>5)&0xffff)as u16){state.pc=target;continue;}
                    state.pc=result.address;unsafe{native(ctx,&state,true,frame)}
                },
                2|3|4=>{
                    unsafe{state.to_context(ctx,raw);}clear_exclusive();let signal=if result.kind==2{if result.error==-errno::EIO{7}else{11}}else if result.kind==3&&instruction&0xffe0001f==0xd4200000{5}else{4};
                    sys::shadow_fault(ctx,signal,if signal==7{2}else{1},result.address);state=unsafe{State::from_context(ctx)};raw=false;
                },
                _=>panic!("Unexpected original A64 precise stop"),
            }
            if !raw&&unsafe{!(*ctx).attn.is_null()&&(*(*ctx).attn).load(Ordering::Acquire)!=0}{
                clear_exclusive();unsafe{state.to_context(ctx,false);}sys::shadow_deliver(ctx);state=unsafe{State::from_context(ctx)};
            }
        }
    }
    #[cfg(test)]
    mod tests{
        use super::*;
        const PAGE:usize=16384;
        unsafe extern "C" fn conservative(opaque:*mut c_void,address:u64,length:usize,required:u32,result:*mut Mapping)->i32{
            let value=unsafe{map(opaque,address,length,required,result)};
            if value==0{unsafe{(*result).flags&=!4;}}value
        }
        #[test]
        fn mixed_owned_source_and_aliased_io_keep_store_barriers_and_fresh_authority(){
            let _epoch=verity_pager::mutation();let _transaction=memory::transaction();memory::reset();
            let allocate=||{let p=unsafe{libc::mmap(std::ptr::null_mut(),PAGE,R|W,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};assert_ne!(p,libc::MAP_FAILED);p as u64};
            let code=allocate();let data=allocate();unsafe{std::ptr::write_volatile(data as*mut u64,0);}
            let alias=crate::patch::vm::alias(data,PAGE as u64,None,R|W).unwrap();
            let program=|words:&[u32]|{let mut bytes=vec![0;PAGE];for(at,word)in words.iter().enumerate(){bytes[at*4..at*4+4].copy_from_slice(&word.to_le_bytes());}bytes};
            let loop_bytes=program(&[0xf9000041,0x91000421,0xf1000400,0x54ffffa1,0xd4200000]);
            memory::prepare(code,&loop_bytes,R|X).unwrap().install(|publish|publish()).unwrap();
            let initial=||State{x:[0;31],sp:0,pc:code,v:[[0;2];32],tp:0,readonly_tp:0,nzcv:0,fpcr:0,fpsr:0};
            let run=|cpu:&mut Cpu,state:&mut State|{cpu.memory.wrote=false;cpu.memory.pending=None;cpu.memory.leases.clear();cpu.memory.regions.clear();let result=unsafe{kl_a64_precise_run_owned(cpu.handle,state,STEP.fetch_add(1,Ordering::Relaxed),code,code+PAGE as u64,1024)};cpu.memory.regions.clear();cpu.memory.leases.clear();result};
            with_cpu(|cpu|{
                assert_eq!(unsafe{kl_a64_precise_clear_exclusive(cpu.handle)},0);assert!(!cpu.memory.private_data(data,4096),"a real alias must not claim the private-data exemption");
                let mut mapping=Mapping{address:0,length:0,host:std::ptr::null_mut(),permissions:0,flags:0};assert_eq!(unsafe{map((&mut*cpu.memory)as*mut _ as*mut _,data,8,W as u32,&mut mapping)},0);assert_eq!(mapping.flags,2,"the real mixed mapping is ordinary IO, not private or execution-owned");
                let before=unsafe{kl_a64_precise_counters(cpu.handle)};let mut state=initial();state.x[0]=32;state.x[2]=data;let mut calls=0;
                while state.pc<code+16{let result=run(cpu,&mut state);calls+=1;assert!(result.kind==0||(result.kind==3&&result.address==code+16));assert!(calls<=33);}
                assert_eq!((calls,state.x[0],state.x[1],state.pc),(33,0,32,code+16),"every unknown-alias source-store barrier remains");assert_eq!(unsafe{*(alias as*const u64)},31);
                let after=unsafe{kl_a64_precise_counters(cpu.handle)};assert_eq!((after.mapped-before.mapped,after.retired-before.retired),(2,2),"owned RX and aliased non-executable IO each map once, then retire on BRK");
                // A cached IO view can itself alias executable source. The
                // original store barrier must stop before the modified word,
                // and full source revalidation must decode its new instruction.
                let modifying=program(&[0xb9000041,0x91000400,0x14000000]);memory::prepare(code,&modifying,R|X).unwrap().install(|publish|publish()).unwrap();
                let source_alias=crate::patch::vm::alias(code,PAGE as u64,None,R|W).unwrap();assert!(!cpu.memory.private_data(source_alias,4096));
                let mut state=initial();state.x[1]=0x91000800;state.x[2]=source_alias+4;let result=run(cpu,&mut state);assert_eq!((result.kind,state.pc,state.x[0]),(0,code+4,0));assert_eq!(unsafe{*((code+4)as*const u32)},0x91000800);
                let changed=unsafe{kl_a64_precise_counters(cpu.handle)};assert_eq!(run(cpu,&mut state).kind,0);assert_eq!(state.x[0],2);assert_eq!(unsafe{kl_a64_precise_counters(cpu.handle)}.source_changed-changed.source_changed,1);
                assert_eq!(unsafe{kl_a64_precise_clear_exclusive(cpu.handle)},0);unsafe{libc::munmap(source_alias as*mut _,PAGE);}
                memory::prepare(code,&loop_bytes,R|X).unwrap().install(|publish|publish()).unwrap();let mut state=initial();state.x[0]=32;state.x[2]=data;assert_eq!(run(cpu,&mut state).kind,0);
                assert_eq!(unsafe{libc::mprotect(data as*mut _,PAGE,R)},0);let denied=run(cpu,&mut state);assert_eq!((denied.kind,denied.error),(2,-errno::EFAULT));assert_eq!(unsafe{*(alias as*const u64)},0,"cached write authority cannot survive mprotect");
                assert_eq!(unsafe{libc::mprotect(data as*mut _,PAGE,R|W)},0);let mut state=initial();state.x[0]=32;state.x[2]=data;assert_eq!(run(cpu,&mut state).kind,0);unsafe{libc::munmap(data as*mut _,PAGE);}
                let retired=run(cpu,&mut state);assert_eq!((retired.kind,retired.error),(2,-errno::EFAULT));assert_eq!(unsafe{*(alias as*const u64)},0,"retired virtual data cannot reuse its former object through a cached pointer");
                assert_eq!(unsafe{kl_a64_precise_clear_exclusive(cpu.handle)},0);eprintln!("mixed actual-Mach owned_RX=1 aliased_IO=1 mapped=2 retired=2 intervals=33 source_store_barriers_preserved=true source_alias_change=true permissions_and_retirement=true");
            });memory::forget(code,PAGE as u64);unsafe{libc::munmap(code as*mut _,PAGE);libc::munmap(alias as*mut _,PAGE);}
        }
        #[test]
        fn owned_rx_cache_reacquires_source_and_preserves_mutation_and_retirement(){
            let _epoch=verity_pager::mutation();let _transaction=memory::transaction();memory::reset();
            let raw=unsafe{libc::mmap(std::ptr::null_mut(),PAGE,R|W,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};assert_ne!(raw,libc::MAP_FAILED);let code=raw as u64;
            let program=|increment:u32|{let mut bytes=vec![0u8;PAGE];bytes[..4].copy_from_slice(&(0x91000000u32|(increment<<10)).to_le_bytes());bytes[4..8].copy_from_slice(&0x17ffffffu32.to_le_bytes());bytes};
            memory::prepare(code,&program(1),R|X).unwrap().install(|publish|publish()).unwrap();
            let initial=||State{x:[0;31],sp:0,pc:code,v:[[0;2];32],tp:0,readonly_tp:0,nzcv:0,fpcr:0,fpsr:0};
            let run=|cpu:&mut Cpu,state:&mut State|{
                cpu.memory.wrote=false;cpu.memory.pending=None;cpu.memory.leases.clear();cpu.memory.regions.clear();
                let result=unsafe{kl_a64_precise_run_owned(cpu.handle,state,STEP.fetch_add(1,Ordering::Relaxed),code,code+PAGE as u64,1024)};
                cpu.memory.regions.clear();cpu.memory.leases.clear();result
            };
            // The prior ART wiring had no cache registration, so every bounded
            // interval discarded and rebuilt the same real Unicorn mapping.
            let mut baseline_owner=Box::<Memory>::default();let config=Config{owner:(&mut*baseline_owner)as*mut _ as*mut _,map,counter,ctr:crate::a64::host_ctr_el0(),frequency:1_000_000_000,dczid:4,write};
            let baseline=unsafe{kl_a64_precise_create(config)};assert!(!baseline.is_null());let mut baseline=Cpu{handle:baseline,memory:baseline_owner};let mut expected=initial();
            let before=unsafe{kl_a64_precise_counters(baseline.handle)};
            for _ in 0..8{assert_eq!(run(&mut baseline,&mut expected).kind,0);}
            let after=unsafe{kl_a64_precise_counters(baseline.handle)};
            assert_eq!((after.mapped-before.mapped,after.retired-before.retired),(8,8));drop(baseline);
            with_cpu(|cpu|{
                assert_eq!(unsafe{kl_a64_precise_clear_exclusive(cpu.handle)},0);let before=unsafe{kl_a64_precise_counters(cpu.handle)};let mut state=initial();
                for _ in 0..8{assert_eq!(run(cpu,&mut state).kind,0);}
                assert_eq!((state.x,state.pc,state.nzcv),(expected.x,expected.pc,expected.nzcv));assert_eq!(state.x[0],4096);
                let warm=unsafe{kl_a64_precise_counters(cpu.handle)};
                assert_eq!((warm.mapped-before.mapped,warm.retired-before.retired,warm.revalidated-before.revalidated),(1,0,7));
                let mut view=Mapping{address:0,length:0,host:std::ptr::null_mut(),permissions:0,flags:0};
                assert_eq!(unsafe{revalidate((&mut*cpu.memory)as*mut _ as*mut _,code,8192,&mut view)},-errno::EFAULT,"the entire retained view must be reacquired");cpu.memory.leases.clear();
                memory::protect(code,PAGE as u64,R|W|X).unwrap().unwrap();memory::write(code,&program(2)[..4]).unwrap();memory::protect(code,PAGE as u64,R|X).unwrap().unwrap();
                assert_eq!(run(cpu,&mut state).kind,0);assert_eq!(state.x[0],5120);let changed=unsafe{kl_a64_precise_counters(cpu.handle)};assert_eq!(changed.source_changed-warm.source_changed,1);
                memory::prepare(code,&program(3),R|X).unwrap().install(|publish|publish()).unwrap();
                assert_eq!(run(cpu,&mut state).kind,0);assert_eq!(state.x[0],6656);let replaced=unsafe{kl_a64_precise_counters(cpu.handle)};
                assert_eq!((replaced.mapped-changed.mapped,replaced.retired-changed.retired),(1,1),"replacement backing retires the stale pointer without reading it");
                let alias=crate::patch::vm::alias(code,PAGE as u64,None,R).unwrap();memory::relocated(code,alias,PAGE as u64).unwrap();memory::protect(alias,PAGE as u64,R|W|X).unwrap().unwrap();memory::write(alias,&program(4)[..4]).unwrap();
                assert_eq!(run(cpu,&mut state).kind,0);assert_eq!(state.x[0],8704);let aliased=unsafe{kl_a64_precise_counters(cpu.handle)};assert_eq!(aliased.source_changed-replaced.source_changed,1,"a real shared alias changes the original instruction bytes");
                memory::protect(code,PAGE as u64,R).unwrap().unwrap();let denied=run(cpu,&mut state);assert_eq!((denied.kind,denied.error,state.x[0]),(2,-errno::EFAULT,8704),"removed execute permission cannot reuse a cached instruction");
                memory::protect(code,PAGE as u64,R|X).unwrap().unwrap();assert_eq!(run(cpu,&mut state).kind,0);let value=state.x[0];memory::forget(code,PAGE as u64);unsafe{libc::munmap(raw,PAGE);}
                let retired=run(cpu,&mut state);assert_eq!((retired.kind,retired.error,state.x[0]),(2,-errno::EFAULT,value),"retired source cannot use an old native pointer");
                assert_eq!(unsafe{kl_a64_precise_clear_exclusive(cpu.handle)},0);let end=unsafe{kl_a64_precise_counters(cpu.handle)};assert_eq!(end.mapped-before.mapped,end.retired-before.retired);
                eprintln!("owned-RX actual-Mach intervals=8 baseline_mapped=8 cached_mapped=1 revalidated=7 source_changes={} all_mappings_retired=true",end.source_changed-before.source_changed);
                memory::forget(alias,PAGE as u64);unsafe{libc::munmap(alias as*mut _,PAGE);}
            });
        }
        #[test]
        fn private_data_stores_keep_exact_bytes_and_reduce_real_mapping_churn(){
            let _epoch=verity_pager::mutation();let _transaction=memory::transaction();memory::reset();
            let allocate=||{let p=unsafe{libc::mmap(std::ptr::null_mut(),PAGE,R|W,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};assert_ne!(p,libc::MAP_FAILED);p as u64};
            let code=allocate();let data=allocate();
            let mut admission=Memory::default();
            assert!(admission.private_data(data,4096),"untouched private zero-fill is data");
            unsafe{std::ptr::write_volatile(data as*mut u8,0x39);}admission.regions.clear();
            let resident=super::super::super::vmmap::region_at(data).unwrap();
            eprintln!("actual-Mach resident prot={} shared={} file={} usage={:?}",resident.prot,resident.shared,resident.file.is_some(),resident.usage);
            assert!(admission.private_data(data,4096),"resident private anonymous object is data");
            // The source owner covers the complete mapped page, including a
            // branch beyond the caller's initial range. Stop at an original
            // architectural exception rather than an artificial byte bound.
            let program=[0xf9000041u32,0x91000421,0xf1000400,0x54ffffa1,0xd4200000];
            let mut source=vec![0u8;PAGE];for(at,word)in program.iter().enumerate(){source[at*4..at*4+4].copy_from_slice(&word.to_le_bytes());}
            memory::prepare(code,&source,R|W|X).unwrap().install(|publish|publish()).unwrap();
            admission.regions.clear();assert!(admission.private_data(data,4096),"source enrollment does not broaden data authority");
            let mut results=Vec::new();
            for admitted in [false,true]{
                unsafe{std::ptr::write_bytes(data as*mut u8,0,PAGE);}
                let mut owner=Box::<Memory>::default();let config=Config{owner:(&mut*owner)as*mut _ as*mut _,map:if admitted{map}else{conservative},counter,ctr:crate::a64::host_ctr_el0(),frequency:1_000_000_000,dczid:4,write};
                let cpu=unsafe{kl_a64_precise_create(config)};assert!(!cpu.is_null());
                let mut state=State{x:[0;31],sp:data+PAGE as u64,pc:code,v:[[0;2];32],tp:0,readonly_tp:0,nzcv:0,fpcr:0,fpsr:0};state.x[0]=1024;state.x[2]=data;
                let before=unsafe{kl_a64_precise_counters(cpu)};let started=std::time::Instant::now();let mut calls=0;
                while state.pc<code+16{
                    owner.wrote=false;owner.pending=None;owner.leases.clear();owner.regions.clear();
                    let result=unsafe{kl_a64_precise_run_owned(cpu,&mut state,STEP.fetch_add(1,Ordering::Relaxed),code,code+16,1024)};
                    owner.regions.clear();owner.leases.clear();calls+=1;
                    // The final budget may stop immediately before BRK; an
                    // earlier source-store barrier may reach the BRK itself.
                    assert!(result.kind==0||(result.kind==3&&state.pc==code+16&&result.address==code+16&&unsafe{kl_a64_precise_instruction(cpu)}==0xd4200000),"kind={} errno={} address={:#x}",result.kind,result.error,result.address);assert!(calls<=1025);
                }
                let after=unsafe{kl_a64_precise_counters(cpu)};let bytes=unsafe{std::slice::from_raw_parts(data as*const u8,PAGE)}.to_vec();
                assert_eq!((state.x[0],state.x[1],state.pc),(0,1024,code+16));assert_eq!(u64::from_le_bytes(bytes[..8].try_into().unwrap()),1023);
                assert_eq!(unsafe{std::slice::from_raw_parts(code as*const u8,PAGE)},source);
                eprintln!("private-data actual-Mach admitted={admitted} calls={calls} mapped={} retired={} elapsed_us={}",after.mapped-before.mapped,after.retired-before.retired,started.elapsed().as_micros());
                results.push((state.x,state.pc,bytes,calls,after.mapped-before.mapped,after.retired-before.retired));unsafe{kl_a64_precise_destroy(cpu);}
            }
            assert_eq!((&results[0].0,results[0].1,&results[0].2),(&results[1].0,results[1].1,&results[1].2));
            assert_eq!(results[0].3,1025);assert_eq!(results[1].3,4);assert!(results[1].4*100<results[0].4);assert_eq!(results[1].4,results[1].5,"no pointer mapping survives its VM lease");
            let alias=crate::patch::vm::alias(data,PAGE as u64,None,R|W).unwrap();admission.regions.clear();assert!(!admission.private_data(data,4096),"real alias cannot omit executable-source barriers");
            unsafe{libc::munmap(alias as*mut _,PAGE);}admission.regions.clear();assert!(admission.private_data(data,4096),"last alias removal restores unique ownership");assert!(!admission.private_data(code,4096));
            assert!(!admission.private_data(data,0));assert!(!admission.private_data(u64::MAX-4,8));
            memory::forget(code,PAGE as u64);unsafe{libc::munmap(code as*mut _,PAGE);libc::munmap(data as*mut _,PAGE);}
        }
    }

}
