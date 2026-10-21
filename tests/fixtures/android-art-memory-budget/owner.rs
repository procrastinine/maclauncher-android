use memory_budget::{configured_total,snapshot};
fn physical() -> u64 {
    let mut value=0u64;let mut size=size_of::<u64>();
    assert_eq!(unsafe{libc::sysctlbyname(c"hw.memsize".as_ptr(),(&raw mut value).cast(),&mut size,std::ptr::null_mut(),0)},0);
    value
}
fn main(){
    let physical=physical();let total=configured_total(physical);
    let expected=std::env::args().nth(1).unwrap().parse::<u64>().unwrap().min(physical);
    assert_eq!(total,expected,"same C-int MiB parser and physical-memory cap");
    let before=snapshot(total);assert!(before.available<=total);
    if total>=256*1024*1024{
        let size=32*1024*1024;
        let memory=unsafe{libc::mmap(std::ptr::null_mut(),size,libc::PROT_READ|libc::PROT_WRITE,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};
        assert_ne!(memory,libc::MAP_FAILED);unsafe{std::ptr::write_bytes(memory.cast::<u8>(),0x6a,size);}
        let after=snapshot(total);assert!(before.available.saturating_sub(after.available)>=24*1024*1024,"headroom follows actual charged private pages: {before:?} -> {after:?}");
        assert_eq!(unsafe{libc::munmap(memory,size)},0);
        assert!(snapshot(total).available>=before.available.saturating_sub(4*1024*1024));
    }
    println!("PASS actual native memory owner total={total} available={} physical-cap={physical}",before.available);
}
