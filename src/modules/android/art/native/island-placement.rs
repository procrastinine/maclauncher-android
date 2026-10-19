// Cold fallback after the original three hints fail. A file's PROT_NONE
// reservation can cover each hint even when a reachable gap exists before it.
// Inspect the real Mach VM map; PROT_NONE remains occupied, never spare space.
fn alloc_stub_in_gaps(islands: &mut Vec<Island>, site: u64, bytes: u64) -> Option<usize> {
    if bytes > ISLAND_SIZE { return None; }
    let reach = (a64::BRANCH_RANGE - ISLAND_SIZE as i64 - 1) as u64;
    let mut cursor = site.saturating_sub(reach).saturating_add(PAGE - 1) & !(PAGE - 1);
    let limit = site.saturating_add(reach);
    while cursor < limit {
        let region = vm::region(cursor);
        let gap_end = region.map_or(limit, |(lo, _, _, _)| lo.min(limit));
        let size = ISLAND_SIZE.min(gap_end.saturating_sub(cursor) & !(PAGE - 1));
        if size >= bytes.max(PAGE) {
            // Prefer the end of a lower gap and the start of an upper gap.
            let hint = if gap_end <= site { gap_end - size } else { cursor };
            if let Some(island) = new_island_sized(hint, size) {
                if in_range(island.rx, site) {
                    islands.push(island);
                    return Some(islands.len() - 1);
                }
                // The non-fixed reservation can lose a race or choose another
                // address. Release only these two newly owned mappings.
                unsafe {
                    libc::munmap(island.rx as *mut _, island.size as usize);
                    libc::munmap(island.rw as *mut _, island.size as usize);
                }
            }
        }
        let Some((_, end, _, _)) = region else { break; };
        let next = end.saturating_add(PAGE - 1) & !(PAGE - 1);
        if next <= cursor { break; }
        cursor = next;
    }
    None
}
