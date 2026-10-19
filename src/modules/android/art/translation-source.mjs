import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** Derive both original code sites and output bytes from one original ELF
 * analysis. Authentication, cache comparison and mapping ownership stay with
 * AIM; repeated instruction discovery does not strengthen those checks. */
export function prepareTranslationAnalysis(aim){
 const replace=(relative,before,after)=>{
  const file=path.join(aim,relative),source=fs.readFileSync(file,'utf8');
  if(source.split(before).length!==2)throw Error('Original ELF translation analysis boundary changed');
  writeChanged(file,source.replace(before,after));
 };
 replace('crates/aim-linux-abi/src/xlate/mod.rs',
`    let a = analyze(&elf);
    let mut report = Report {`,
`    let a = analyze(&elf);
    translate_analyzed(&elf, &a, opts)
}

pub(crate) fn translate_analyzed(elf: &Elf, a: &Analysis, opts: &Options) -> Result<Translation, String> {
    let bytes = elf.bytes;
    let mut report = Report {`);
 replace('crates/aim-linux-abi/src/xrt_verified.rs',
`            let analysis = xlate::analyze(&elf);
            let sites = Arc::new(super::Sites {`,
`            let analysis = xlate::analyze(&elf);
            let translated = xlate::translate_analyzed(&elf, &analysis, &xlate::Options { ctr_el0: ctr })
                .map_err(|_| errno::ENOEXEC)?;
            let sites = Arc::new(super::Sites {`);
 replace('crates/aim-linux-abi/src/xrt_verified.rs',
`            let translated = xlate::translate(&bytes, &xlate::Options { ctr_el0: ctr })
                .map_err(|_| errno::ENOEXEC)?;
            let (output, identity) = match translated.outcome {`,
`            let (output, identity) = match translated.outcome {`);
}
