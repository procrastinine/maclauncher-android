//! Original-file identity for published AOT translations. The mutable-code
//! caller owns the exclusive VM epoch; all changes here affect staging only.
use std::{collections::HashMap, fs::File, io, os::fd::AsRawFd, path::{Path,PathBuf}, sync::{Arc,LazyLock,Mutex}};
use super::FileStat;
use aim_storage::private_fd::PrivateFile;
#[derive(Clone)]
struct Binding { original:PathBuf, original_stat:FileStat, derived:PathBuf, derived_stat:FileStat }
static BINDINGS:LazyLock<Mutex<HashMap<FileStat,Arc<Binding>>>>=LazyLock::new(Default::default);
fn invalid()->io::Error{io::Error::from_raw_os_error(libc::ENOEXEC)}
fn stale()->io::Error{io::Error::from_raw_os_error(libc::ESTALE)}
fn stat(file:&PrivateFile)->io::Result<FileStat>{FileStat::of_fd(file.as_raw_fd()).map(|value|value.0)}
fn open(path:&Path,expected:&FileStat)->io::Result<PrivateFile>{
    use std::os::unix::fs::OpenOptionsExt;
    let file=PrivateFile::allocate(||File::options().read(true).custom_flags(libc::O_NOFOLLOW|libc::O_CLOEXEC).open(path))?;
    if &stat(&file)?!=expected||FileStat::of_path(path)?!=*expected{return Err(stale());}Ok(file)
}
pub(super) fn bind(original:&Path,expected:&FileStat,derived_fd:i32,derived:&Path)->io::Result<()> {
    // Identity checks happen on both opened descriptions and their current
    // names. A renamed/replaced input fails closed rather than rebinding it.
    let source=open(original,expected)?;
    let derived_stat=FileStat::of_fd(derived_fd)?.0;
    if FileStat::of_path(derived)?!=derived_stat||stat(&source)?!=*expected{return Err(stale());}
    let binding=Arc::new(Binding{original:original.to_owned(),original_stat:expected.clone(),derived:derived.to_owned(),derived_stat:derived_stat.clone()});
    let mut owner=BINDINGS.lock().unwrap_or_else(|e|e.into_inner());
    if let Some(known)=owner.get(&derived_stat){
        if known.original!=binding.original||known.original_stat!=binding.original_stat||known.derived!=binding.derived{return Err(stale());}
    }else{owner.insert(derived_stat,binding);}Ok(())
}
pub(super) fn bind_paths(original:&Path,original_stat:&FileStat,derived:&Path)->io::Result<()> {
    let translated=PrivateFile::allocate(||File::open(derived))?;
    bind(original,original_stat,translated.as_raw_fd(),derived)
}
fn lookup(path:&Path,dev:u64,ino:u64)->io::Result<Option<Arc<Binding>>>{
    let bindings={let owner=BINDINGS.lock().unwrap_or_else(|e|e.into_inner());owner.values().filter(|known|known.derived_stat.dev==dev&&known.derived_stat.ino==ino).cloned().collect::<Vec<_>>()};
    if bindings.is_empty(){return if super::in_cache(path){Err(stale())}else{Ok(None)};}
    let current=FileStat::of_path(path)?;
    if current.dev!=dev||current.ino!=ino{return Err(stale());}
    bindings.into_iter().find(|binding|binding.derived_stat==current).map(Some).ok_or_else(stale)
}
fn read_exact_at(file:&PrivateFile,bytes:&mut[u8],offset:u64)->io::Result<()>{
    let mut done=0;while done<bytes.len(){let at=offset.checked_add(done as u64).and_then(|value|i64::try_from(value).ok()).ok_or_else(invalid)?;
        let count=unsafe{libc::pread(file.as_raw_fd(),bytes[done..].as_mut_ptr().cast(),bytes.len()-done,at)};
        if count<0{let error=io::Error::last_os_error();if error.kind()==io::ErrorKind::Interrupted{continue;}return Err(error);}
        if count==0{return Err(io::Error::from(io::ErrorKind::UnexpectedEof));}done+=count as usize;
    }Ok(())
}
fn original_count(binding:&Binding,derived:&PrivateFile,offset:u64,length:usize)->io::Result<usize>{
    let end=offset.checked_add(length as u64).ok_or_else(invalid)?;
    let page=unsafe{libc::sysconf(libc::_SC_PAGESIZE)};
    if page<=0||!(page as u64).is_power_of_two(){return Err(invalid());}
    let page=page as u64;
    let rounded=binding.original_stat.size.checked_add(page-1).ok_or_else(invalid)?&!(page-1);
    // A file's final mapped page has an architectural zero-filled EOF tail.
    // Derived bytes inside that tail must also be zero; real appended code,
    // further derived pages, or missing original bytes remain unsupported.
    if end>rounded{return Err(invalid());}
    let count=binding.original_stat.size.saturating_sub(offset).min(length as u64);
    if count>0&&offset.checked_add(count).ok_or_else(invalid)?>binding.derived_stat.size{return Err(invalid());}
    let tail=offset.max(binding.original_stat.size);let stop=end.min(binding.derived_stat.size);
    if tail<stop{
        let mut published=vec![0;(stop-tail)as usize];read_exact_at(derived,&mut published,tail)?;
        if published.iter().any(|byte|*byte!=0){return Err(invalid());}
    }
    Ok(count as usize)
}
fn restore_part(binding:&Binding,offset:u64,bytes:&mut[u8])->io::Result<()> {
    let source=open(&binding.original,&binding.original_stat)?;
    let derived=open(&binding.derived,&binding.derived_stat)?;
    let count=original_count(binding,&derived,offset,bytes.len())?;
    let mut original=vec![0;count];let mut published=vec![0;count];
    read_exact_at(&source,&mut original,offset)?;read_exact_at(&derived,&mut published,offset)?;
    if stat(&source)?!=binding.original_stat||stat(&derived)?!=binding.derived_stat||FileStat::of_path(&binding.original)?!=binding.original_stat||FileStat::of_path(&binding.derived)?!=binding.derived_stat{return Err(stale());}
    for at in (0..count).step_by(4){let size=(count-at).min(4);let range=at..at+size;
        if original[range.clone()]==published[range.clone()]{continue;}
        if size!=4||offset%4!=0{return Err(invalid());}
        if bytes[range.clone()]!=published[range.clone()]&&bytes[range.clone()]!=original[range.clone()]{return Err(invalid());}
        bytes[range.clone()].copy_from_slice(&original[range]);
    }Ok(())
}
fn reload_part(path:&Path,dev:u64,ino:u64,offset:u64,bytes:&mut[u8])->io::Result<()> {
    let binding=lookup(path,dev,ino)?;
    let (source_path,expected,derived):(&Path,FileStat,Option<PrivateFile>)=if let Some(binding)=binding.as_ref(){
        let derived=open(&binding.derived,&binding.derived_stat)?;
        original_count(binding,&derived,offset,bytes.len())?;
        (&binding.original,binding.original_stat,Some(derived))
    }else{
        let expected=FileStat::of_path(path)?;
        if expected.dev!=dev||expected.ino!=ino{return Err(stale());}
        (path,expected,None)
    };
    let source=open(source_path,&expected)?;
    let count=expected.size.saturating_sub(offset).min(bytes.len()as u64)as usize;
    read_exact_at(&source,&mut bytes[..count],offset)?;
    if stat(&source)?!=expected||FileStat::of_path(source_path)?!=expected{return Err(stale());}
    if let (Some(binding),Some(derived))=(binding.as_ref(),derived.as_ref()){
        if stat(derived)?!=binding.derived_stat||FileStat::of_path(&binding.derived)?!=binding.derived_stat{return Err(stale());}
    }
    Ok(())
}
/// Stage a private mapping's discard image without touching public bytes.
/// Anonymous extents become zero; bound AOT files use authenticated originals,
/// and ordinary file mappings use their current inode and exact file offset.
pub fn reload_original_mapping(base:u64,length:u64)->io::Result<Vec<u8>> {
    let end=base.checked_add(length).ok_or_else(invalid)?;
    let size=usize::try_from(length).map_err(|_|invalid())?;
    let mut bytes=Vec::new();bytes.try_reserve_exact(size).map_err(|_|io::Error::from_raw_os_error(libc::ENOMEM))?;bytes.resize(size,0);
    let mut cursor=base;
    for region in crate::sys::original_file_mapping_regions(base,end){
        if region.start!=cursor{return Err(stale());}
        if let Some((path,dev,ino))=&region.file{
            reload_part(path,*dev,*ino,region.offset,&mut bytes[(region.start-base)as usize..(region.end-base)as usize])?;
        }else{
            for (start,copy)in crate::sys::original_file_copy_regions(region.start,region.end){
                let at=start.max(region.start);let stop=copy.end.min(region.end);
                reload_part(&copy.host,copy.dev,copy.ino,copy.offset.checked_add(at-start).ok_or_else(invalid)?,&mut bytes[(at-base)as usize..(stop-base)as usize])?;
            }
        }
        cursor=region.end;
    }
    if cursor!=end{return Err(stale());}Ok(bytes)
}
/// Candidate discovery under the caller's VM epoch. It performs no source
/// file reads. A cache mapping without a complete binding remains a candidate
/// so restoration rejects it rather than silently treating derived code as raw.
pub fn has_original_mapping(base:u64,length:u64)->bool {
    let Some(end)=base.checked_add(length)else{return true;};
    let candidate=|path:&Path,dev:u64,ino:u64|{
        BINDINGS.lock().unwrap_or_else(|e|e.into_inner()).keys().any(|stat|stat.dev==dev&&stat.ino==ino)||super::in_cache(path)
    };
    for region in crate::sys::original_file_mapping_regions(base,end){
        if let Some((path,dev,ino))=&region.file{if candidate(path,*dev,*ino){return true;}}
        else{for (_,copy)in crate::sys::original_file_copy_regions(region.start,region.end){if candidate(&copy.host,copy.dev,copy.ino){return true;}}}
    }false
}
/// Restore only exact AOT word differences in the currently leased mapping.
/// Relocations and application modifications outside those words are retained;
/// a conflicting word, source replacement or synthetic tail fails atomically.
pub fn restore_original_mapping(base:u64,bytes:&mut[u8])->io::Result<()> {
    if base%4!=0||bytes.len()%4!=0{return Err(io::Error::from_raw_os_error(libc::EINVAL));}
    let end=base.checked_add(bytes.len()as u64).ok_or_else(invalid)?;
    let mut staging=bytes.to_vec();let mut cursor=base;
    for region in crate::sys::original_file_mapping_regions(base,end){
        if region.start!=cursor{return Err(stale());}
        if let Some((path,dev,ino))=&region.file {
            if let Some(binding)=lookup(path,*dev,*ino)? {
                restore_part(&binding,region.offset,&mut staging[(region.start-base)as usize..(region.end-base)as usize])?;
            }
        }else{
            // Adjacent anonymous copies may share one Darwin VM region. Walk
            // every original copy extent rather than only its first address.
            for (start,copy) in crate::sys::original_file_copy_regions(region.start,region.end){
                let at=start.max(region.start);let stop=copy.end.min(region.end);
                if let Some(binding)=lookup(&copy.host,copy.dev,copy.ino)? {
                    restore_part(&binding,copy.offset.checked_add(at-start).ok_or_else(invalid)?,&mut staging[(at-base)as usize..(stop-base)as usize])?;
                }
            }
        }
        cursor=region.end;
    }
    if cursor!=end{return Err(stale());}
    bytes.copy_from_slice(&staging);Ok(())
}
fn save_stat(w:&mut crate::sys::fork_state::Writer,s:&FileStat){w.u64(s.dev);w.u64(s.ino);w.u64(s.size);w.i64(s.mtime_s);w.i64(s.mtime_ns);}
fn load_stat(r:&mut crate::sys::fork_state::Reader)->FileStat{FileStat{dev:r.u64(),ino:r.u64(),size:r.u64(),mtime_s:r.i64(),mtime_ns:r.i64()}}
pub(super) fn fork_save(w:&mut crate::sys::fork_state::Writer){
    let owner=BINDINGS.lock().unwrap_or_else(|e|e.into_inner());
    w.seq(owner.values(),|w,b|{w.path(&b.original);save_stat(w,&b.original_stat);w.path(&b.derived);save_stat(w,&b.derived_stat);});
}
pub(super) fn fork_restore(r:&mut crate::sys::fork_state::Reader){
    let bindings=r.seq(|r|{let original=r.path();let original_stat=load_stat(r);let derived=r.path();let derived_stat=load_stat(r);(derived_stat.clone(),Arc::new(Binding{original,original_stat,derived,derived_stat}))});
    let mut owner=BINDINGS.lock().unwrap_or_else(|e|e.into_inner());owner.clear();owner.extend(bindings);
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_original_file_mapping_restoration_and_fork_identity(){
        let root=std::env::temp_dir().join(format!("aim-original-files-{}",std::process::id()));std::fs::create_dir_all(&root).unwrap();
        let original=root.join("original");let derived=root.join("published");let page=16384usize;
        let mut raw=vec![0x31u8;page*2];raw[4..8].copy_from_slice(&0xd4000001u32.to_le_bytes());raw[page+12..page+16].copy_from_slice(&0xd53bd040u32.to_le_bytes());
        let mut translated=raw.clone();translated[4..8].copy_from_slice(&0x14001000u32.to_le_bytes());translated[page+12..page+16].copy_from_slice(&0x14002000u32.to_le_bytes());
        std::fs::write(&original,&raw).unwrap();std::fs::write(&derived,&translated).unwrap();let expected=FileStat::of_path(&original).unwrap();
        bind_paths(&original,&expected,&derived).unwrap();let file=File::open(&derived).unwrap();let mapped=unsafe{libc::mmap(std::ptr::null_mut(),raw.len(),libc::PROT_READ,libc::MAP_PRIVATE,file.as_raw_fd(),0)};assert_ne!(mapped,libc::MAP_FAILED);let base=mapped as u64;
        assert!(has_original_mapping(base,raw.len()as u64));
        assert_eq!(reload_original_mapping(base,raw.len()as u64).unwrap(),raw);
        let mut staged=translated.clone();staged[24..28].copy_from_slice(&0x12345678u32.to_le_bytes());restore_original_mapping(base,&mut staged).unwrap();assert_eq!(&staged[4..8],&raw[4..8]);assert_eq!(&staged[page+12..page+16],&raw[page+12..page+16]);assert_eq!(&staged[24..28],&0x12345678u32.to_le_bytes());
        let alternate=root.join("alternate-original");std::fs::write(&alternate,&raw).unwrap();let alternate_stat=FileStat::of_path(&alternate).unwrap();assert!(bind_paths(&alternate,&alternate_stat,&derived).is_err());let mut retained=translated.clone();restore_original_mapping(base,&mut retained).unwrap();assert_eq!(retained,raw);
        // One changed translated instruction fails without changing even an
        // earlier valid word or unrelated relocated data in caller staging.
        let mut conflict=translated.clone();conflict[page+12..page+16].copy_from_slice(&7u32.to_le_bytes());let before=conflict.clone();assert!(restore_original_mapping(base,&mut conflict).is_err());assert_eq!(conflict,before);
        let mut writer=crate::sys::fork_state::Writer::default();fork_save(&mut writer);let saved=writer.into_bytes();BINDINGS.lock().unwrap().clear();if super::super::in_cache(&std::fs::canonicalize(&derived).unwrap()){let mut unavailable=translated.clone();let before=unavailable.clone();assert!(restore_original_mapping(base,&mut unavailable).is_err());assert_eq!(unavailable,before);}let mut reader=crate::sys::fork_state::Reader::new(&saved);fork_restore(&mut reader);assert!(reader.ok());let mut restored=translated.clone();restore_original_mapping(base,&mut restored).unwrap();assert_eq!(restored,raw);
        // The same real file at a nonzero mapping offset restores that source
        // page, never byte zero of a guessed guest pathname.
        let second=unsafe{libc::mmap(std::ptr::null_mut(),page,libc::PROT_READ,libc::MAP_PRIVATE,file.as_raw_fd(),page as i64)};assert_ne!(second,libc::MAP_FAILED);let mut part=translated[page..].to_vec();restore_original_mapping(second as u64,&mut part).unwrap();assert_eq!(part,raw[page..]);assert_eq!(reload_original_mapping(second as u64,page as u64).unwrap(),raw[page..]);unsafe{libc::munmap(second,page);}
        // Anonymous source copies also retain exact original-file ownership.
        let copy=unsafe{libc::mmap(std::ptr::null_mut(),raw.len(),libc::PROT_READ|libc::PROT_WRITE,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};assert_ne!(copy,libc::MAP_FAILED);assert_eq!(reload_original_mapping(copy as u64,raw.len()as u64).unwrap(),vec![0;raw.len()]);crate::sys::note_copy(copy as u64,raw.len()as u64,&derived,"/original",0);assert!(has_original_mapping(copy as u64,raw.len()as u64));let mut copied=translated.clone();restore_original_mapping(copy as u64,&mut copied).unwrap();assert_eq!(copied,raw);assert_eq!(reload_original_mapping(copy as u64,raw.len()as u64).unwrap(),raw);crate::sys::forget_original_file_test_copy(copy as u64,copy as u64+raw.len()as u64);unsafe{libc::munmap(copy,raw.len());}
        // Source replacement cannot reuse the old mapping's source binding.
        let replacement=root.join("replacement");std::fs::write(&replacement,&raw).unwrap();std::fs::rename(&replacement,&original).unwrap();let mut stale_bytes=translated.clone();let before=stale_bytes.clone();assert!(restore_original_mapping(base,&mut stale_bytes).is_err());assert_eq!(stale_bytes,before);assert!(reload_original_mapping(base,raw.len()as u64).is_err());unsafe{libc::munmap(mapped,raw.len());}
        // A translator-only appended stub region has no original-code view.
        std::fs::write(&original,&raw).unwrap();let mut appended=translated.clone();appended.extend(vec![0x55;page]);std::fs::write(&derived,&appended).unwrap();let expected=FileStat::of_path(&original).unwrap();bind_paths(&original,&expected,&derived).unwrap();let file=File::open(&derived).unwrap();let stub=unsafe{libc::mmap(std::ptr::null_mut(),page,libc::PROT_READ,libc::MAP_PRIVATE,file.as_raw_fd(),(page*2)as i64)};assert_ne!(stub,libc::MAP_FAILED);let mut bytes=vec![0x55;page];let before=bytes.clone();assert!(restore_original_mapping(stub as u64,&mut bytes).is_err());assert_eq!(bytes,before);assert!(reload_original_mapping(stub as u64,page as u64).is_err());unsafe{libc::munmap(stub,page);}
        // Ordinary file-backed discard reads current bytes at the actual file
        // offset and retains the zero-filled final page tail.
        let unrelated=std::env::temp_dir().join(format!("aim-plain-original-{}",std::process::id()));let mut plain=vec![0x77;page];plain.extend_from_slice(&[1,2,3,4,5]);std::fs::write(&unrelated,&plain).unwrap();let file=File::open(&unrelated).unwrap();let mapped=unsafe{libc::mmap(std::ptr::null_mut(),page,libc::PROT_READ,libc::MAP_PRIVATE,file.as_raw_fd(),page as i64)};assert_ne!(mapped,libc::MAP_FAILED);let mut expected=vec![0;page];expected[..5].copy_from_slice(&[1,2,3,4,5]);assert_eq!(reload_original_mapping(mapped as u64,page as u64).unwrap(),expected);unsafe{libc::munmap(mapped,page);}std::fs::remove_file(unrelated).unwrap();
        // A short file occupies a full physical mapping page. Restore known
        // words and retain application edits in its authenticated zero tail.
        let short=root.join("short-original");let published=root.join("short-published");let mut source=vec![0x44;page+24];source[page+4..page+8].copy_from_slice(&0xd4000001u32.to_le_bytes());let mut translated=source.clone();translated[page+4..page+8].copy_from_slice(&0x14001000u32.to_le_bytes());std::fs::write(&short,&source).unwrap();std::fs::write(&published,&translated).unwrap();let expected=FileStat::of_path(&short).unwrap();bind_paths(&short,&expected,&published).unwrap();let file=File::open(&published).unwrap();let mapped=unsafe{libc::mmap(std::ptr::null_mut(),page,libc::PROT_READ,libc::MAP_PRIVATE,file.as_raw_fd(),page as i64)};assert_ne!(mapped,libc::MAP_FAILED);let mut staged=vec![0;page];staged[..24].copy_from_slice(&translated[page..]);staged[128..132].copy_from_slice(&99u32.to_le_bytes());restore_original_mapping(mapped as u64,&mut staged).unwrap();assert_eq!(&staged[..24],&source[page..]);assert_eq!(&staged[128..132],&99u32.to_le_bytes());let mut pristine=vec![0;page];pristine[..24].copy_from_slice(&source[page..]);assert_eq!(reload_original_mapping(mapped as u64,page as u64).unwrap(),pristine);assert_eq!(reload_original_mapping(mapped as u64+128,16).unwrap(),vec![0;16]);unsafe{libc::munmap(mapped,page);}
        // A translator may zero-pad that same final physical page, but any
        // appended nonzero instruction byte has no original architectural view.
        for(zero,name)in[(true,"zero-tail"),(false,"stub-tail")]{let file_path=root.join(name);let mut bytes=translated.clone();bytes.resize(page*2,if zero{0}else{0x55});std::fs::write(&file_path,&bytes).unwrap();bind_paths(&short,&expected,&file_path).unwrap();let file=File::open(&file_path).unwrap();let mapped=unsafe{libc::mmap(std::ptr::null_mut(),page,libc::PROT_READ,libc::MAP_PRIVATE,file.as_raw_fd(),page as i64)};assert_ne!(mapped,libc::MAP_FAILED);let mut staged=bytes[page..].to_vec();let before=staged.clone();if zero{restore_original_mapping(mapped as u64,&mut staged).unwrap();assert_eq!(staged,pristine);assert_eq!(reload_original_mapping(mapped as u64,page as u64).unwrap(),pristine);}else{assert!(restore_original_mapping(mapped as u64,&mut staged).is_err());assert_eq!(staged,before);assert!(reload_original_mapping(mapped as u64,page as u64).is_err());}unsafe{libc::munmap(mapped,page);}}
        BINDINGS.lock().unwrap().clear();std::fs::remove_dir_all(root).unwrap();
    }
}
