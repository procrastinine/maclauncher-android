use super::{cred,procrec,vmmap};
use crate::loader::{self,Image,StackInputs,PAGE};
use std::{ffi::CStr,os::unix::fs::DirBuilderExt};

#[test]
fn initial_table_is_private_and_argument_record_remains_live(){
    let directory=std::env::temp_dir().join(format!("android-initial-stack-{}",std::process::id()));
    std::fs::DirBuilder::new().mode(0o700).create(&directory).unwrap();
    cred::init(cred::Identity::default(),Some(directory.clone()));
    let program=Image{bias:0,entry:0x1234,phdr:0x5678,phnum:7,end:0,interp:None,source:String::new(),stats:Default::default()};
    for (argv,envp) in [
        (vec![b"stack-probe".to_vec(),b"argument".to_vec()],vec![b"STACK_TEST=value".to_vec()]),
        (vec![vec![b'a';PAGE as usize+17],b"tail".to_vec()],vec![vec![b'e';PAGE as usize+33]]),
        (Vec::new(),Vec::new()),
    ]{
        let sp=loader::build_stack(&StackInputs{argv:&argv,envp:&envp,execfn:b"stack-probe",program:&program,interp_base:0x9000}).unwrap();
        assert_eq!(sp%16,0);
        let words=unsafe{std::slice::from_raw_parts(sp as*const u64,1+argv.len()+1+envp.len()+1+40)};
        assert_eq!(words[0],argv.len()as u64);
        for(index,expected)in argv.iter().enumerate(){assert_eq!(unsafe{CStr::from_ptr(words[1+index]as*const i8)}.to_bytes(),expected);}
        assert_eq!(words[1+argv.len()],0);
        let env_at=2+argv.len();
        for(index,expected)in envp.iter().enumerate(){assert_eq!(unsafe{CStr::from_ptr(words[env_at+index]as*const i8)}.to_bytes(),expected);}
        assert_eq!(words[env_at+envp.len()],0);
        let aux_at=env_at+envp.len()+1;
        let mut vdso=0;let mut found_entry=false;
        for pair in words[aux_at..].chunks_exact(2){if pair[0]==0{break;}if pair[0]==33{vdso=pair[1];}if pair[0]==9{assert_eq!(pair[1],program.entry);found_entry=true;}}
        assert!(found_entry);assert_ne!(vdso,0);
        let region=vmmap::region_at(sp).unwrap();
        assert_eq!(region.prot,3);assert!(!region.shared);assert!(region.file.is_none());
        assert_eq!(region.usage.share_mode,vmmap::SM_PRIVATE);assert_eq!(region.usage.ref_count,1);
        let mut command=Vec::new();for arg in &argv{command.extend(arg);command.push(0);}
        assert_eq!(procrec::cmdline(std::process::id()as i32).unwrap(),command);
        if !argv.is_empty(){
            let arg=words[1];assert!(!procrec::is_strings(sp));assert!(procrec::is_strings(arg));
            assert!(sp+(words.len()as u64*8)<=(arg&!(PAGE-1)));
            let old=unsafe{*(arg as*const u8)};unsafe{*(arg as*mut u8)=b'Z'};
            command[0]=b'Z';assert_eq!(procrec::cmdline(std::process::id()as i32).unwrap(),command);
            unsafe{*(arg as*mut u8)=old};
        }
        unsafe{libc::munmap((vdso-PAGE)as*mut _,(2*PAGE)as usize);}
        // The stack occupies the architecture's final eight MiB, including its guard.
        unsafe{libc::munmap((super::arena::HI-(8<<20))as*mut _,8<<20);}
    }
    std::fs::remove_dir_all(directory).unwrap();
}
