// Unreachable instruction veneers share the original process-lived islands.
// Signal lookup copies immutable metadata; it never locks or allocates.
use std::{collections::{BTreeMap,BTreeSet},sync::{Arc,Mutex,atomic::{AtomicPtr,AtomicUsize,AtomicU64,Ordering}}};

const EXIT:u16=0xa0df;
#[derive(Clone)]
struct Recipe {site:u64,original:u32,token:u16,body:u64,branches:Vec<(u64,u64)>}
#[derive(Clone,Copy)]
struct Trap {pc:u64,token:u16,target:u64}
struct Snapshot {traps:Box<[Trap]>}
#[derive(Default)]
struct State {history:Vec<Arc<Recipe>>,active:BTreeMap<u64,(u64,Arc<Recipe>)>,used:BTreeMap<u64,BTreeSet<u16>>}
static STATE:Mutex<State>=Mutex::new(State{history:Vec::new(),active:BTreeMap::new(),used:BTreeMap::new()});
static SNAPSHOT:AtomicPtr<Snapshot>=AtomicPtr::new(std::ptr::null_mut());
static READERS:AtomicUsize=AtomicUsize::new(0);
static GENERATION:AtomicU64=AtomicU64::new(0);
unsafe extern "C"{fn mach_vm_read_overwrite(task:u32,address:u64,length:u64,output:u64,copied:*mut u64)->i32;}
fn current_word(site:u64)->Option<u32>{let mut word=0u32;let mut copied=0;let result=unsafe{mach_vm_read_overwrite(vm::task(),site,4,&mut word as*mut _ as u64,&mut copied)};(result==0&&copied==4).then_some(word)}

