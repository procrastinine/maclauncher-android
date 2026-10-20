import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** ARM64 Linux exports its VDSO entry points in LINUX_2.6.39. Consumers
 * resolve both the symbol and its ELF version through AT_SYSINFO_EHDR. */
export function prepareVdsoVersions(aim){
 const file=path.join(aim,'crates/aim-linux-abi/src/vdso.rs');
 let source=fs.readFileSync(file,'utf8');
 const edit=(from,to)=>{
  if(source.split(from).length!==2)throw Error('AIM VDSO version boundary changed: '+from.slice(0,60));
  source=source.replace(from,to);
 };
 edit('const DT_SONAME: u64 = 14;',`const DT_SONAME: u64 = 14;
const DT_VERSYM: u64 = 0x6ffffff0;
const DT_VERDEF: u64 = 0x6ffffffc;
const DT_VERDEFNUM: u64 = 0x6ffffffd;
const SHT_GNU_VERSYM: u32 = 0x6fffffff;
const SHT_GNU_VERDEF: u32 = 0x6ffffffd;`);
 edit('    let soname = name(SONAME);','    let soname = name(SONAME);\n    let linux_version = name("LINUX_2.6.39");');
 edit('    let dynamic = words(&[',`    // Null symbol is local; every exported function has version index 2.
    let mut versym = vec![0u16];
    versym.resize(nsyms, 2);
    let versym: Vec<u8> = versym.iter().flat_map(|v| v.to_le_bytes()).collect();
    let versym_off = put(&mut b, &versym);
    b.resize(b.len().next_multiple_of(8), 0);
    let verdef_off = b.len();
    let elf_hash = |name: &str| -> u32 {
        let mut hash = 0u32;
        for byte in name.bytes() {
            hash = (hash << 4).wrapping_add(byte as u32);
            let high = hash & 0xf0000000;
            hash = (hash ^ (high >> 24)) & !high;
        }
        hash
    };
    // Elf64_Verdef plus one Elf64_Verdaux, with relative offsets.
    for (flags, index, hash, name, next) in [
        (1u16, 1u16, elf_hash(SONAME), soname, 28u32),
        (0, 2, elf_hash("LINUX_2.6.39"), linux_version, 0),
    ] {
        for half in [1u16, flags, index, 1] { b.extend(half.to_le_bytes()); }
        for word in [hash, 20, next, name, 0] { b.extend(word.to_le_bytes()); }
    }
    let verdef_len = b.len() - verdef_off;
    b.resize(b.len().next_multiple_of(8), 0);
    let dynamic = words(&[`);
 edit('        DT_SONAME,\n        soname as u64,',`        DT_SONAME,
        soname as u64,
        DT_VERSYM,
        versym_off as u64,
        DT_VERDEF,
        verdef_off as u64,
        DT_VERDEFNUM,
        2,`);
 edit('    let shstrtab = b"\\0.hash\\0.dynsym\\0.dynstr\\0.dynamic\\0.text\\0.shstrtab\\0";',
      '    let shstrtab = b"\\0.hash\\0.dynsym\\0.dynstr\\0.dynamic\\0.text\\0.shstrtab\\0.gnu.version\\0.gnu.version_d\\0";');
 edit('let sections: [(u32, u32, usize, usize, u32, u32, u64); 7]',
      'let sections: [(u32, u32, usize, usize, u32, u32, u64); 9]');
 edit('        (38, SHT_STRTAB, shstrtab_off, shstrtab.len(), 0, 0, 0),',`        (38, SHT_STRTAB, shstrtab_off, shstrtab.len(), 0, 0, 0),
        (48, SHT_GNU_VERSYM, versym_off, versym.len(), 2, 2, 2),
        (61, SHT_GNU_VERDEF, verdef_off, verdef_len, 3, 2, 0),`);
 edit('if ty == SHT_DYNSYM { 1u32 } else { 0 }',
      'if ty == SHT_DYNSYM { 1u32 } else if ty == SHT_GNU_VERDEF { 2 } else { 0 }');
 writeChanged(file,source);
}
