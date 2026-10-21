import {includeDependencies} from './upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {farEmitterSource} from '../art/x18-far-emitter-source.mjs';
import {farStackResumeSource} from '../art/x18-far-stack-source.mjs';

// Extend the existing Klepton thread-slot veneer; preserve its decoder, scratch
// register ownership, pool bounds, code/data filtering and branch checks.
export function prepareX18Source(klepton,build){
  let source=fs.readFileSync(path.join(klepton,'runtime/kl_x18.c'),'utf8');
  source=includeDependencies(source,'#include <errno.h>');
  const replace=(from,to)=>{if(source.split(from).length!==2)throw Error('Klepton register veneer source changed');source=source.replace(from,to);};
  replace('    o->gp_used |= 1u << r;','    o->gp_used |= 1u << r;\n    if (role & KLX_W) o->gp_written |= 1u << r;');
  replace('static int dp_imm(uint32_t w, klx_info *o) {',`// Architectural reserved fields in the already recognized GP families. The
// same predicate informs the data-neighborhood pass; unknown extensions remain
// neutral rather than becoming evidence that an executable range is data.
static int klj_reserved_gp_encoding(uint32_t w) {
    unsigned sf=w>>31, immediate=(w>>23)&63u, family=(w>>24)&31u;
    // ADDG/SUBG require X registers, clear S/op3 and fixed bits 15:14.
    if(immediate==0x23 && (w&0xa040c000u)!=0x80000000u)return 1;
    if(immediate==0x24) {
        unsigned n=(w>>22)&1u, imms=(w>>10)&63u;
        unsigned selector=(n<<6)|(~imms&63u);
        if((!sf&&n)||selector<2)return 1;
        unsigned levels=(1u<<(31u-(unsigned)__builtin_clz(selector)))-1u;
        if((imms&levels)==levels)return 1;
    }
    if(immediate==0x26 && (((w>>29)&3u)==3u || sf!=((w>>22)&1u) ||
        (!sf&&(w&((1u<<21)|(1u<<15))))))return 1;
    if(family==0x0a && !sf && (w&(1u<<15)))return 1;
    if(family==0x0b) {
        if(w&(1u<<21)) {
            if((w&(3u<<22))||((w>>10)&7u)>4u)return 1;
        } else if(((w>>22)&3u)==3u || (!sf&&(w&(1u<<15))))return 1;
    }
    if((w&0x1fe00000u)==0x1a000000u && (w&0xfc00u)) {
        // RMIF and SETF8/16 share the carry major group and have no Rm/Rd.
        if((w&0xffe07c10u)==0xba000400u)return 0;
        if((w&0xffffbc1fu)==0x3a00080du)return 0;
        return 1;
    }
    if(family==0x1a) {
        unsigned group=(w>>21)&7u;
        if(group&1u)return 1;
        if(group==2u && (!(w&(1u<<29)) || (w&0x410u)))return 1;
        if(group==4u && (w&((1u<<29)|(1u<<11))))return 1;
    }
    if(family==0x1b) {
        unsigned opcode=(w>>21)&7u;
        if(((w>>29)&3u) || (!sf&&opcode))return 1;
        if(opcode==3u||opcode==4u||opcode==7u)return 1;
        if((opcode==2u||opcode==6u) && ((w&0xfc00u)!=0x7c00u))return 1;
    }
    if((w&0x3a000000u)==0x28000000u) {
        unsigned opc=w>>30,vector=(w>>26)&1u,mode=(w>>23)&3u;
        if(opc==3u || (!vector&&opc==1u&&!mode))return 1;
    }
    if((w&0x3b000000u)==0x38000000u || (w&0x3b000000u)==0x39000000u) {
        unsigned size=w>>30, vector=(w>>26)&1u, opc=(w>>22)&3u;
        // SIMD B/H/S/D use opc 0/1; Q alone uses opc 2/3 with size 0.
        // Register offsets extend W/X indices, so option<1> must be set.
        // The atomic subgroup has GP operands, never SIMD operands.
        if(vector&&opc>=2u&&size)return 1;
        if(!(w&(1u<<24))&&(w&(1u<<21))) {
            unsigned mode=(w>>10)&3u;
            if(mode==2u&&!(w&(1u<<14)))return 1;
            if(vector&&mode==0u)return 1;
        }
    }
    if(!(w&(1u<<30))&&family==0x1eu&&
       (!(w&(1u<<21))||!(w&0xfc00u))) {
        // Original QEMU FP fixed/int conversion allocation rules. Fields in
        // unallocated words do not name GP registers. Keep all FP16, ordinary
        // conversion, FMOV lane and FJCVTZS forms without host feature gating.
        unsigned type=(w>>22)&3u, opcode=(w>>16)&7u, rounding=(w>>19)&3u;
        if(w&(1u<<29))return 1;
        if(!(w&(1u<<21))) {
            unsigned operation=rounding*8u+opcode;
            if(type==2u||(!sf&&!(w&(1u<<15))))return 1;
            if(operation!=2u&&operation!=3u&&operation!=24u&&operation!=25u)return 1;
        } else if(opcode<6u) {
            if(type==2u||(opcode>=2u&&rounding))return 1;
        } else {
            int move=!rounding&&(type==3u||type==sf);
            int lane=sf&&type==2u&&rounding==1u;
            int javascript=!sf&&type==1u&&rounding==3u&&opcode==6u;
            if(!move&&!lane&&!javascript)return 1;
        }
    }
    return 0;
}

static int dp_imm(uint32_t w, klx_info *o) {`);
  replace('    memset(o, 0, sizeof *o);','    memset(o, 0, sizeof *o);\n    if(klj_reserved_gp_encoding(w))return 0;');
  replace(`    case 0:                                    // add/sub with carry
    case 4:`, `    case 0:                                    // add/sub with carry
        if(w&0xfc00u) { // allocated RMIF / SETF8 / SETF16
            addfld(o,w,KLX_RN,KLX_R);return 1;
        }
        // fall through
    case 4:`);
  replace(`        unsigned L = (w >> 22) & 1, o2 = (w >> 23) & 1;
        addfld(o, w, KLX_RN, KLX_R);`, `        unsigned L = (w >> 22) & 1, o2 = (w >> 23) & 1;
        unsigned pair = (w >> 21) & 1, rt2 = (w >> 10) & 31;
        unsigned rs = (w >> 16) & 31;
        // CAS/CASP and ordered single-register forms fix Rt2 to 11111.
        // Those bits never name x18. Preserve unallocated packed words rather
        // than publishing a veneer that changes their reserved encoding.
        if ((o2 || (pair && !(w >> 31))) && rt2 != 31) return 0;
        if (o2 && !pair && rs != 31) return 0;
        if (!o2 && pair && !(w >> 31) && ((rs | w) & 1)) return 0;
        addfld(o, w, KLX_RN, KLX_R);`);
  replace(`    if ((w & 0xBE000000) == 0x0C000000) {
        unsigned post = (w >> 23) & 1;`, `    if ((w & 0xBE000000) == 0x0C000000) {
        // Architectural SIMD structure encodings, also described by the pinned
        // QEMU a64.decode LD/ST_mult and LD/ST_single declarations. Reserved
        // opcode combinations are data, not GP-register operands to rewrite.
        unsigned post = (w >> 23) & 1, rm = (w >> 16) & 31;
        unsigned size = (w >> 10) & 3;
        if (!post && rm) return 0;
        if (!((w >> 24) & 1)) {
            unsigned opcode = (w >> 12) & 15;
            if ((w >> 21) & 1) return 0;
            if (!((0x05d5u >> opcode) & 1)) return 0;
            // Only LD1/ST1 accept a single 64-bit lane (Q=0,size=3).
            if (size == 3 && !((w >> 30) & 1) &&
                !((0x04c4u >> opcode) & 1)) return 0;
        } else {
            unsigned scale = (w >> 14) & 3, s = (w >> 12) & 1;
            if (scale == 1 && (size & 1)) return 0;
            if (scale == 2 && ((size & 2) || ((size & 1) && s))) return 0;
            if (scale == 3 && (!((w >> 22) & 1) || s)) return 0;
        }`);
  replace('    if (((w >> 25) & 0x7f) == 0x6b) {          // br / blr / ret',`    if (((w >> 25) & 0x7f) == 0x6b) {          // br / blr / ret
        // The major opcode also contains unallocated encodings. Only actual
        // architectural register fields may acquire a veneer; packed data must
        // not become a branch merely because bits 9:5 happen to contain 18.
        // These are the complete branch-register forms, including PA operands.
        switch (w & 0xfffffc00u) {
        case 0xd61f0000u: case 0xd63f0000u: case 0xd65f0000u:
            if (w & 31u) return 0;
            break;
        case 0xd61f0800u: case 0xd61f0c00u:
        case 0xd63f0800u: case 0xd63f0c00u:
            if ((w & 31u) != 31u) return 0;
            break;
        case 0xd71f0800u: case 0xd71f0c00u:
        case 0xd73f0800u: case 0xd73f0c00u:
            addfld(o, w, KLX_RD, KLX_R); // authentication modifier
            break;
        case 0xd65f0800u: case 0xd65f0c00u:
            return (w & 0x3ffu) == 0x3ffu; // RETAA / RETAB: implicit operands
        case 0xd69f0000u: case 0xd6bf0000u:
            return (w & 0x3ffu) == 0x3e0u; // ERET / DRPS: implicit operands
        case 0xd69f0800u: case 0xd69f0c00u:
            return (w & 0x3ffu) == 0x3ffu; // ERETAA / ERETAB
        default: return 0;
        }`);
  replace('    if (!w) return 0;                      // padding, not a constant table',`    if (!w) return 0;                      // padding, not a constant table
    if (klj_reserved_gp_encoding(w)) return 1;
    if (((w >> 25) & 0x7f) == 0x6b) {
        klx_info decoded; klx_decode(w,&decoded);
        if (!decoded.ok) return 1;
    }`);
  replace('in.hazard || in.pcrel == KLX_PC_ADR || in.pcrel == KLX_PC_LITERAL ||','in.hazard ||');
  replace('// The emitter proper.',`// Stack writeback needs a real temporary frame: the original memory access
// may overwrite the old red zone. Keep all scratch below both guest extents,
// execute the access through the original SP in a scratch register, then commit
// the architecturally requested SP after restoring the scratch registers.
static int klj_stack_memory(uint32_t word, int *delta, uint32_t *plain) {
    if (((word >> 5) & 31u) != 31u || ((word >> 26) & 1u)) return 0;
    if ((word & 0x3a000000u) == 0x28000000u) {
        unsigned mode = (word >> 23) & 3u, opc = word >> 30;
        if ((mode != 1 && mode != 3) || opc == 3) return 0;
        int immediate = (int)((word >> 15) & 127u); if (immediate & 64) immediate -= 128;
        int tagged = opc == 1 && !((word >> 22) & 1u);
        *delta = immediate * (tagged ? 16 : opc == 2 ? 8 : 4);
        *plain = (word & ~(3u << 23)) | (2u << 23);
        if (mode == 1) *plain &= ~(127u << 15);
        return 1;
    }
    if ((word & 0x3b200000u) == 0x38000000u) {
        unsigned mode = (word >> 10) & 3u;
        if (mode != 1 && mode != 3) return 0;
        int immediate = (int)((word >> 12) & 511u); if (immediate & 256) immediate -= 512;
        *delta = immediate; *plain = word & ~(3u << 10);
        if (mode == 1) *plain &= ~(511u << 12);
        return 1;
    }
    return 0;
}

static int klj_stack_simd(uint32_t word, uint32_t *plain) {
    klx_info info;klx_decode(word,&info);
    if(!info.ok || info.hazard!=KLX_HZ_SPWRITE ||
       (word&0xbe000000u)!=0x0c000000u || ((word>>16)&31u)!=18u)return 0;
    *plain=word&~((1u<<23)|(31u<<16));
    return 1;
}

static int klj_stack_alu(uint32_t word,unsigned a,unsigned b,uint32_t *plain) {
    klx_info info;klx_decode(word,&info);
    unsigned immediate=(word>>23)&63u;
    int extended=((word>>24)&31u)==0x0bu && (word&(1u<<21));
    if(!info.ok || !info.nfields || info.hazard!=KLX_HZ_SPWRITE ||
       (word&31u)!=31u || (word&(1u<<29)) ||
       (immediate!=0x22u && immediate!=0x23u && !extended))return 0;
    *plain=(klx_substitute(word,&info,b)&~31u)|b;
    if(((word>>5)&31u)==31u)*plain=(*plain&~(31u<<5))|(a<<5);
    return 1;
}

// The emitter proper.`);
  replace('        if (in.hazard ||',`        int stack_delta = 0; uint32_t stack_plain = 0;
        int stack_memory = in.hazard == KLX_HZ_SPWRITE && klj_stack_memory(word, &stack_delta, &stack_plain);
        if ((in.hazard && !stack_memory) ||`);
  replace('        int stack_delta = 0; uint32_t stack_plain = 0;',`        uint32_t simd_plain,alu_plain;unsigned frame_a,frame_b;
        pick_scratch(in.gp_used,&frame_a,&frame_b);
        int stack_simd=klj_stack_simd(word,&simd_plain);
        int stack_alu=klj_stack_alu(word,frame_a,frame_b,&alu_plain);
        if (stack_simd || stack_alu) {
            if(out+VEN_MAX_INSN>pool_end){note_refusal(word);st->refused++;continue;}
            unsigned a=frame_a,b=frame_b;
            uint32_t body[VEN_MAX_INSN],patch;unsigned k=0;int ok=1;
            body[k++]=0xd10083ffu; // sub sp,sp,#32: owned scratch frame
            body[k++]=enc_stp(a,b,0);
            body[k++]=enc_mrs_tpidrro(a);
            body[k++]=enc_ldr(b,a,(unsigned)KLX_TSD_SLOT*8);
            body[k++]=0x910083e0u|a; // original SP
            body[k++]=stack_simd ? (simd_plain&~(31u<<5))|(a<<5) : alu_plain;
            body[k++]=stack_simd ? 0x8b000000u|(b<<16)|(a<<5)|b : 0xd503201fu; // final SP
            // Complete through the existing saved signal context. An arbitrary
            // SP increase must not expose unrestored spills to a signal frame.
            body[k++]=0xd4400000u|(((stack_simd?0x6800u:0x6c00u)|(a<<5)|b)<<5);
            ok&=enc_b(vpc+k*4,spc+4,&body[k]);k++;
            if(stack_alu)body[k++]=word; // validated completion metadata, never executed
            ok&=enc_b(spc,vpc,&patch);
            if(!ok){note_refusal(word);st->refused++;continue;}
            memcpy(out,body,k*4);out+=k;w[i]=patch;st->patched++;continue;
        }
        int stack_delta = 0; uint32_t stack_plain = 0;`);
  replace('        body[k++] = enc_stp(a, b, -16);\n        body[k++] = enc_mrs_tpidrro(a);\n\n        if (in.pcrel',`        unsigned stack_reserve = stack_memory ? ((unsigned)(stack_delta < 0 ? -stack_delta : 0) + 31u) & ~15u : 0;
        if (stack_memory) body[k++] = 0xd10003ffu | (stack_reserve << 10); // sub sp,sp,#reserve
        body[k++] = enc_stp(a, b, stack_memory ? 0 : -16);
        if (!stack_memory || (in.roles & KLX_R)) body[k++] = enc_mrs_tpidrro(a);

        if (in.pcrel`);
  replace('            if (in.pcrel == KLX_PC_ADRP) {',`            if (stack_memory) {
                body[k++] = 0x910003e0u | (stack_reserve << 10) | a; // add a,sp,#reserve
                sub = klx_substitute(stack_plain, &in, b);
                sub = (sub & ~(31u << 5)) | (a << 5);
            }
            if (in.pcrel == KLX_PC_ADRP) {`);
  replace('            if (in.roles & KLX_W) body[k++] = enc_str(b, a, (unsigned)KLX_TSD_SLOT * 8);\n            body[k++] = enc_ldp(a, b, -16);',`            if (in.roles & KLX_W) {
                if (stack_memory) body[k++] = enc_mrs_tpidrro(a);
                body[k++] = enc_str(b, a, (unsigned)KLX_TSD_SLOT * 8);
            }
            body[k++] = enc_ldp(a, b, stack_memory ? 0 : -16);
            if (stack_memory) body[k++] = 0x910003ffu | ((stack_reserve + stack_delta) << 10); // commit guest SP`);
  replace('            body[k++] = sub;\n            if (in.roles & KLX_W)',`            if (in.pcrel == KLX_PC_ADR || in.pcrel == KLX_PC_LITERAL) {
                // PC-relative addresses belong to the original instruction.
                // Materialize it in scratch with ADRP+ADD; the nearby pool may
                // exceed ADR's 1 MiB range but stays inside ADRP's 4 GiB range.
                uint32_t raw = ((word >> 5) & 0x7ffffu) << 2;
                if (in.pcrel == KLX_PC_ADR) raw |= (word >> 29) & 3u;
                int64_t immediate = (int64_t)(raw ^ 0x100000u) - 0x100000;
                uint64_t target = spc + (uint64_t)immediate;
                int64_t pages = ((int64_t)(target & ~0xfffull) - (int64_t)((vpc + k * 4) & ~0xfffull)) / 4096;
                if (pages < -(1LL << 20) || pages >= (1LL << 20)) ok = 0;
                uint32_t encoded = (uint32_t)((uint64_t)pages & 0x1fffffu);
                body[k++] = 0x90000000u | ((encoded & 3u) << 29) | ((encoded >> 2) << 5) | b;
                body[k++] = 0x91000000u | ((uint32_t)(target & 0xfffu) << 10) | (b << 5) | b;
                if (in.pcrel == KLX_PC_LITERAL) {
                    // LDR W/X and LDRSW preserve their zero/sign extension.
                    // The destination doubles as the address scratch, leaving
                    // the thread-slot base and every guest register intact.
                    unsigned opc = word >> 30;
                    uint32_t load = opc == 0 ? 0xb9400000u : opc == 1 ? 0xf9400000u : 0xb9800000u;
                    body[k++] = load | (b << 5) | b;
                }
            } else body[k++] = sub;
            if (in.roles & KLX_W)`);
  replace('    imm = (imm << 43) >> 43;                                  // sign-extend 21 bits\n    int64_t target = (int64_t)(oldpc & ~0xFFFULL) + (imm << 12);',
    '    imm = (imm ^ 0x100000) - 0x100000;                         // sign-extend 21 bits\n    int64_t target = (int64_t)(oldpc & ~0xFFFULL) + imm * 4096;');
  replace(`                ok &= fits;
            }
            if (in.pcrel == KLX_PC_ADR || in.pcrel == KLX_PC_LITERAL) {`,
    `                if (fits) body[k++] = sub;
                else {
                    // A nearby island can cross ADRP's signed 4 GiB edge.
                    // Preserve the original page through the existing exact
                    // immediate emitter instead of requiring another island.
                    uint32_t raw = (((word >> 5) & 0x7ffffu) << 2) | ((word >> 29) & 3u);
                    int64_t pages = (int64_t)(raw ^ 0x100000u) - 0x100000;
                    uint64_t target = (spc & ~0xfffULL) + (uint64_t)(pages * 4096);
                    k += enc_mov_imm64(b, target, body + k);
                }
            } else if (in.pcrel == KLX_PC_ADR || in.pcrel == KLX_PC_LITERAL) {`);
  // Derive a necessary-condition prefilter from the complete original decoder.
  // Recognition, data classification and veneer generation remain upstream.
  const fields=[...new Set([...source.matchAll(/addfld\(o, w, (KLX_[A-Z]+),/g)].map(match=>match[1]))];
  const system=[...source.matchAll(/if \(\(w & (0x[0-9A-Fa-f]+u)\) == (0x[0-9A-Fa-f]+u)\) o->sysreg = (KLX_SYS_\w+);/g)];
  const assignments=[...source.matchAll(/o->sysreg\s*=\s*(KLX_SYS_\w+)/g)].filter(match=>match[1]!=='KLX_SYS_NONE');
  if(!fields.length||system.length!==assignments.length)throw Error('Klepton register decoder operands changed');
  const predicates=[...fields.map(field=>`((w >> ${field}) & 31u) == 18u`),...system.map(match=>`(w & ${match[1]}) == ${match[2]}`)];
  replace('static unsigned klx_survey(',`static inline int klj_x18_candidate(uint32_t w) {
    return ${predicates.join(' || ')};
}

static unsigned klx_survey(`);
  const decode='        klx_info in;\n        klx_decode(w[i], &in);';
  if(source.split(decode).length!==3)throw Error('Klepton register scan loops changed');
  source=source.replaceAll(decode,'        if (!klj_x18_candidate(w[i])) continue;\n'+decode);
  // The live loader has already surveyed the chunk to size its veneer pool.
  // Reuse that result rather than decoding the entire chunk again in emit.
  replace(`int kl_x18_emit(void *code, size_t size, uint64_t code_va,
                void *pool, size_t poolcap, uint64_t pool_va,
                kl_x18_stats *st, size_t *pool_used) {`,`static int klj_x18_emit_surveyed(void *code, size_t size, uint64_t code_va,
                void *pool, size_t poolcap, uint64_t pool_va,
                kl_x18_stats *st, size_t *pool_used,
                unsigned want, unsigned data_words, unsigned ctr_sites) {`);
  replace(`    unsigned want = klx_survey(code, size, &st->data_words, &st->ctr_sites);
    st->sites = want;
    if (pool_used)`, `    st->data_words = data_words; st->ctr_sites = ctr_sites;
    st->sites = want;
    if (pool_used)`);
  replace('static uint8_t *klx_place_pool(',`int kl_x18_emit(void *code, size_t size, uint64_t code_va,
                void *pool, size_t poolcap, uint64_t pool_va,
                kl_x18_stats *st, size_t *pool_used) {
    unsigned data_words, ctr_sites;
    unsigned want = klx_survey(code, size, &data_words, &ctr_sites);
    return klj_x18_emit_surveyed(code, size, code_va, pool, poolcap, pool_va,
                                st, pool_used, want, data_words, ctr_sites);
}

static uint8_t *klx_place_pool(`);
  replace('static uint8_t *klx_place_pool(',`int kl_x18_emit_context(void *code,size_t size,uint64_t address,
    void *pool,size_t capacity,uint64_t pool_address,kl_x18_stats *stats,size_t *used,
    const klx_code_context *context) {
    unsigned data,ctr;
    unsigned count=klj_x18_survey_context(code,size,&data,&ctr,context);
    return klj_x18_emit_surveyed(code,size,address,pool,capacity,pool_address,
        stats,used,count,data,ctr,context);
}

static uint8_t *klx_place_pool(`);
  replace(`    int rc = kl_x18_emit(code, size, (uint64_t)(uintptr_t)code,
                         base, cap, (uint64_t)(uintptr_t)base, st, NULL);`,`    int rc = klj_x18_emit_surveyed(code, size, (uint64_t)(uintptr_t)code,
                         base, cap, (uint64_t)(uintptr_t)base, st, NULL,
                         want, st->data_words, st->ctr_sites);`);
  // Function/initializer boundaries bound the neighborhood vote without
  // fragmenting allocation into one 64 KiB veneer pool per function.
  replace('static int klx_looks_like_data(const uint32_t *w, size_t n, size_t i) {',
    'static int klj_x18_data_context(const uint32_t *w, size_t n, size_t i, const klx_code_context *context) {');
  replace('    unsigned bad = 0;',`    if(context && context->reachable_count) {
        uint64_t address=context->address+i*4;size_t left=0,right=context->reachable_count;
        while(left<right){size_t mid=left+(right-left)/2;if(context->reachable[mid].start<=address)left=mid+1;else right=mid;}
        if(left && address<context->reachable[left-1].end)return 0;
        // Unwind/function addresses may describe encrypted source bytes.
        // Without an actual decoded control path they cannot turn a narrow
        // constant neighborhood into code merely by surrounding it.
        context=NULL;
    }
    if (context && context->count) {
        uint64_t address=context->address+i*4;size_t left=0,right=context->count;
        while(left<right){size_t mid=left+(right-left)/2;if(context->boundaries[mid]<=address)left=mid+1;else right=mid;}
        if(left && context->boundaries[left-1]>context->address) {
            size_t bound=(size_t)((context->boundaries[left-1]-context->address)/4);if(bound>lo)lo=bound;
        }
        if(left<context->count && context->boundaries[left]>=context->address) {
            uint64_t bound=(context->boundaries[left]-context->address)/4;if(bound<hi)hi=(size_t)bound;
        }
    }
    unsigned bad = 0;`);
  replace('// Exposed so the offline tool reports',`static int klx_looks_like_data(const uint32_t *w,size_t n,size_t i) {
    return klj_x18_data_context(w,n,i,NULL);
}
int kl_x18_is_data_context(const void *code,size_t size,size_t index,const klx_code_context *context) {
    return klj_x18_data_context(code,size/4,index,context);
}

// Exposed so the offline tool reports`);
  replace(`static unsigned klx_survey(const void *code, size_t size, unsigned *data,
                           unsigned *ctr) {`,`static unsigned klj_x18_survey_context(const void *code, size_t size, unsigned *data,
                           unsigned *ctr, const klx_code_context *context) {`);
  replace('        if (klx_looks_like_data(w, n, i)) { if (data && in.nfields) (*data)++; continue; }',
    '        if (klj_x18_data_context(w, n, i, context)) { if (data && in.nfields) (*data)++; continue; }');
  replace('unsigned kl_x18_count(const void *code, size_t size) {',`static unsigned klx_survey(const void *code,size_t size,unsigned *data,unsigned *ctr) {
    return klj_x18_survey_context(code,size,data,ctr,NULL);
}
unsigned kl_x18_count_context(const void *code,size_t size,const klx_code_context *context) {
    unsigned ctr=0;return klj_x18_survey_context(code,size,NULL,&ctr,context)+ctr;
}
unsigned kl_x18_capacity(const void *code,size_t size) {
    const uint32_t *words=code;unsigned count=0;
    for(size_t i=0;i<size/4;i++)if(klj_x18_candidate(words[i])) {
        klx_info in;klx_decode(words[i],&in);count+=!!in.nfields+!!in.sysreg;
    }
    return count;
}
unsigned kl_x18_count(const void *code, size_t size) {`);
  replace('                unsigned want, unsigned data_words, unsigned ctr_sites) {',
    '                unsigned want, unsigned data_words, unsigned ctr_sites, const klx_code_context *context) {');
  replace('        if (klx_looks_like_data(w, n, i)) continue;',
    '        if (klj_x18_data_context(w, n, i, context)) continue;');
  replace('                                st, pool_used, want, data_words, ctr_sites);',
    '                                st, pool_used, want, data_words, ctr_sites, NULL);');
  replace(`int kl_x18_patch(void *code, size_t size, void *arena, size_t arena_size,
                 size_t *arena_used, kl_x18_stats *st) {`,`int kl_x18_patch_context(void *code, size_t size, void *arena, size_t arena_size,
                 size_t *arena_used, kl_x18_stats *st, const klx_code_context *context) {`);
  replace('    unsigned want = klx_survey(code, size, &st->data_words, &st->ctr_sites);',
    '    unsigned want = klj_x18_survey_context(code, size, &st->data_words, &st->ctr_sites, context);');
  replace('                         want, st->data_words, st->ctr_sites);',
    '                         want, st->data_words, st->ctr_sites, context);');
  replace('// ---------------------------------------------------- data inside code',`int kl_x18_patch(void *code,size_t size,void *arena,size_t arena_size,size_t *arena_used,kl_x18_stats *stats) {
    return kl_x18_patch_context(code,size,arena,arena_size,arena_used,stats,NULL);
}

// ---------------------------------------------------- data inside code`);
  // The original linker retains the resource lease itself. Reuse the same
  // placement/decoder without an untracked fallback allocation on this path.
  replace(`int kl_x18_patch_context(void *code, size_t size, void *arena, size_t arena_size,
                 size_t *arena_used, kl_x18_stats *st, const klx_code_context *context) {`,
    `static int klj_x18_patch_owned(void *code, size_t size, void *arena, size_t arena_size,
                 size_t *arena_used, kl_x18_stats *st, const klx_code_context *context, int owned) {`);
  replace('    if (!base) base = klx_place_pool(code, size, cap);',
    '    if (!base && !owned) base = klx_place_pool(code, size, cap);');
  replace('    mprotect(base, cap, PROT_READ | PROT_EXEC);',
    '    if (mprotect(base, cap, PROT_READ | PROT_EXEC) != 0 && owned) return -1;');
  replace('int kl_x18_patch(void *code,size_t size,void *arena,size_t arena_size,size_t *arena_used,kl_x18_stats *stats) {',`
int kl_x18_patch_context(void *code,size_t size,void *arena,size_t arena_size,size_t *used,kl_x18_stats *stats,const klx_code_context *context) {
    return klj_x18_patch_owned(code,size,arena,arena_size,used,stats,context,0);
}
int kl_x18_patch_owned(void *code,size_t size,void *arena,size_t arena_size,size_t *used,kl_x18_stats *stats,const klx_code_context *context) {
    int result=klj_x18_patch_owned(code,size,arena,arena_size,used,stats,context,1);
    if(!result && stats->refused) { errno=ENOEXEC;return -1; }
    return result;
}
void *kl_x18_reserve_near(void *code,size_t size,size_t capacity) {
    uint8_t *memory=klx_place_pool(code,size,capacity);
    if(!memory)return NULL;
    uintptr_t first=(uintptr_t)code,last=first+size,low=(uintptr_t)memory,high=low+capacity;
    if(last<first||high<low|| (low<last && last-low>=(UINT64_C(1)<<27)) ||
        (first<high && high-first>=(UINT64_C(1)<<27))) {
        munmap(memory,capacity);errno=ERANGE;return NULL;
    }
    return memory;
}
int kl_x18_patch(void *code,size_t size,void *arena,size_t arena_size,size_t *arena_used,kl_x18_stats *stats) {`);
  const header=fs.readFileSync(path.join(klepton,'runtime/kl_x18.h'),'utf8')
    .replace('    unsigned nfields;', '    uint32_t gp_written; // original decoded GP write roles\n    unsigned nfields;');
  if(header.split('#define KLX_VEN_MAX_INSN 8').length!==2)throw Error('Klepton veneer size declaration changed');
  const contextDeclarations=`
#include ${JSON.stringify(path.join(import.meta.dirname,'instruction-context.h'))}
int kl_x18_is_data_context(const void *,size_t,size_t,const klx_code_context *);
int klx_stack_resume(void *);
unsigned kl_x18_count_context(const void *,size_t,const klx_code_context *);
unsigned kl_x18_capacity(const void *,size_t);
int kl_x18_emit_context(void *,size_t,uint64_t,void *,size_t,uint64_t,kl_x18_stats *,size_t *,const klx_code_context *);
int kl_x18_patch_context(void *,size_t,void *,size_t,size_t *,kl_x18_stats *,const klx_code_context *);
int kl_x18_patch_owned(void *,size_t,void *,size_t,size_t *,kl_x18_stats *,const klx_code_context *);
void *kl_x18_reserve_near(void *,size_t,size_t);
`;
  fs.writeFileSync(path.join(build,'kl_x18.h'),header.replace('#define KLX_VEN_MAX_INSN 8','#define KLX_VEN_MAX_INSN 12')
    .replace('\n#endif\n',contextDeclarations+'\n#endif\n'));
  source+='\n#include '+JSON.stringify(path.join(import.meta.dirname,'instruction-evidence.h'))+'\n';
  source+='\n#include '+JSON.stringify(path.join(import.meta.dirname,'x18-stack-resume.h'))+'\n';
  const output=path.join(build,'x18-source.c');fs.writeFileSync(output,source);return output;
}

// The two execution engines share the instruction compiler and bounded Darwin
// stack-veneer completion. The caller owns loading, thread-key allocation and
// executable addresses; completion retains that caller's interrupted context.
export function prepareX18Core(klepton,build){
  fs.mkdirSync(build,{recursive:true});
  const generated=prepareX18Source(klepton,build);
  const source=fs.readFileSync(generated,'utf8');
  const between=(begin,end)=>{
    const a=source.indexOf(begin),b=source.indexOf(end,a);
    if(a<0||b<0)throw Error('Klepton register emission boundary changed');
    return source.slice(a,b);
  };
  const decoder=between('static void addfld(', '// ============================ veneer emission');
  const survey=between('#define KLX_WIN_HALF', '// ---------- instruction encoders');
  let emit=between('static uint32_t enc_stp(', 'int kl_x18_emit(void *code, size_t size, uint64_t code_va,');
  const trim=(begin,end,replacement)=>{
    const a=emit.indexOf(begin),b=emit.indexOf(end,a);
    if(a<0||b<0)throw Error('Klepton pure register emission source changed');
    emit=emit.slice(0,a)+replacement+emit.slice(b);
  };
  trim('static void note_refusal(', '// The emitter proper.',
    'static void note_refusal(uint32_t word) { (void)word; }\n');
  // The stack-memory adaptation precedes the emitter marker and is retained.
  const stack=between('static int klj_stack_memory(', '// The emitter proper.');
  emit=emit.replace('unsigned ctr_sites, const klx_code_context *context) {',
    'unsigned ctr_sites, const klx_code_context *context, unsigned slot, uint64_t ctr) {')
    .replace('want && veneer_enabled()', 'want != 0')
    .replace('st->ctr_sites && ctr_enabled()', 'st->ctr_sites != 0')
    .replaceAll('(unsigned)KLX_TSD_SLOT', 'slot').replaceAll('ctr_value()', 'ctr');
  trim('        // KL_X18_MAP=', '        memcpy(out, body, k * 4);', '');
  const api=`
int klx_core_classify(uint32_t word) {
    klx_info info; klx_decode(word,&info);
    return info.nfields ? 1 | (info.hazard << 8) : 0;
}
int klx_core_emit(uint32_t word,uint64_t site,uint64_t pool,unsigned slot,uint64_t ctr,
                  uint32_t *words,size_t capacity) {
    kl_x18_stats stats;size_t used=0;
    klx_info info;klx_decode(word,&info);
    if(!info.nfields || slot>=4096) return -1;
    int result=klj_x18_emit_surveyed(&word,4,site,words,capacity*4,pool,&stats,&used,
                                    1,0,info.sysreg!=0,NULL,slot,ctr);
    if(result || stats.refused || stats.ctr_refused || !(stats.patched+stats.ctr_patched))return -1;
    return (int)(used/4);
}
`;
  const output=path.join(build,'x18-core.c');
  const header=path.join(build,'kl_x18.h');
  fs.writeFileSync(header,fs.readFileSync(header,'utf8').replace(
    JSON.stringify(path.join(import.meta.dirname,'instruction-context.h')), '"instruction-context.h"'));
  fs.copyFileSync(path.join(import.meta.dirname,'instruction-context.h'),path.join(build,'instruction-context.h'));
  for(const name of ['instruction-evidence.h','image-code-metadata.h','image-code-ranges.h','image-instruction-evidence.h','x18-stack-resume.h'])
    fs.copyFileSync(path.join(import.meta.dirname,name),path.join(build,name));
  const image=fs.readFileSync(path.join(klepton,'runtime/kl_image.c'),'utf8');
  const types=image.indexOf('typedef struct { uint8_t e_ident[16];'),end=image.indexOf('struct kl_image {',types);
  if(types<0||end<0)throw Error('Klepton ELF declarations changed');
  fs.writeFileSync(path.join(build,'image-elf-types.h'),image.slice(types,end));
  fs.writeFileSync(output,'#include <string.h>\n#include <stdlib.h>\n#include "kl_x18.h"\n#define VEN_MAX_INSN KLX_VEN_MAX_INSN\n'+decoder+survey+stack+emit+api+farEmitterSource(emit)+'\n#include "instruction-evidence.h"\n#include "image-elf-types.h"\n#include "image-instruction-evidence.h"\n#include "x18-stack-resume.h"\n'+farStackResumeSource(fs.readFileSync(path.join(import.meta.dirname,'x18-stack-resume.h'),'utf8')));
  return output;
}

