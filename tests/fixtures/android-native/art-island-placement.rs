extern crate aim_linux_abi;
use aim_linux_abi::{a64,patch};

unsafe extern "C" {
    fn mmap(at:*mut u8,length:usize,prot:i32,flags:i32,fd:i32,offset:i64)->*mut u8;
    fn mprotect(at:*mut u8,length:usize,prot:i32)->i32;
    fn munmap(at:*mut u8,length:usize)->i32;
}

#[repr(C)] struct Stack {pointer:*mut u8,size:usize,flags:i32}
#[repr(C)] struct Context {onstack:i32,mask:u32,stack:Stack,link:*mut u8,size:usize,machine:*mut Machine}
#[repr(C)] struct Registers {x:[u64;29],fp:u64,lr:u64,sp:u64,pc:u64,cpsr:u32,pad:u32}
#[repr(C)] struct Neon {v:[u128;32],fpsr:u32,fpcr:u32}
#[repr(C)] struct Machine {exception:[u64;2],registers:Registers,neon:Neon}
fn interrupted_target(site:u64,word:u32)->u64{unsafe{
    let mut machine:Machine=std::mem::zeroed();machine.exception[1]=0xf2000000|((word as u64>>5)&0xffff);machine.registers.pc=site;
    let mut context:Context=std::mem::zeroed();context.machine=&mut machine;
    assert!(patch::handle_brk((&mut context as*mut Context).cast()));machine.registers.pc
}}

fn reservation(base:usize,length:usize,offset:usize)->(*mut u8,*mut u8){unsafe{
    let base=base as*mut u8;
    assert_eq!(mmap(base,length,0,0x1002,-1,0),base,"exact synthetic VM reservation");
    let site=base.add(offset);assert_eq!(mprotect(site,16384,3),0);
    (site as*mut u32).write(0xaa0003f2); // MOV X18,X0; never executed here.
    (base,site)
}}

fn main(){unsafe{
    // A reachable gap exists below this large image, while every original
    // placement hint falls inside its occupied PROT_NONE reservation.
    let length=320usize<<20;
    let (base,site)=reservation(0x8400000000,length,96<<20);
    let result=patch::rewrite_sites(&[(site as u64,a64::Kind::X18,0xaa0003f2)],site as u64,site as u64+16384,true);
    if std::env::args().nth(1).as_deref()==Some("previous") {
        assert_eq!(result.unwrap_err().raw_os_error(),Some(12));
        assert_eq!((site as*const u32).read(),0xaa0003f2);
        println!("PASS previous island placement ENOMEM reproduced with reachable free gap");
    } else {
        assert_eq!(result.unwrap().x18,1);
        let branch=(site as*const u32).read();assert_eq!(branch&0xfc000000,0x14000000);
        let displacement=((branch as i64&0x03ffffff)<<38)>>36;
        let target=(site as i64+displacement)as u64;
        assert!(target<base as u64||target>=base as u64+length as u64);
        assert!((target as i64-site as i64).abs()<128<<20);
        let (lo,hi,prot,_)=patch::vm::region(base as u64).unwrap();
        assert_eq!(lo,base as u64);assert_eq!(hi,site as u64);assert_eq!(prot,0);
        println!("PASS original island placement finds reachable free gap target={target:#x}");
    }
    assert_eq!(munmap(base,length),0);
    if std::env::args().nth(1).as_deref()!=Some("previous") {
        // Match the real Bionic alignment trim: a reachable 48 KiB gap in an
        // otherwise occupied neighborhood, too small for a default island.
        let length=512usize<<20;
        let (base,site)=reservation(0x8800000000,length,256<<20);
        let gap=site.sub(96<<20);let capacity=3*16384;
        assert_eq!(munmap(gap,capacity),0);
        let result=patch::rewrite_sites(&[(site as u64,a64::Kind::X18,0xaa0003f2)],site as u64,site as u64+16384,true);
        assert_eq!(result.unwrap().x18,1);
        let branch=(site as*const u32).read();
        let target=(site as i64+(((branch as i64&0x03ffffff)<<38)>>36))as u64;
        assert!(target>=gap as u64&&target<gap as u64+capacity as u64);
        assert_eq!(patch::vm::region(gap as u64-1).unwrap().2,0);
        assert_eq!(patch::vm::region(gap as u64+capacity as u64).unwrap().2,0);
        // Exhaust the small mapping. Its advertised capacity must not let a
        // later branch escape into the neighboring PROT_NONE reservation.
        let maximum=capacity/(a64::stub_len(a64::Kind::X18)*4)+1;
        let mut exhausted=false;
        for _ in 0..maximum {
            (site as*mut u32).write(0xaa0003f2);
            match patch::rewrite_sites(&[(site as u64,a64::Kind::X18,0xaa0003f2)],site as u64,site as u64+16384,true) {
                Ok(_)=>{
                    let branch=(site as*const u32).read();
                    if branch&0xffe0001f==0xd4200000 {
                        assert_eq!(patch::vm::region(gap as u64+capacity as u64).unwrap().2,0);
                        exhausted=true;break;
                    }
                    let target=(site as i64+(((branch as i64&0x03ffffff)<<38)>>36))as u64;
                    assert!(target>=gap as u64&&target<gap as u64+capacity as u64);
                }
                Err(error)=>{
                    assert_eq!(error.raw_os_error(),Some(12));
                    assert_eq!((site as*const u32).read(),0xaa0003f2);
                    exhausted=true;break;
                }
            }
        }
        assert!(exhausted,"small island capacity is bounded");
        assert_eq!(munmap(base,length),0);
        println!("PASS reachable 48 KiB gap fits bounded original instruction island");
    }
    // A fully occupied branch range uses a hardware far body. Old fetched
    // identities retain their exact body after replacement at the same PC.
    let length=512usize<<20;
    let (base,site)=reservation(0x8600000000,length,256<<20);
    let result=patch::rewrite_sites(&[(site as u64,a64::Kind::X18,0xaa0003f2)],site as u64,site as u64+16384,true);
    assert_eq!(result.unwrap().x18,1);
    let first=(site as*const u32).read();assert_eq!(first&0xffe0001f,0xd4200000);
    let original_target=interrupted_target(site as u64,first);
    (site as*mut u32).write(0xd1038243);
    assert_eq!(patch::rewrite_sites(&[(site as u64,a64::Kind::X18,0xd1038243)],site as u64,site as u64+16384,true).unwrap().x18,1);
    let next=(site as*const u32).read();assert_ne!(first,next);
    assert_eq!(interrupted_target(site as u64,first),original_target);
    assert_ne!(interrupted_target(site as u64,next),original_target);
    assert_eq!(patch::check_original_instructions(site as u64,&first.to_le_bytes()).unwrap_err().raw_os_error(),Some(8));
    assert_eq!((site as*const u32).read(),next);
    assert_eq!(patch::vm::region(base as u64).unwrap().2,0);
    assert_eq!(munmap(base,length),0);
    println!("PASS far generations preserve old fetched semantics and reject original BRK collisions");
}}
