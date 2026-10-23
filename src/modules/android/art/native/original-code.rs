// Original words and the exact published replacements belong to one mapping
// generation. Restoring a different generation is an error, never a guess.
use std::{collections::BTreeMap, sync::Mutex};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Word { pub site:u64, pub original:u32, pub published:u32 }
#[derive(Clone, Copy)]
struct Owned { word:Word, generation:u64 }
static WORDS:Mutex<BTreeMap<u64,Owned>>=Mutex::new(BTreeMap::new());

pub(super) fn commit(words:&[Word],generation:u64){
    let mut owner=WORDS.lock().unwrap_or_else(|error|error.into_inner());
    for &word in words { owner.insert(word.site,Owned{word,generation}); }
}
pub(super) fn capture(sites:&[(u64,super::Kind,u32)],published:&[(u64,u32)])->Vec<Word>{
    let originals:BTreeMap<_,_>=sites.iter().map(|&(site,kind,rt)|{
        let word=match kind {
            super::Kind::Svc=>super::a64::SVC0,
            super::Kind::MrsTp=>super::a64::MRS_TPIDR_EL0|rt,
            super::Kind::MsrTp=>super::a64::MSR_TPIDR_EL0|rt,
            super::Kind::ScsPush=>super::a64::SCS_PUSH,
            super::Kind::ScsPop=>super::a64::SCS_POP,
            super::Kind::MrsCtr=>super::a64::MRS_CTR_EL0|rt,
            super::Kind::X18=>rt,
        };(site,word)
    }).collect();
    published.iter().map(|&(site,published)|Word{site,original:originals[&site],published}).collect()
}
pub(super) fn overlaps(start:u64,length:u64)->bool{let end=start.saturating_add(length);WORDS.lock().unwrap_or_else(|error|error.into_inner()).range(start..end).next().is_some()}
pub(super) fn snapshot(start:u64,length:u64)->Vec<Word>{
    let end=start.saturating_add(length);
    WORDS.lock().unwrap_or_else(|error|error.into_inner()).range(start..end)
        .filter(|(site,_)|site.saturating_add(4)<=end).map(|(_,owned)|owned.word).collect()
}
/// The caller copied the currently leased mapping into a private staging view.
/// Check every replacement before changing any byte in that view.
pub(super) fn restore(start:u64,bytes:&mut[u8])->std::io::Result<()> {
    restore_words(start,bytes,&snapshot(start,bytes.len()as u64))
}
fn restore_words(start:u64,bytes:&mut[u8],words:&[Word])->std::io::Result<()> {
    for word in words {
        let at=usize::try_from(word.site.checked_sub(start).ok_or_else(||std::io::Error::from_raw_os_error(libc::ENOEXEC))?).map_err(|_|std::io::Error::from_raw_os_error(libc::ENOEXEC))?;
        let current=bytes.get(at..at+4).ok_or_else(||std::io::Error::from_raw_os_error(libc::ENOEXEC))?;
        if current!=word.published.to_le_bytes(){return Err(std::io::Error::from_raw_os_error(libc::ENOEXEC));}
    }
    for word in words {let at=(word.site-start)as usize;bytes[at..at+4].copy_from_slice(&word.original.to_le_bytes());}
    Ok(())
}
pub(super) fn forget(start:u64,length:u64){replace(start,length,u64::MAX);}
pub(super) fn replace(start:u64,length:u64,generation:u64){
    WORDS.lock().unwrap_or_else(|error|error.into_inner()).retain(|site,owned|*site<start||*site>=start.saturating_add(length)||owned.generation>generation);
}
pub(super) fn reset(){WORDS.lock().unwrap_or_else(|error|error.into_inner()).clear();}
pub(super) fn save(writer:&mut crate::sys::fork_state::Writer){
    let owner=WORDS.lock().unwrap_or_else(|error|error.into_inner());
    writer.seq(owner.values(),|writer,owned|{writer.u64(owned.word.site);writer.u64(owned.word.original as u64);writer.u64(owned.word.published as u64);});
}
pub(super) fn load(reader:&mut crate::sys::fork_state::Reader,generation:u64){
    let words=reader.seq(|reader|Word{site:reader.u64(),original:reader.u64()as u32,published:reader.u64()as u32});
    reset();commit(&words,generation);
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_generation_restoration_is_all_or_nothing(){
        let words=[Word{site:0x1000,original:0xd53bd040,published:0x14000040},Word{site:0x1004,original:0x8b120000,published:0xd4342000}];
        let mut bytes=words.iter().flat_map(|word|word.published.to_le_bytes()).collect::<Vec<_>>();
        let mut stale=bytes.clone();stale[7]^=1;let before=stale.clone();
        assert!(restore_words(0x1000,&mut stale,&words).is_err());assert_eq!(stale,before);
        restore_words(0x1000,&mut bytes,&words).unwrap();
        assert_eq!(bytes,words.iter().flat_map(|word|word.original.to_le_bytes()).collect::<Vec<_>>());
    }
    #[test]
    fn replacement_retains_only_newer_commits(){
        let site=0x77770000;let old=Word{site,original:1,published:2};let new=Word{site,original:3,published:4};
        commit(&[old],40);commit(&[new],41);replace(site,4,40);assert_eq!(snapshot(site,4),vec![new]);
        forget(site,4);assert!(snapshot(site,4).is_empty());
    }
}