fn publish(state:&State){
    let mut traps=Vec::new();
    for recipe in &state.history {
        traps.push(Trap{pc:recipe.site,token:recipe.token,target:recipe.body});
        traps.extend(recipe.branches.iter().map(|&(pc,target)|Trap{pc,token:EXIT,target}));
    }
    traps.sort_unstable_by_key(|trap|(trap.pc,trap.token));
    let next=Box::into_raw(Box::new(Snapshot{traps:traps.into_boxed_slice()}));
    let old=SNAPSHOT.swap(next,Ordering::SeqCst);
    // A reader announces itself before fetching the pointer. Bodies remain
    // owned by ISLANDS after this descriptor grace period has completed.
    while READERS.load(Ordering::SeqCst)!=0 {std::thread::yield_now();}
    if !old.is_null(){unsafe{drop(Box::from_raw(old));}}
}
fn lookup(pc:u64,token:u16)->Option<u64>{
    READERS.fetch_add(1,Ordering::SeqCst);
    let snapshot=SNAPSHOT.load(Ordering::SeqCst);
    let result=if snapshot.is_null(){None}else{unsafe{&*snapshot}.traps.binary_search_by_key(&(pc,token),|trap|(trap.pc,trap.token)).ok().map(|index|unsafe{&*snapshot}.traps[index].target)};
    READERS.fetch_sub(1,Ordering::SeqCst);result
}
fn token(site:u64)->io::Result<u16>{
    let original=current_word(site).filter(|word|word&0xffe0001f==0xd4200000).map(|word|((word>>5)&0xffff)as u16);
    let mut state=STATE.lock().unwrap_or_else(|error|error.into_inner());
    let used=state.used.entry(site).or_default();
    // Tokens are per PC, not a global site limit. Never recycle an instruction
    // identity while an old fetched BRK can still be delivered by Darwin.
    // Darwin reserves 0xb000..0xbfff for unrecoverable user traps. Stay in
    // the ordinary range beside AIM's existing 0xa0xx instruction traps.
    let token=(0xa100u16..=0xafff).find(|token|Some(*token)!=original&&!used.contains(token))
        .ok_or_else(||io::Error::from_raw_os_error(libc::ENOMEM))?;
    used.insert(token);Ok(token)
}
pub(super) struct Prepared {recipes:Vec<Arc<Recipe>>,published:bool,committed:bool}
impl Prepared {
    pub(super) fn empty()->Self{Self{recipes:Vec::new(),published:false,committed:false}}
    pub(super) fn add(&mut self,islands:&mut Vec<Island>,site:u64,original:u32,ctr:u32)->io::Result<u32>{
        let token=token(site)?;
        self.compile(islands,site,original,ctr,token)
    }
    fn compile(&mut self,islands:&mut Vec<Island>,site:u64,original:u32,ctr:u32,token:u16)->io::Result<u32>{
        let result=(||{
            let bytes=(a64::x18_far_capacity()*4)as u64;
            let mut selected=None;
            for(index,island)in islands.iter().enumerate(){
                if island.used+bytes<=island.size&&let Some(emitted)=a64::x18_emit_far(original,site,island.rx+island.used,ctr,token){selected=Some((index,emitted));break;}
            }
            let(index,(words,branches))=if let Some(selected)=selected{selected}else{
                let island=new_island(site).ok_or_else(||io::Error::from_raw_os_error(libc::ENOMEM))?;
                let emitted=a64::x18_emit_far(original,site,island.rx,ctr,token).ok_or_else(||io::Error::from_raw_os_error(libc::ENOEXEC));
                if emitted.is_err(){unsafe{libc::munmap(island.rx as*mut _,island.size as usize);libc::munmap(island.rw as*mut _,island.size as usize);}}
                let emitted=emitted?;islands.push(island);(islands.len()-1,emitted)
            };
            let island=&mut islands[index];let body=island.rx+island.used;
            if words.len()*4>bytes as usize{return Err(io::Error::from_raw_os_error(libc::ENOEXEC));}
            for(index,word)in words.iter().enumerate(){unsafe{((island.rw+island.used)as*mut u32).add(index).write(*word);}}
            unsafe{sys_icache_invalidate(body as*mut _,words.len()*4);}
            island.used+=bytes;
            self.recipes.push(Arc::new(Recipe{site,original,token,body,branches}));
            Ok(0xd4200000|((token as u32)<<5))
        })();
        if result.is_err(){let mut state=STATE.lock().unwrap_or_else(|e|e.into_inner());if let Some(used)=state.used.get_mut(&site){used.remove(&token);}}
        result
    }
    pub(super) fn publish(&mut self){
        if self.published{return;}
        let mut state=STATE.lock().unwrap_or_else(|error|error.into_inner());
        for recipe in &self.recipes{if !state.history.iter().any(|old|old.site==recipe.site&&old.token==recipe.token){state.history.push(recipe.clone());}}
        if !self.recipes.is_empty(){publish(&state);}
        self.published=true;
    }
    pub(super) fn commit(&mut self){
        if self.committed{return;}self.publish();
        let mut state=STATE.lock().unwrap_or_else(|error|error.into_inner());
        let generation=GENERATION.fetch_add(1,Ordering::AcqRel)+1;
        for recipe in &self.recipes{state.active.insert(recipe.site,(generation,recipe.clone()));}
        self.committed=true;
    }
}
impl Drop for Prepared {fn drop(&mut self){if !self.published&&!self.recipes.is_empty(){let mut state=STATE.lock().unwrap_or_else(|e|e.into_inner());for recipe in &self.recipes{if !state.history.iter().any(|old|old.site==recipe.site&&old.token==recipe.token){if let Some(used)=state.used.get_mut(&recipe.site){used.remove(&recipe.token);}}}}}}
pub(super) fn valid(site:u64,original:u32,word:u32)->bool{
    let state=STATE.lock().unwrap_or_else(|error|error.into_inner());
    state.history.iter().any(|recipe|recipe.site==site&&recipe.original==original&&word==(0xd4200000|((recipe.token as u32)<<5)))
}
pub(super) fn check_original(base:u64,bytes:&[u8])->io::Result<()>{
    check_bytes(base,bytes,false)
}
pub(super) fn check_bytes(base:u64,bytes:&[u8],existing:bool)->io::Result<()>{
    let state=STATE.lock().unwrap_or_else(|error|error.into_inner());
    for recipe in &state.history{
        let Some(offset)=recipe.site.checked_sub(base).and_then(|offset|usize::try_from(offset).ok())else{continue;};
        let Some(word)=bytes.get(offset..offset.saturating_add(4))else{continue;};
        if u32::from_le_bytes(word.try_into().unwrap())==0xd4200000|((recipe.token as u32)<<5){
            if existing&&state.active.get(&recipe.site).is_some_and(|(_,active)|active.token==recipe.token){continue;}
            crate::diag!("[linux-abi] executable publication rejected: original BRK identity conflicts at {:#x}, immediate {:#x}",recipe.site,recipe.token);
            return Err(io::Error::from_raw_os_error(libc::ENOEXEC));
        }
    }
    Ok(())
}
pub(super) fn history_sites(start:u64,length:u64)->Vec<u64>{
    let state=STATE.lock().unwrap_or_else(|error|error.into_inner());
    state.history.iter().filter(|recipe|recipe.site>=start&&recipe.site.saturating_add(4)<=start.saturating_add(length)).map(|recipe|recipe.site).collect::<BTreeSet<_>>().into_iter().collect()
}
pub(super) fn writable_words(start:u64,length:u64)->Vec<(u64,u32)>{
    let state=STATE.lock().unwrap_or_else(|error|error.into_inner());
    state.active.range(start..start.saturating_add(length)).map(|(&site,(_,recipe))|(site,recipe.original)).collect()
}
pub(super) fn forget(start:u64,length:u64){
    let mut state=STATE.lock().unwrap_or_else(|error|error.into_inner());
    state.active.retain(|site,_|*site<start||*site>=start.saturating_add(length));
}
pub(super) fn reconcile(start:u64,length:u64){
    let mut state=STATE.lock().unwrap_or_else(|error|error.into_inner());
    state.active.retain(|site,(_,recipe)|*site<start||*site>=start.saturating_add(length)||
        current_word(*site)==Some(0xd4200000|((recipe.token as u32)<<5)));
}
pub(super) fn epoch()->u64{GENERATION.load(Ordering::Acquire)}
pub(super) fn replace(start:u64,length:u64,epoch:u64){
    let mut state=STATE.lock().unwrap_or_else(|error|error.into_inner());
    state.active.retain(|site,(generation,_)|*site<start||*site>=start.saturating_add(length)||*generation>epoch);
}
pub(super) fn relocation(old:u64,new:u64,length:u64)->io::Result<Prepared>{
    {
        let state=STATE.lock().unwrap_or_else(|error|error.into_inner());
        for target in &state.history{
            if target.site<new||target.site.saturating_add(4)>new.saturating_add(length){continue;}
            let source=old+(target.site-new);
            let entry=0xd4200000|((target.token as u32)<<5);
            if current_word(source)!=Some(entry)||vm::region(source).is_none_or(|(_,_,protection,_)|protection&libc::PROT_EXEC==0){continue;}
            if state.active.get(&source).is_some_and(|(_,recipe)|recipe.token==target.token){continue;}
            crate::diag!("[linux-abi] executable move rejected: original BRK identity conflicts at {:#x}, immediate {:#x}",target.site,target.token);
            return Err(io::Error::from_raw_os_error(libc::ENOEXEC));
        }
    }
    let recipes:Vec<_>={let state=STATE.lock().unwrap_or_else(|error|error.into_inner());state.active.range(old..old.saturating_add(length)).map(|(_,(_,recipe))|recipe.clone()).collect()};
    let mut prepared=Prepared::empty();let mut islands=ISLANDS.lock().unwrap_or_else(|error|error.into_inner());
    for recipe in recipes{
        let site=new.checked_add(recipe.site-old).ok_or_else(||io::Error::from_raw_os_error(libc::EINVAL))?;
        let existing={let mut state=STATE.lock().unwrap_or_else(|error|error.into_inner());
            if let Some(existing)=state.history.iter().find(|item|item.site==site&&item.token==recipe.token){
                if existing.original!=recipe.original{return Err(io::Error::from_raw_os_error(libc::ENOMEM));}Some(existing.clone())
            }else{
                if current_word(site)==Some(0xd4200000|((recipe.token as u32)<<5)){return Err(io::Error::from_raw_os_error(libc::ENOEXEC));}
                let used=state.used.entry(site).or_default();if !used.insert(recipe.token){return Err(io::Error::from_raw_os_error(libc::ENOMEM));}None
            }};
        if let Some(existing)=existing{prepared.recipes.push(existing);}else{prepared.compile(&mut islands,site,recipe.original,a64::host_ctr_el0(),recipe.token)?;}
    }
    Ok(prepared)
}
pub(super) fn save(writer:&mut crate::sys::fork_state::Writer){
    let state=STATE.lock().unwrap_or_else(|error|error.into_inner());
    writer.seq(state.history.iter(),|writer,recipe|{writer.u64(recipe.site);writer.u64(recipe.original as u64);writer.u64(recipe.token as u64);writer.u64(recipe.body);
        writer.u64(u64::from(state.active.get(&recipe.site).is_some_and(|(_,active)|active.token==recipe.token)));
        writer.seq(recipe.branches.iter(),|writer,&(pc,target)|{writer.u64(pc);writer.u64(target);});});
}
pub(super) fn restore(reader:&mut crate::sys::fork_state::Reader,islands:&[Island]){
    let records=reader.seq(|reader|{let site=reader.u64();let original=reader.u64()as u32;let token=reader.u64()as u16;let body=reader.u64();let active=reader.u64()!=0;let branches=reader.seq(|reader|(reader.u64(),reader.u64()));(Recipe{site,original,token,body,branches},active)});
    let mut state=STATE.lock().unwrap_or_else(|error|error.into_inner());state.history.clear();state.active.clear();state.used.clear();
    for(recipe,active)in records{
        let(words,branches)=a64::x18_emit_far(recipe.original,recipe.site,recipe.body,a64::host_ctr_el0(),recipe.token).expect("original far veneer recipe");
        assert_eq!(branches,recipe.branches,"original far continuation recipe");
        assert!(islands.iter().any(|island|recipe.body>=island.rx&&recipe.body.checked_add((words.len()*4)as u64).is_some_and(|end|end<=island.rx+island.used)),"far body island owner");
        for(index,&word)in words.iter().enumerate(){assert_eq!(current_word(recipe.body+(index*4)as u64),Some(word),"far body original bytes");}
        assert!(state.used.entry(recipe.site).or_default().insert(recipe.token),"unique far instruction identity");
        let recipe=Arc::new(recipe);if active{assert_eq!(current_word(recipe.site),Some(0xd4200000|((recipe.token as u32)<<5)),"active far entry bytes");let generation=GENERATION.fetch_add(1,Ordering::AcqRel)+1;state.active.insert(recipe.site,(generation,recipe.clone()));}state.history.push(recipe);
    }
    publish(&state);
}
pub(super) unsafe fn handle(context:*mut libc::ucontext_t)->bool{unsafe{
    if context.is_null()||(*context).uc_mcontext.is_null(){return false;}
    let machine=&mut *((*context).uc_mcontext as*mut MContext64);
    let esr=machine.es[1]as u32;
    if esr>>26!=0x3c{return false;}
    // ESR retains the actually fetched immediate even if another thread has
    // replaced the public mapping before this signal is delivered.
    let Some(target)=lookup(machine.ss.pc,esr as u16)else{return false;};
    machine.ss.pc=target;true
}}
pub(super) unsafe fn resume_stack(context:*mut libc::c_void)->bool{unsafe{
    if context.is_null(){return false;}
    let uc=context as*mut libc::ucontext_t;if (*uc).uc_mcontext.is_null(){return false;}
    let pc=(*((*uc).uc_mcontext as*mut MContext64)).ss.pc;
    let Some(target)=lookup(pc+4,EXIT)else{return false;};
    a64::x18_resume_stack_far(context,target)
}}
pub(super) fn reset(){
    let mut state=STATE.lock().unwrap_or_else(|error|error.into_inner());
    state.active.clear();state.history.clear();state.used.clear();publish(&state);
}
