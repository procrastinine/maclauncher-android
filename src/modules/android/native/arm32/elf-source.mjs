import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from '../binder-rpc/build.mjs';
import {writeChanged} from '../upstream/source-files.mjs';
import {prepareArm32JumpSources} from '../nonlocal-jump/build.mjs';
import {prepareSearchSources} from '../native-search/build.mjs';
import {prepareArm32Synchronization} from './synchronization.mjs';
import {prepareArm32LegacyCharacterData} from '../native-ctype/build.mjs';

// The CPU format adapter consumes Bionic's original packed relocation decoder.
// Only its surrounding linker types and failure reporting differ on the host.
export function prepareElf32({bionicSource,output}) {
  const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));
  const source=path.join(output,'bionic');
  const files=['linker/linker_reloc_iterators.h','linker/linker_sleb128.h','linker/linker_gnu_hash.h','linker/linker_soinfo.cpp','libc/NOTICE','libc/private/bionic_ssp.h','libc/stdio/stdio.cpp','libc/tzcode/localtime.c','libc/arch-arm/bionic/__aeabi.c','libc/arch-arm/bionic/exidx_dynamic.c','libc/bionic/new.cpp','libstdc++/include/new','libstdc++/NOTICE'];
  exportSources(bionicSource,pins.aospBionic,files,source);
  let leb=fs.readFileSync(path.join(source,files[1]),'utf8')
    .replace('#include <async_safe/log.h>','').replace('#include "linker_debug.h"','');
  let reloc=fs.readFileSync(path.join(source,files[0]),'utf8')
    .replace('#include "linker.h"','').replace('#include "linker_sleb128.h"','')
    .replace('.r_offset = decoder.pop_front(),','.r_offset = static_cast<uint32_t>(decoder.pop_front()),');
  const gnu=fs.readFileSync(path.join(source,'linker/linker_gnu_hash.h'),'utf8').match(/^static std::pair<uint32_t, uint32_t> calculate_gnu_hash_simple\(const char\* name\) \{[\s\S]*?^\}/m);
  const sysv=fs.readFileSync(path.join(source,'linker/linker_soinfo.cpp'),'utf8').match(/^uint32_t calculate_elf_hash\(const char\* name\) \{[\s\S]*?^\}/m);
  if(!gnu||!sysv)throw Error('Original Bionic ELF symbol hash functions changed');
  const wrapper=`#pragma once
#include <cstdint>
#include <cstddef>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <utility>
namespace maclauncher::arm32::bionic {
struct Elf32_Rel { uint32_t r_offset,r_info; };
[[noreturn]] inline void packed_failure(const char *message){throw std::runtime_error(message);}
#define async_safe_fatal(...) packed_failure(__VA_ARGS__)
#define __predict_false(value) __builtin_expect(!!(value),0)
#define ElfW(name) Elf32_ ## name
${leb}
${reloc}
${gnu[0]}
${sysv[0]}
#undef ElfW
#undef __predict_false
#undef async_safe_fatal
}
`;
  writeChanged(path.join(output,'bionic-packed.h'),wrapper);
  // The published legacy header leaves this array incomplete. Its original
  // definition supplies the extent; the original NDK supplies each ABI layout.
  const streams=fs.readFileSync(path.join(source,'libc/stdio/stdio.cpp'),'utf8').match(/^FILE\s+__sF\s*\[\s*\d+\s*\](?=\s*=)/m);
  if(!streams)throw Error('Original Bionic standard stream declaration changed');
  const stdio=path.join(output,'bionic-stdio.h');
  const timezone=fs.readFileSync(path.join(source,'libc/tzcode/localtime.c'),'utf8').match(/^char\s*\*\s*tzname\s*\[\s*\d+\s*\](?=\s*=)/m);
  if(!timezone)throw Error('Original Bionic timezone name declaration changed');
  writeChanged(stdio,'#include <stdio.h>\n#include <bits/struct_file.h>\n#include <time.h>\nextern '+streams[0]+';\nextern '+timezone[0]+';\n');
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  fs.copyFileSync(path.join(source,'libc/NOTICE'),path.join(notices,'bionic.txt'));
  fs.copyFileSync(path.join(source,'libstdc++/NOTICE'),path.join(notices,'bionic-libstdc++.txt'));
  // Allocation operators must retain ARM32 size_t and C++ symbol encoding.
  // The original unit calls the shared malloc/free owners; only its terminal
  // diagnostic is routed to the already-supplied Android logging endpoint.
  const allocation=path.join(output,'bionic-new.cpp');
  const originalNew=fs.readFileSync(path.join(source,'libc/bionic/new.cpp'),'utf8');
  if(!originalNew.includes('#include <async_safe/log.h>'))throw Error('Original Bionic allocation diagnostic include changed');
  writeChanged(allocation,originalNew.replace('#include <async_safe/log.h>','#include <android/log.h>\n#define async_safe_fatal(...) __android_log_assert(nullptr, "libc", __VA_ARGS__)'));
  fs.writeFileSync(path.join(notices,'bionic.json'),JSON.stringify({revision:pins.aospBionic,files},null,2)+'\n');
  const jump=prepareArm32JumpSources({bionicSource,output:path.join(output,'jump')});
  const search=prepareSearchSources({bionicSource,output:path.join(output,'search')});
  const synchronization=prepareArm32Synchronization({bionicSource,output:path.join(output,'synchronization')});
  const characterData=prepareArm32LegacyCharacterData({bionicSource,output:path.join(output,'legacy-character-data')});
  fs.writeFileSync(path.join(notices,'bionic-portable-families.json'),JSON.stringify({revision:pins.aospBionic,jump:jump.files,search:search.relativeUnits,synchronization:synchronization.files,characterData:characterData.files},null,2)+'\n');
  return {ownerHeaders:[path.join(source,'libc/private/bionic_ssp.h'),stdio],aeabi:path.join(source,'libc/arch-arm/bionic/__aeabi.c'),allocation:{source:allocation,include:path.join(source,'libstdc++/include')},portableFamilies:[jump,search,synchronization,characterData,{units:[path.join(source,'libc/arch-arm/bionic/exidx_dynamic.c')],include:source}]};
}
