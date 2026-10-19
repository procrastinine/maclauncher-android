/// Bound imported-inode defaults to an existing namespace area.
/// Use the containing area rather than a narrower bind alias, as attr_key does;
/// a read-only area participates only when its own inode declares code policy.
/// Original image mounts and host ancestors never acquire imported defaults.
pub(crate) fn imported_root(host: &Path) -> Option<PathBuf> {
    let v = VFS.get()?;
    if !v.mapped {
        return strip_host_prefix(host, &v.root).map(|_| v.root.clone());
    }
    let mounts = v.mounts.read().unwrap();
    let mut root: Option<PathBuf> = None;
    for mount in mounts.iter() {
        if mount.area != Area::Writable && mount.area != Area::Image { continue; }
        if strip_host_prefix(host, &mount.host).is_some()
            && !hidden(&mounts, mount)
            && (mount.area == Area::Writable || (mount.area == Area::Image && aim_storage::imported_inodes::code_tree(&mount.host).unwrap_or(false)))
            && root.as_ref().is_none_or(|old| mount.host.as_os_str().len() < old.as_os_str().len()) {
            root = Some(mount.host.clone());
        }
    }
    if root.is_some() { return root; }
    // F_GETPATH may return the resolved physical spelling of a mapped alias.
    // Reuse the VFS's existing canonical-path owner only for that case.
    for mount in mounts.iter() {
        if mount.area != Area::Writable && mount.area != Area::Image { continue; }
        if let Some(candidate) = canonical_host(&mount.host).filter(|path| strip_host_prefix(host, path).is_some()) {
            // Visibility and import policy matter only for a containing mount.
            // Canonical published roots reuse the original VFS path cache;
            // dynamic aliases keep its existing retargeting behavior.
            if hidden(&mounts, mount) { continue; }
            if mount.area != Area::Writable && !(mount.area == Area::Image && aim_storage::imported_inodes::code_tree(&candidate).unwrap_or(false)) { continue; }
            if root.as_ref().is_none_or(|old| candidate.as_os_str().len() < old.as_os_str().len()) { root = Some(candidate); }
        }
    }
    root
}
