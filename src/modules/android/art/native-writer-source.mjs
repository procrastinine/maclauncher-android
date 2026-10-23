import fs from 'node:fs';import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';
/** Add the separate native-writer continuation to an explicitly prepared AIM tree. */
export function prepareNativeWriter(aim){
 const source=path.join(aim,'crates/aim-linux-abi/src/sys');
 copyChanged(path.join(import.meta.dirname,'native/native-writer.rs'),path.join(source,'native-writer.rs'));
 const edit=(name,from,to)=>{const file=path.join(source,name);let text=fs.readFileSync(file,'utf8');if(text.split(from).length!==2)throw Error('Original native writer boundary changed: '+name);writeChanged(file,text.replace(from,to));};
 edit('mod.rs','mod jit;','mod jit;\n#[path="native-writer.rs"]\npub(crate) mod native_writer;');
 edit('signal.rs','    let ctx = context::current_ctx();\n    if hsig == CARRIER {','    let ctx = context::current_ctx();\n    if unsafe{super::native_writer::fault(ctx,hsig,si,uc)}{return None;}\n    if hsig == CARRIER {');
 // Initial execution is independently optional. When present, its sparse
 // publication bypasses prepare() and must initialize the same owner first.
 if(fs.readFileSync(path.join(source,'mod.rs'),'utf8').includes('pub(crate) mod initial_execution;')){
  const initial='pub(crate) fn prepare_initial(start:u64,bytes:&[u8],protections:&[i32],sites:&[u64])->std::io::Result<InitialPrepared>{';
  edit('initial-memory.rs',initial,initial+'\n    super::native_writer::initialize().map_err(std::io::Error::other)?;');
 }
 edit('mutable-memory.rs','    let length=bytes.len()as u64;','    super::native_writer::initialize().map_err(std::io::Error::other)?;\n    let length=bytes.len()as u64;');
 edit('mutable-memory.rs','    let records=reader.seq(|reader|(reader.u64(),reader.u64(),reader.i32()));let mut owner=','    let records=reader.seq(|reader|(reader.u64(),reader.u64(),reader.i32()));if !records.is_empty(){super::native_writer::initialize().expect("Native writer memory owner");}let mut owner=');
}
