#[cfg(test)]
mod completion_cpu_tests {
    use super::*;
    use std::os::fd::AsFd;
    fn state(value: u64) -> Arc<CompletionState> {
        Arc::new(CompletionState { event: 0, value, error: AtomicI32::new(0) })
    }
    fn status(file: &OwnedFd) -> i32 {
        match crate::state(file.as_fd()) {
            State::Signaled { status, .. } => status,
            other => panic!("fence did not retire: {other:?}"),
        }
    }
    #[test]
    fn completion_cpu_real_writer_failure_and_callback_races() {
        let initial_private=crate::PRIVATE.lock().unwrap().len();
        // Failure before publication is latched into the real writer record.
        let early=state(1000);unsafe {command_completed(Arc::as_ptr(&early).cast(),-5)};
        let (file,writer)=pair().unwrap();assert!(!register_pending(std::ptr::null_mut(),1000,writer,Some(&early)));
        assert_eq!(status(&file),-5);assert!(crate::wait(file.as_fd(),10));
        assert!(crate::is_sync_file(file.as_fd()));
        let mut poll=libc::pollfd{fd:std::os::fd::AsRawFd::as_raw_fd(&file),events:libc::POLLIN,revents:0};
        assert_eq!(unsafe {libc::poll(&mut poll,1,0)},1);assert_ne!(poll.revents&libc::POLLIN,0);
        assert_eq!(poll.revents&(libc::POLLERR|libc::POLLHUP),0);
        // A descriptor duplicated after retirement observes the same error.
        let duplicate=file.try_clone().unwrap();assert_eq!(status(&duplicate),-5);
        // Failure after publication claims and closes its exact writer once.
        let late=state(1001);let (late_file,writer)=pair().unwrap();
        assert!(register_pending(std::ptr::null_mut(),1001,writer,Some(&late)));
        unsafe {command_completed(Arc::as_ptr(&late).cast(),-5);command_completed(Arc::as_ptr(&late).cast(),-5)};
        assert_eq!(status(&late_file),-5);
        // Success is delivered only by the original event listener.
        let good=state(1002);let (good_file,writer)=pair().unwrap();
        assert!(register_pending(std::ptr::null_mut(),1002,writer,Some(&good)));
        unsafe {command_completed(Arc::as_ptr(&good).cast(),0)};
        assert_eq!(crate::state(good_file.as_fd()),State::Active);
        unsafe {reached(std::ptr::null(),std::ptr::null_mut(),1002)};
        assert_eq!(status(&good_file),1);
        // Unobserved/imported work with another value remains pending.
        let one=state(1003);let (one_file,one_writer)=pair().unwrap();let (borrowed,borrowed_writer)=pair().unwrap();
        assert!(register_pending(std::ptr::null_mut(),1003,one_writer,Some(&one)));
        assert!(register_pending(std::ptr::null_mut(),1004,borrowed_writer,None));
        unsafe {command_completed(Arc::as_ptr(&one).cast(),-5)};
        assert_eq!(status(&one_file),-5);assert_eq!(crate::state(borrowed.as_fd()),State::Active);
        unsafe {reached(std::ptr::null(),std::ptr::null_mut(),1004)};assert_eq!(status(&borrowed),1);
        // Competing real terminal callbacks must retire the writer exactly once.
        for value in 2000..2032 {
            let completion=state(value);let (file,writer)=pair().unwrap();
            assert!(register_pending(std::ptr::null_mut(),value,writer,Some(&completion)));
            let raw=Arc::as_ptr(&completion) as usize;
            unsafe {completion_retain(raw as *const c_void)};
            let error=std::thread::spawn(move||unsafe {command_completed(raw as *const c_void,-5);completion_release(raw as *const c_void)});
            let event=std::thread::spawn(move||unsafe {reached(std::ptr::null(),std::ptr::null_mut(),value)});
            error.join().unwrap();event.join().unwrap();assert!([1,-5].contains(&status(&file)));
            assert_eq!(Arc::strong_count(&completion),1);assert!(PENDING.lock().unwrap().is_empty());
        }
        assert_eq!(crate::PRIVATE.lock().unwrap().len(),initial_private);
    }
}
