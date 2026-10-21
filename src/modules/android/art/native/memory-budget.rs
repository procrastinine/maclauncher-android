//! Accounting policy for the existing AIM memory owner. The configured MiB
//! budget and Darwin process footprint follow Klepton's native memory policy;
//! this module does not load another VM or libc into the ART process.
use std::sync::atomic::{AtomicU64,AtomicUsize,Ordering};

#[derive(Clone, Copy, Debug)]
pub struct Snapshot {
    pub total: u64,
    pub available: u64,
}

/// Match kl_env_int's strtol(base 0) -> C int and unsigned MiB conversion.
/// Configuration is fixed before process entry; fork retains the same budget.
pub fn configured_total(physical: u64) -> u64 {
    // Lock-free publication remains usable if first read races with fork.
    static TOTAL: AtomicU64 = AtomicU64::new(u64::MAX);
    let cached=TOTAL.load(Ordering::Acquire);
    if cached!=u64::MAX{return cached;}
    let value={
        let value = unsafe { libc::getenv(c"KL_MEM_TOTAL_MB".as_ptr()) };
        let mib = if value.is_null() { 6144 } else {
            unsafe { libc::strtol(value, std::ptr::null_mut(), 0) as libc::c_int }
        };
        let requested = (mib as u64).wrapping_shl(20);
        if physical != 0 { requested.min(physical) } else { requested }
    };
    TOTAL.compare_exchange(u64::MAX,value,Ordering::AcqRel,Ordering::Acquire).map(|_|value).unwrap_or_else(|current|current)
}

fn footprint() -> Option<u64> {
    let mut usage: libc::rusage_info_v2 = unsafe { std::mem::zeroed() };
    let result = unsafe { libc::proc_pid_rusage(libc::getpid(), libc::RUSAGE_INFO_V2,
        (&raw mut usage).cast()) };
    (result == 0 && usage.ri_phys_footprint != 0).then_some(usage.ri_phys_footprint)
}

fn native_headroom() -> Option<u64> {
    type Available = unsafe extern "C" fn() -> usize;
    static FUNCTION: AtomicUsize = AtomicUsize::new(usize::MAX);
    let mut address=FUNCTION.load(Ordering::Acquire);
    if address==usize::MAX {
        let resolved=unsafe { libc::dlsym(libc::RTLD_DEFAULT,c"os_proc_available_memory".as_ptr()) } as usize;
        address=FUNCTION.compare_exchange(usize::MAX,resolved,Ordering::AcqRel,Ordering::Acquire).map(|_|resolved).unwrap_or_else(|current|current);
    }
    if address==0{return None;}
    let function=unsafe{std::mem::transmute::<usize,Available>(address)};
    let value=unsafe{function()} as u64;
    (value != 0).then_some(value)
}

/// Used only when Darwin cannot measure this process's charged footprint.
/// The real free+inactive count remains bounded by the configured budget.
#[allow(deprecated)]
fn native_free() -> u64 {
    unsafe extern "C" { fn mach_port_deallocate(task:libc::mach_port_t,name:libc::mach_port_t)->libc::c_int; }
    let mut vm: libc::vm_statistics64 = unsafe { std::mem::zeroed() };
    let mut count = libc::HOST_VM_INFO64_COUNT;
    let host = unsafe { libc::mach_host_self() };
    #[allow(deprecated)]
    let result = unsafe { libc::host_statistics64(host, libc::HOST_VM_INFO64,
        (&raw mut vm).cast(), &mut count) };
    unsafe { mach_port_deallocate(libc::mach_task_self(), host); }
    let page = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    if result == 0 && page > 0 {
        (u64::from(vm.free_count) + u64::from(vm.inactive_count)).saturating_mul(page as u64)
    } else { 0 }
}

pub fn snapshot(total: u64) -> Snapshot {
    let remaining = footprint().map_or_else(|| native_free().min(total), |used| total.saturating_sub(used));
    let available = native_headroom().map_or(remaining, |headroom| remaining.min(headroom));
    Snapshot { total, available }
}
