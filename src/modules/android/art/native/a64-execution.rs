//! CPU binding around the original process, mapping and Linux context owners.
use std::sync::OnceLock;
pub fn enabled()->bool {
    static ENABLED:OnceLock<bool>=OnceLock::new();
    *ENABLED.get_or_init(|| {
        let requested=std::env::var_os("MACLAUNCHER_A64_ORIGINAL_CODE").is_some_and(|v|v=="1");
        if requested && !cfg!(maclauncher_a64){panic!("The selected native runner has no A64 CPU component");}
        requested
    })
}

#[cfg(maclauncher_a64)]
mod engine {
    use crate::{context::{self,GuestContext},sys};
    use std::{cell::{Cell,RefCell},ffi::c_void,sync::atomic::{AtomicU64,Ordering}};
    const RETURN:u64=0xffff_ffff_ffff_fff0;
    static GENERATION:AtomicU64=AtomicU64::new(1);
    #[repr(C)]
    struct State{x:[u64;31],sp:u64,pc:u64,v:[[u64;2];32],tp:u64,readonly_tp:u64,nzcv:u32,fpcr:u32,fpsr:u32}
    #[repr(C)]
    struct Memory{owner:*mut c_void,read:unsafe extern "C" fn(*mut c_void,u64,*mut c_void,usize,i32)->i32,write:unsafe extern "C" fn(*mut c_void,u64,*const c_void,usize)->i32,exclusive:unsafe extern "C" fn(*mut c_void,u64,*const c_void,*const c_void,usize)->i32,counter:unsafe extern "C" fn(*mut c_void)->u64}
    #[repr(C)]
    struct Config{memory:Memory,ctr:u32,frequency:u32,dczid:u32,direct:i32}
    #[repr(C)]
    struct Exit{kind:u32,detail:u32,address:u64,error:i32}
    unsafe extern "C" {
        fn kl_a64_create(config:Config)->*mut c_void;
        fn kl_a64_destroy(cpu:*mut c_void);
        fn kl_a64_run(cpu:*mut c_void,state:*mut State,budget:u64)->Exit;
        fn kl_a64_clear_cache(cpu:*mut c_void);
        fn kl_a64_compare_exchange(address:u64,value:*const c_void,expected:*const c_void,size:usize)->i32;
    }
    struct Cpu{handle:*mut c_void,generation:Cell<u64>}
    impl Drop for Cpu{fn drop(&mut self){unsafe{kl_a64_destroy(self.handle);}}}
    thread_local!{
        static CPU:RefCell<Option<Cpu>>=const{RefCell::new(None)};
        static DISPATCH:Cell<usize>=const{Cell::new(0)};
    }
    unsafe extern "C" fn read(_: *mut c_void,address:u64,output:*mut c_void,length:usize,execute:i32)->i32 {
        if execute!=0 {
            if address==RETURN{return -crate::errno::EFAULT;}
            if let Err(error)=sys::user_memory::prepare_execute(address,length){return -error;}
        }
        match sys::user_memory::read_exact(address,length){
            Ok(bytes)=>{unsafe{std::ptr::copy_nonoverlapping(bytes.as_ptr(),output.cast(),length);}0},
            Err(error)=>-error,
        }
    }
    unsafe extern "C" fn write(_: *mut c_void,address:u64,input:*const c_void,length:usize)->i32 {
        let bytes=unsafe{std::slice::from_raw_parts(input.cast::<u8>(),length)};
        match sys::user_memory::write_exact(address,bytes){Ok(())=>0,Err(error)=>-error}
    }
    unsafe extern "C" fn exclusive(_: *mut c_void,address:u64,value:*const c_void,expected:*const c_void,length:usize)->i32 {
        if let Err(error)=sys::user_memory::prepare_write(address,length){return -error;}
        if address%length as u64!=0{return -crate::errno::EFAULT;}
        unsafe{kl_a64_compare_exchange(address,value,expected,length)}
    }
    unsafe extern "C" fn counter(_: *mut c_void)->u64{aim_hostcall::clock::monotonic_ns() as u64}
    fn cpu()->*mut c_void {
        CPU.with(|slot|{
            let mut slot=slot.borrow_mut();
            let cpu=slot.get_or_insert_with(||{
                let config=Config{memory:Memory{owner:std::ptr::null_mut(),read,write,exclusive,counter},ctr:crate::a64::host_ctr_el0(),frequency:1_000_000_000,dczid:4,direct:1};
                let handle=unsafe{kl_a64_create(config)};
                assert!(!handle.is_null(),"A64 CPU allocation");
                Cpu{handle,generation:Cell::new(0)}
            });
            let generation=GENERATION.load(Ordering::Acquire);
            if cpu.generation.replace(generation)!=generation{unsafe{kl_a64_clear_cache(cpu.handle);}}
            cpu.handle
        })
    }
    impl State {
        unsafe fn from_context(ctx:*mut GuestContext)->Self {
            let ctx=unsafe{&*ctx};
            Self{x:ctx.x,sp:ctx.sp,pc:ctx.resume_pc(),v:ctx.v.map(|value|[value as u64,(value>>64)as u64]),tp:context::guest_tp(),readonly_tp:0,nzcv:ctx.nzcv as u32,fpcr:ctx.fpcr as u32,fpsr:ctx.fpsr as u32}
        }
        unsafe fn to_context(&self,ctx:*mut GuestContext){
            let ctx=unsafe{&mut *ctx};ctx.x=self.x;ctx.sp=self.sp;ctx.pc=self.pc;ctx.stub_ret=0;ctx.v=self.v.map(|v|(v[0]as u128)|((v[1]as u128)<<64));ctx.nzcv=self.nzcv as u64;ctx.fpcr=self.fpcr as u64;ctx.fpsr=self.fpsr as u64;
            context::set_guest_tp(self.tp);context::set_guest_scs(self.x[18]);
        }
    }
    struct DispatchScope;
    impl DispatchScope{fn enter()->Self{DISPATCH.with(|v|v.set(v.get()+1));Self}}
    impl Drop for DispatchScope{fn drop(&mut self){DISPATCH.with(|v|v.set(v.get()-1));}}
    struct Resume;
    pub fn memory_changed(){GENERATION.fetch_add(1,Ordering::AcqRel);}
    unsafe fn execute(ctx:*mut GuestContext,callback:bool)->u64 {
        unsafe{(*ctx).in_host=1;(*ctx).stub_ret=0;}
        loop {
            sys::shadow_deliver(ctx);
            let mut state=unsafe{State::from_context(ctx)};
            let result=unsafe{kl_a64_run(cpu(),&mut state,16_384)};
            unsafe{state.to_context(ctx);}
            if callback && (state.pc==RETURN||result.address==RETURN){return state.x[0];}
            match result.kind {
                0=>{},
                1=>{
                    if result.detail!=0{sys::shadow_fault(ctx,4,1,state.pc);continue;}
                    let number=state.x[8];unsafe{(*ctx).orig_x0=state.x[0];}
                    let dispatched=std::panic::catch_unwind(std::panic::AssertUnwindSafe(||{
                        let _scope=DispatchScope::enter();unsafe{sys::dispatch(&mut *ctx);}
                    }));
                    if let Err(value)=dispatched{if !value.is::<Resume>(){std::panic::resume_unwind(value);}}
                    if matches!(number,214|215|216|222|226)&&unsafe{(*ctx).x[0]as i64}>=0{memory_changed();}
                },
                2=>{let signal=if result.error== -crate::errno::EIO{7}else{11};sys::shadow_fault(ctx,signal,if signal==7{2}else{1},result.address);},
                3=>sys::shadow_fault(ctx,if result.detail==9{11}else{4},1,result.address),
                4=>{
                    crate::diag!("[a64-execution] unsupported instruction pc={:#x} bytes={:?}",result.address,sys::user_memory::read_exact(result.address,4));
                    sys::shadow_fault(ctx,4,1,result.address);
                },
                6=>memory_changed(),
                _=>panic!("Unexpected A64 CPU stop"),
            }
        }
    }
    pub unsafe fn enter(ctx:*mut GuestContext)->! {
        if DISPATCH.with(|v|v.get()!=0){std::panic::resume_unwind(Box::new(Resume));}
        unsafe{execute(ctx,false);}
        unreachable!()
    }
    pub unsafe fn call_guest(function:u64,argument:u64)->u64 {
        let ctx=context::current_ctx();assert!(!ctx.is_null());
        let mut saved=unsafe{State::from_context(ctx)};
        unsafe{(*ctx).pc=function;(*ctx).stub_ret=0;(*ctx).x[0]=argument;(*ctx).x[30]=RETURN;}
        let result=unsafe{execute(ctx,true)};
        saved.tp=context::guest_tp();unsafe{saved.to_context(ctx);}
        result
    }
}
#[cfg(maclauncher_a64)]
pub use engine::{enter,call_guest,memory_changed};
