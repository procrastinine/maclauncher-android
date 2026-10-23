import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Bind original words to the exact mapping publication that installed them. */
export function prepareOriginalCode(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{const file=path.join(root,name);let text=fs.readFileSync(file,'utf8');for(const[from,to,count=1]of changes){if(text.split(from).length!==count+1)throw Error('Original code owner boundary changed: '+name+' '+from.slice(0,70));text=text.split(from).join(to);}writeChanged(file,text);};
 copyChanged(path.join(import.meta.dirname,'native/original-code.rs'),path.join(root,'original-code.rs'));
 edit('patch.rs',[
  ['mod far { use super::*; include!("far-veneer.rs"); }','mod far { use super::*; include!("far-veneer.rs"); }\nmod original { include!("original-code.rs"); }'],
  ['pub stats:PatchStats,far:far::Prepared}','pub stats:PatchStats,far:far::Prepared,original:Vec<original::Word>}'],
  ['pub fn commit(&mut self){self.far.commit();}','pub fn commit(&mut self){self.far.commit();original::commit(&self.original,far::epoch());}'],
  ['pub fn relocate_far(old:u64,new:u64,length:u64)->io::Result<PreparedFarRelocation>{Ok(PreparedFarRelocation(far::relocation(old,new,length)?))}', 'pub fn relocate_far(old:u64,new:u64,length:u64)->io::Result<PreparedFarRelocation>{let words=original::snapshot(old,length).into_iter().map(|word|original::Word{site:new+word.site-old,..word}).collect();Ok(PreparedFarRelocation(far::relocation(old,new,length)?,words))}'],
  ['pub struct PreparedFarRelocation(far::Prepared);','pub struct PreparedFarRelocation(far::Prepared,Vec<original::Word>);'],
  ['pub fn commit(&mut self){self.0.commit();}','pub fn commit(&mut self){self.0.commit();original::commit(&self.1,far::epoch());}'],
  ['pub fn forget_far(start:u64,length:u64){far::forget(start,length);}','pub fn forget_far(start:u64,length:u64){far::forget(start,length);original::forget(start,length);}'],
  ['pub fn replace_far(start:u64,length:u64,epoch:u64){far::replace(start,length,epoch);}','pub fn replace_far(start:u64,length:u64,epoch:u64){far::replace(start,length,epoch);original::replace(start,length,epoch);}'],
  ['far:far::Prepared::empty()});}','far:far::Prepared::empty(),original:Vec::new()});}',2],
  ['Ok(PreparedRewrite{words:site_words,stats,far:pending_far})','let original=original::capture(sites,&site_words);\n    Ok(PreparedRewrite{words:site_words,stats,far:pending_far,original})'],
  ['    far::reset();','    far::reset();original::reset();'],
  ['    far::save(w);','    far::save(w);original::save(w);'],
  ['    far::restore(r,&islands);','    far::restore(r,&islands);original::load(r,far::epoch());'],
  ['pub fn far_history_sites(start:u64,length:u64)->Vec<u64>{far::history_sites(start,length)}',`pub fn far_history_sites(start:u64,length:u64)->Vec<u64>{far::history_sites(start,length)}
pub fn restore_original_copy(start:u64,bytes:&mut[u8])->io::Result<()>{original::restore(start,bytes)}
pub fn note_original_words(words:&[(u64,u32,u32)]){let words:Vec<_>=words.iter().map(|&(site,original,published)|original::Word{site,original,published}).collect();original::commit(&words,far::epoch());}`],
 ]);
}
