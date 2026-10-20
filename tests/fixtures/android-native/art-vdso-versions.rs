// Metadata-only entry points keep the upstream ELF producer independent of
// host commpage clocks and translated syscall execution in this unit check.
const PAGE: u64 = 16384;
const TEXT_OFF: usize = 0x1000;
mod elf {
    pub const EM_AARCH64: u16 = 183;
    pub const PF_R: u32 = 4;
    pub const PF_X: u32 = 1;
}
mod a64 {
    pub fn host_ctr_el0() -> u64 { 0 }
    pub fn classify(_: u32) -> Option<((), u32)> { None }
    pub fn stub_for(_: (), _: u32, _: u64, _: u64, _: u64) -> Option<Vec<u32>> { None }
    pub fn encode_b(_: u64, _: u64) -> Option<u32> { None }
}
#[cfg(target_vendor="apple")]
std::arch::global_asm!(
    ".text", ".p2align 2", ".globl _aim_vdso_start", "_aim_vdso_start:",
    ".globl _aim_vdso_clock_gettime", ".alt_entry _aim_vdso_clock_gettime", "_aim_vdso_clock_gettime:", "ret",
    ".globl _aim_vdso_gettimeofday", ".alt_entry _aim_vdso_gettimeofday", "_aim_vdso_gettimeofday:", "ret",
    ".globl _aim_vdso_clock_getres", ".alt_entry _aim_vdso_clock_getres", "_aim_vdso_clock_getres:", "ret",
    ".globl _aim_vdso_rt_sigreturn", ".alt_entry _aim_vdso_rt_sigreturn", "_aim_vdso_rt_sigreturn:", "ret",
    ".globl _aim_vdso_end", ".alt_entry _aim_vdso_end", "_aim_vdso_end:", "udf #0",
);
#[cfg(not(target_vendor="apple"))]
std::arch::global_asm!(
    ".text", ".balign 4", ".global aim_vdso_start", "aim_vdso_start:",
    ".global aim_vdso_clock_gettime", "aim_vdso_clock_gettime:", ".long 0xd65f03c0",
    ".global aim_vdso_gettimeofday", "aim_vdso_gettimeofday:", ".long 0xd65f03c0",
    ".global aim_vdso_clock_getres", "aim_vdso_clock_getres:", ".long 0xd65f03c0",
    ".global aim_vdso_rt_sigreturn", "aim_vdso_rt_sigreturn:", ".long 0xd65f03c0",
    ".global aim_vdso_end", "aim_vdso_end:", ".long 0",
);
unsafe extern "C" {
    fn aim_vdso_start();
    fn aim_vdso_clock_gettime();
    fn aim_vdso_gettimeofday();
    fn aim_vdso_clock_getres();
    fn aim_vdso_rt_sigreturn();
    fn aim_vdso_end();
}
include!(env!("VDSO_BUILD_SOURCE"));

#[test]
fn linux_arm64_versions_resolve_from_the_complete_elf() {
    use std::collections::BTreeMap;
    let image=build();
    let u16_at=|o:usize|u16::from_le_bytes(image[o..o+2].try_into().unwrap());
    let u32_at=|o:usize|u32::from_le_bytes(image[o..o+4].try_into().unwrap());
    let u64_at=|o:usize|u64::from_le_bytes(image[o..o+8].try_into().unwrap());
    let string=|o:usize|std::str::from_utf8(image[o..].split(|&b|b==0).next().unwrap()).unwrap();
    assert_eq!(u16_at(18),183);
    let phoff=u64_at(32)as usize;
    let dynamic=(0..u16_at(56)as usize).map(|i|phoff+i*56).find(|&p|u32_at(p)==2).unwrap();
    let mut tags=BTreeMap::new();
    let mut at=u64_at(dynamic+8)as usize;
    while u64_at(at)!=0 {tags.insert(u64_at(at),u64_at(at+8)as usize);at+=16;}
    let strtab=tags[&5];
    assert_eq!(string(strtab+tags[&14]),"linux-vdso.so.1");
    assert_eq!(tags[&0x6ffffffd],2);
    // The same Verdef/Verdaux chain consumed by native ELF version lookup.
    let mut def=tags[&0x6ffffffc];
    assert_eq!(u16_at(def),1);
    assert_eq!(u16_at(def+2),1);
    assert_eq!(u16_at(def+4),1);
    assert_eq!(string(strtab+u32_at(def+u32_at(def+12)as usize)as usize),"linux-vdso.so.1");
    def+=u32_at(def+16)as usize;
    assert_eq!(u16_at(def),1);
    assert_eq!(u16_at(def+2),0);
    assert_eq!(u16_at(def+4),2);
    assert_eq!(u16_at(def+6),1);
    assert_eq!(u32_at(def+8),0x075fcb89);
    assert_eq!(string(strtab+u32_at(def+u32_at(def+12)as usize)as usize),"LINUX_2.6.39");
    assert_eq!(u32_at(def+16),0);
    let versym=tags[&0x6ffffff0];
    assert_eq!(u16_at(versym),0);
    let symtab=tags[&6];
    for (i,want) in ["__kernel_clock_gettime","__kernel_gettimeofday","__kernel_clock_getres","__kernel_rt_sigreturn"].iter().enumerate() {
        let sym=symtab+(i+1)*24;
        assert_eq!(string(strtab+u32_at(sym)as usize),*want);
        assert_eq!(u16_at(sym+6),5,"preserve the .text section index");
        assert_eq!(u16_at(versym+(i+1)*2),2);
        assert!(u64_at(sym+8)>=TEXT_OFF as u64);
    }
    // Section-based Bionic lookup and dynamic-table lookup share the tables.
    let shoff=u64_at(40)as usize;
    let sections=(0..u16_at(60)as usize).map(|i|shoff+i*64).collect::<Vec<_>>();
    for &section in &sections {
        let alignment=u64_at(section+48);
        if alignment>1 {
            assert_eq!(u64_at(section+16)%alignment,0,"section address alignment");
            assert_eq!(u64_at(section+24)%alignment,0,"section file alignment");
        }
    }
    let version=*sections.iter().find(|&&s|u32_at(s+4)==0x6fffffff).unwrap();
    assert_eq!(u64_at(version+24)as usize,versym);
    assert_eq!(u32_at(version+40),2);
    let definitions=*sections.iter().find(|&&s|u32_at(s+4)==0x6ffffffd).unwrap();
    assert_eq!(u64_at(definitions+24)as usize,tags[&0x6ffffffc]);
    assert_eq!(u32_at(definitions+40),3);
    assert_eq!(u32_at(definitions+44),2);
    assert_eq!(u16_at(62),6,"preserve the section-name table index");
    println!("PASS complete ARM64 VDSO ELF: version definitions, symbol versions, native lookup and original section indices");
}
