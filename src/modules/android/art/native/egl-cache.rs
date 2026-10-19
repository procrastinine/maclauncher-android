//! ANGLE uses the shared native EGL program cache after original initialization.
use std::ffi::{c_char,c_void};
use std::sync::atomic::{AtomicUsize,Ordering};
static EGL:AtomicUsize=AtomicUsize::new(0);
unsafe extern "C" {
    fn kl_android_egl_cache_install(display:usize,symbol:extern "C" fn(*const c_char)->*mut c_void);
}
extern "C" fn symbol(name:*const c_char)->*mut c_void {
    let library=EGL.load(Ordering::Acquire);
    if name.is_null()||library==0{return std::ptr::null_mut();}
    // The existing GPU loader retains this exact provider for process life.
    // Callback-only exports need not occur in the guest's forwarding table.
    unsafe {libc::dlsym(library as *mut c_void,name)}
}
pub fn provider(library:*mut c_void){EGL.store(library as usize,Ordering::Release);}
pub fn initialized(display:usize){
    // The original display owner calls this once per initialized native display.
    unsafe {kl_android_egl_cache_install(display,symbol);}
}
