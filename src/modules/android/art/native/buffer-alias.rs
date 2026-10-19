// A host GPU object can outlive the guest's EGLImage and its mmap. Keep a
// shared VM alias until Metal releases the actual storage, without copying it.
use std::ffi::c_void;

unsafe extern "C" {
    static mach_task_self_: u32;
    static _NSConcreteStackBlock: [*const c_void; 32];
    fn mach_vm_remap(target: u32, address: *mut u64, size: u64, mask: u64,
        flags: i32, source: u32, source_address: u64, copy: i32,
        current: *mut i32, maximum: *mut i32, inherit: i32) -> i32;
    fn mach_vm_deallocate(task: u32, address: u64, size: u64) -> i32;
}

#[repr(C)]
struct Descriptor { reserved: usize, size: usize }

#[repr(C)]
pub struct Block {
    isa: *const c_void,
    flags: i32,
    reserved: i32,
    invoke: unsafe extern "C" fn(*const Block, *mut c_void, usize),
    descriptor: *const Descriptor,
    address: u64,
    length: u64,
}

unsafe extern "C" fn released(block: *const Block, _: *mut c_void, _: usize) {
    // Metal copies the stack block and invokes it once when the buffer dies.
    unsafe { mach_vm_deallocate(mach_task_self_, (*block).address, (*block).length); }
}

pub struct Mapping { pub address: u64, length: u64 }

impl Mapping {
    pub fn new(source: u64, length: u64) -> Option<Self> {
        let mut address = 0;
        let (mut current, mut maximum) = (0, 0);
        // VM_FLAGS_ANYWHERE, shared mapping, VM_INHERIT_SHARE. The alias
        // retains the same file-backed VM object after the guest unmaps it.
        let result = unsafe { mach_vm_remap(mach_task_self_, &mut address,
            length, 0, 1, mach_task_self_, source, 0,
            &mut current, &mut maximum, 0) };
        (result == 0).then_some(Self { address, length })
    }

    pub fn block(&self) -> Block {
        static DESCRIPTOR: Descriptor = Descriptor { reserved: 0, size: size_of::<Block>() };
        Block {
            isa: std::ptr::addr_of!(_NSConcreteStackBlock).cast(),
            flags: 0, reserved: 0, invoke: released, descriptor: &DESCRIPTOR,
            address: self.address, length: self.length,
        }
    }

    // Successful MTLBuffer creation copied the deallocator block and owns it.
    pub fn transferred(self) { std::mem::forget(self); }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        unsafe { mach_vm_deallocate(mach_task_self_, self.address, self.length); }
    }
}
