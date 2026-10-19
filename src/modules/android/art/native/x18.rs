//! VM binding for the common Klepton A64 instruction translator. AIM owns
//! all mappings, thread slots, Linux contexts and executable publication.
unsafe extern "C" {
    fn klx_core_classify(word: u32) -> i32;
    fn klx_stack_resume(context: *mut libc::c_void) -> i32;
    fn klx_core_emit(word: u32, site: u64, pool: u64, slot: u32, ctr: u64,
                     words: *mut u32, capacity: usize) -> i32;
    fn klx_elf_instruction_evidence(file: *const u8, size: usize, state: *mut u8, count: usize) -> i32;
    fn klx_instruction_evidence(words: *const u32, count: usize, base: u64,
                               roots: *const u64, root_count: usize, state: *mut u8) -> i32;
}

pub(crate) fn evidence(bytes: &[u8], base: u64, roots: &[u64], state: &mut [u8]) {
    let words: Vec<u32> = bytes.chunks_exact(4).map(|w|u32::from_le_bytes(w.try_into().unwrap())).collect();
    assert_eq!(words.len(),state.len());
    // Pure shared analysis. Its bytes belong to the original source image,
    // before either loader publishes any translated instructions.
    assert_eq!(unsafe {klx_instruction_evidence(words.as_ptr(),words.len(),base,
        roots.as_ptr(),roots.len(),state.as_mut_ptr())},0,"instruction evidence allocation");
}

pub(crate) fn elf_evidence(bytes: &[u8]) -> Vec<u8> {
    let mut state=vec![0;bytes.len().div_ceil(4)];
    assert_eq!(unsafe {klx_elf_instruction_evidence(bytes.as_ptr(),bytes.len(),state.as_mut_ptr(),state.len())},0,
        "original ELF instruction evidence");
    state
}

pub const MAX_WORDS: usize = 12;

/// The shared hardware veneer completes an arbitrary SP update through the
/// existing interrupted context, after validating its complete instruction body.
pub unsafe fn resume_stack(context: *mut libc::c_void) -> bool {
    unsafe { klx_stack_resume(context) != 0 }
}

pub fn classify(word: u32) -> bool {
    // SAFETY: pure instruction metadata, with no host or guest memory access.
    unsafe { klx_core_classify(word) != 0 }
}

pub fn terminal(word: u32) -> Option<u32> {
    // The shared decoder establishes that this is a real GP-register operand;
    // only the VM's continuation differs from the common direct veneer.
    let class = unsafe { klx_core_classify(word) };
    if class >> 8 != 1 && class >> 8 != 3 { return None; }
    match word {
        0xd61f0240 => Some(0), // br x18
        0xd63f0240 => Some(1), // blr x18
        0xd65f0240 => Some(2), // ret x18
        _ => None,
    }
}

pub fn emit(word: u32, site: u64, pool: u64, ctr: u32) -> Option<Vec<u32>> {
    if let Some(op) = terminal(word) {
        // Keep the continuation relative to the trap so the original ELF
        // loader's ASLR bias applies equally to cached and live veneers.
        let continuation=(site+4).wrapping_sub(pool);
        return Some(vec![super::brk(super::BRK_TAG | ((super::Kind::X18 as u32) << 5) | op),
                         continuation as u32, (continuation >> 32) as u32]);
    }
    let mut words = [0; MAX_WORDS];
    // The shadow-call-stack slot is the original architectural x18 value in
    // AIM's signal/ptrace/clone contract, not a second register owner.
    let count = unsafe { klx_core_emit(word, site, pool, super::slot::SCS_KEY as u32,
                                      ctr as u64, words.as_mut_ptr(), words.len()) };
    (count > 0).then(|| words[..count as usize].to_vec())
}
