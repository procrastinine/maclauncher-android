// The shared-event listener remains the success owner. A real failed command
// buffer claims the same pending writer once, using the Linux fence error ABI.
use std::sync::Arc;
use std::sync::atomic::{AtomicI32, Ordering};

const COMPLETION_KEY: &CStr = c"maclauncher.angle.fence.completion.v1";

struct CompletionState {
    event: usize,
    value: u64,
    error: AtomicI32,
}

impl Drop for CompletionState {
    fn drop(&mut self) {
        // This independent reference prevents event-address reuse until the
        // exact registered command buffer has delivered its completion.
        unsafe { objc_release(self.event as Id) };
    }
}

#[repr(C)]
struct CompletionRequest {
    size: u32,
    version: u32,
    attached: u32,
    reserved: u32,
    value: u64,
    context: *const c_void,
    retain: unsafe extern "C" fn(*const c_void),
    completed: unsafe extern "C" fn(*const c_void, i32),
    release: unsafe extern "C" fn(*const c_void),
}

const _: () = assert!(size_of::<CompletionRequest>() == 56);

unsafe extern "C" fn completion_retain(context: *const c_void) {
    unsafe { Arc::increment_strong_count(context.cast::<CompletionState>()) };
}

unsafe extern "C" fn completion_release(context: *const c_void) {
    unsafe { Arc::decrement_strong_count(context.cast::<CompletionState>()) };
}

unsafe extern "C" fn command_completed(context: *const c_void, status: i32) {
    let state = unsafe { &*context.cast::<CompletionState>() };
    if status >= 0 { return; }
    state.error.store(status, Ordering::Release);
    let done: Vec<_> = {
        let mut pending = PENDING.lock().unwrap_or_else(|error| error.into_inner());
        let (done, left) = std::mem::take(&mut *pending).into_iter()
            .partition(|(event, value, _)| *event == state.event && *value == state.value);
        *pending = left;
        done
    };
    for (event, _, writer) in done {
        writer.signal(status);
        unsafe { objc_release(event as Id) };
    }
}

fn register_pending(event: Id, value: u64, writer: Writer,
                    completion: Option<&CompletionState>) -> bool {
    // The error is latched before acquiring this same registry lock. Failure
    // before, during or after publication therefore cannot miss its writer.
    let mut pending = PENDING.lock().unwrap_or_else(|error| error.into_inner());
    let failure = completion.map_or(0, |state| state.error.load(Ordering::Acquire));
    if failure < 0 {
        drop(pending);
        writer.signal(failure);
        return false;
    }
    unsafe { objc_retain(event) };
    pending.push((event as usize, value, writer));
    true
}

/// Registration storage remains alive across the synchronous eglCreateSync.
/// ANGLE copies the request and retains its context before acknowledging it.
pub struct CompletionObservation {
    state: Arc<CompletionState>,
    request: Box<CompletionRequest>,
}

impl CompletionObservation {
    pub fn detach(&self) {
        unsafe { objc_setAssociatedObject(self.state.event as Id, sel(COMPLETION_KEY),
                                         std::ptr::null_mut(), 1) };
    }

    /// Export only a fence whose actual producer acknowledged its observer.
    pub fn fence(&self) -> io::Result<OwnedFd> {
        if self.request.attached != 1 { return Err(io::ErrorKind::Unsupported.into()); }
        unsafe { fence_impl(self.state.event as Id, self.state.value, Some(&self.state)) }
    }
}

impl Drop for CompletionObservation {
    fn drop(&mut self) { self.detach(); }
}

/// Observe only this owned event/value; imported and borrowed events do not
/// install this metadata and retain their ordinary shared-event behavior.
///
/// # Safety
/// `event` is a newly owned MTLSharedEvent, used by the next eglCreateSync.
pub unsafe fn observe_completion(event: Id, value: u64) -> io::Result<CompletionObservation> {
    if event.is_null() { return Err(io::ErrorKind::InvalidInput.into()); }
    let state = Arc::new(CompletionState {
        event: unsafe { objc_retain(event) } as usize, value, error: AtomicI32::new(0),
    });
    let mut request = Box::new(CompletionRequest {
        size: size_of::<CompletionRequest>() as u32, version: 1, attached: 0,
        reserved: 0, value, context: Arc::as_ptr(&state).cast(),
        retain: completion_retain, completed: command_completed, release: completion_release,
    });
    let pointer: *const c_void = (&raw mut *request).cast();
    let class = unsafe { objc_getClass(c"NSValue".as_ptr()) };
    if class.is_null() { return Err(io::ErrorKind::Unsupported.into()); }
    let boxed = send!(class, c"alloc" => Id);
    let boxed = send!(boxed, c"initWithBytes:objCType:" => Id,
        *const c_void = (&pointer as *const *const c_void).cast(),
        *const c_char = c"^v".as_ptr());
    if boxed.is_null() { return Err(io::ErrorKind::OutOfMemory.into()); }
    unsafe {
        objc_setAssociatedObject(event, sel(COMPLETION_KEY), boxed, 1);
        objc_release(boxed);
    }
    Ok(CompletionObservation { state, request })
}

/// Existing callers retain the event-only contract.
pub unsafe fn fence(event: Id, value: u64) -> io::Result<OwnedFd> {
    unsafe { fence_impl(event, value, None) }
}
