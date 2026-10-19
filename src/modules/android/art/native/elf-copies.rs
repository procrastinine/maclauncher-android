// File-copy loaders (including Bionic's 4K-on-16K loader) keep the same
// source/inode owner as ordinary mmap. This extends copies.rs, not a second
// address registry. A read qualifies only when it copies a complete original
// PT_LOAD, including its 4K leading bytes, into an aligned anonymous range.
use std::{os::fd::{AsRawFd, BorrowedFd}, sync::Arc};
use super::verified_source::VerifiedSource;

pub(super) fn note_elf_read(pin: &super::fdtab::Pinned, base: u64, len: u64, offset: u64) {
    const COMPAT_PAGE: u64 = 4096;
    if base & (COMPAT_PAGE - 1) != 0 || len == 0 { return; }
    let fd = pin.descriptor().as_raw_fd();
    let Some((start, size)) = crate::xrt::elf_extent(fd, offset) else { return; };
    let Ok(source) = VerifiedSource::from_pinned(pin) else { return; };
    let mut head = [0u8; 64];
    if source.read_exact_at(&mut head, start).is_err() { return; }
    let Ok(header) = crate::elf::parse_header(&head) else { return; };
    let Some(end) = header.e_phoff.checked_add(header.e_phnum as u64 * header.e_phentsize as u64) else { return; };
    if end > size { return; }
    let mut bytes = vec![0u8; end.max(64) as usize];
    if source.read_exact_at(&mut bytes, start).is_err() { return; }
    let Ok(headers) = crate::elf::parse_phdrs(&bytes, &header) else { return; };
    let valid = headers.iter().any(|p| {
        p.p_type == crate::elf::PT_LOAD && p.p_filesz != 0
            && start.checked_add(p.p_offset & !(COMPAT_PAGE - 1)) == Some(offset)
            && p.p_offset.checked_add(p.p_filesz).is_some_and(|end|
                end <= size && end - (p.p_offset & !(COMPAT_PAGE - 1)) == len)
    });
    if !valid { return; }
    let Some(end) = base.checked_add(len) else { return; };
    if !super::vmmap::regions(base, end).all(|r|
        r.file.is_none() && !r.shared && r.prot & libc::PROT_WRITE as u32 != 0) { return; }
    let Some(host) = crate::xrt::fd_path(fd) else { return; };
    let guest = super::procfs::fd_guest_path(fd).unwrap_or_default();
    let id = source.identity();
    note_file(base, len, &host, &guest, offset, (id.dev, id.ino));
    if let Some(copy) = COPIES.lock().unwrap().get_mut(&base) {
        copy.source = Some(Arc::new(source));
    }
}

/// Snapshot existing mapping records without holding the registry during file
/// verification, translation, allocation or executable publication.
pub(super) fn file_copies(lo: u64, hi: u64) -> Vec<(u64, Copy)> {
    COPIES.lock().unwrap().range(..hi)
        .filter(|(start, copy)| **start < hi && copy.end > lo && copy.source.is_some())
        .map(|(start, copy)| (*start, copy.clone())).collect()
}

/// Original ELF sites describe only the bytes copied from the file. Once those
/// sites have been translated, later writable generations must be scanned from
/// their current bytes. Keep the ordinary mapping identity and offsets, while
/// retiring this range's original-site source lease. Other parts of the copied
/// LOAD may still need their first translation.
pub(super) fn forget_elf_sites(lo: u64, hi: u64) {
    if lo >= hi { return; }
    let mut copies = COPIES.lock().unwrap();
    let starts: Vec<_> = copies.range(..hi)
        .filter(|(start, copy)| **start < hi && copy.end > lo && copy.source.is_some())
        .map(|(start, _)| *start).collect();
    for start in starts {
        let copy = copies.remove(&start).unwrap();
        let begin = start.max(lo);
        let end = copy.end.min(hi);
        if start < begin { copies.insert(start, Copy { end: begin, ..copy.clone() }); }
        copies.insert(begin, Copy {
            end, offset: copy.offset + begin - start, source: None, ..copy.clone()
        });
        if end < copy.end {
            copies.insert(end, Copy { offset: copy.offset + end - start, ..copy });
        }
    }
}

fn save_source(w: &mut super::fork_state::Writer, source: Option<&Arc<VerifiedSource>>) {
    w.opt(source, |w, source| {
        let fd = source.as_raw_fd();
        w.retain_private(fd);
        w.i32(fd);
    });
}

fn restore_source(r: &mut super::fork_state::Reader) -> Option<Arc<VerifiedSource>> {
    r.opt(|r| {
        let fd = r.i32();
        let borrowed = unsafe { BorrowedFd::borrow_raw(fd) };
        let result = super::regular_file::adopt_kernel(borrowed, 0)
            .and_then(|description| VerifiedSource::from_descriptor(borrowed, description));
        super::fdtab::remember_restored_private(&[fd]);
        match result {
            Ok(source) => Some(Arc::new(source)),
            Err(_) => { r.invalidate(); None }
        }
    }).flatten()
}
