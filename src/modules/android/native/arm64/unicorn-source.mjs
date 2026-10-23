import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {prepareSourceTree,writeChanged} from '../upstream/source-files.mjs';

/** The Linux process has direct native peers; retain QEMU's real host atomics. */
export function prepareUnicorn({source,output}){
 const pin=JSON.parse(fs.readFileSync(new URL('./unicorn-source.json',import.meta.url),'utf8'));
 const revision=execFileSync('git',['-C',source,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
 if(revision!==pin.revision)throw Error('Unicorn revision does not match unicorn-source.json');
 prepareSourceTree(output,temporary=>{
  const archive=execFileSync('git',['-C',source,'archive','--format=tar',revision],{maxBuffer:64<<20});
  execFileSync('tar',['-xf','-','-C',temporary],{input:archive});
  const file=path.join(temporary,'qemu/include/exec/exec-all.h'),before='static inline uint32_t curr_cflags(void)\n{\n    return 0;\n}';
  const text=fs.readFileSync(file,'utf8');
  if(text.split(before).length!==2)throw Error('Original Unicorn parallel atomic boundary changed');
  writeChanged(file,text.replace(before,'static inline uint32_t curr_cflags(void)\n{\n    return CF_PARALLEL;\n}'));
  // Probe the same public compiler builtins used by QEMU's atomic owner.
  // Clang does not declare GCC's size-suffixed private builtin spellings.
  const configure=path.join(temporary,'qemu/configure');let checks=fs.readFileSync(configure,'utf8');
  for(const operation of ['load','store','compare_exchange','exchange','fetch_add']){
   const pattern=new RegExp('__atomic_'+operation+'_(8|16)\\b','g');
   const count=[...checks.matchAll(pattern)].length;
   if(count!==(operation==='exchange'||operation==='fetch_add'?1:2))throw Error('Original Unicorn atomic compiler probe changed: '+operation);
   checks=checks.replace(pattern,'__atomic_'+operation+(operation==='fetch_add'?'':'_n'));
  }
  writeChanged(configure,checks,0o755);
  const edit=(name,from,to)=>{const file=path.join(temporary,name),text=fs.readFileSync(file,'utf8');if(text.split(from).length!==2)throw Error('Original Unicorn memory owner boundary changed: '+name);writeChanged(file,text.replace(from,to));};
  // Darwin exposes cache geometry through sysctl and cache maintenance through
  // sys_icache_invalidate. Its EL0 cannot read CTR_EL0 when that query fails;
  // retain QEMU's existing conservative fallback instead of a privileged MRS.
  edit('qemu/util/cacheinfo.c','#if defined(__aarch64__)\n\nstatic void arch_cache_info',
   '#if defined(__aarch64__) && !defined(__APPLE__)\n\nstatic void arch_cache_info');
  edit('include/uc_priv.h','    int cpu_model;',
   '    int cpu_model;\n    void *(*native_atomic_owner)(void *, uint64_t, size_t, unsigned);\n    void *native_atomic_owner_data;\n    sigjmp_buf native_atomic_jump;\n    volatile sig_atomic_t native_atomic_active;\n    uint64_t native_atomic_guest, native_atomic_host, native_atomic_length;\n    uintptr_t native_atomic_retaddr;\n    volatile sig_atomic_t native_exclusive_active, native_exclusive_retry;\n    void (*native_written)(void *, uint64_t, size_t);\n    uint64_t native_exclusive_guest, native_exclusive_host, native_exclusive_length;');
  edit('qemu/accel/tcg/cputlb.c',
   '    /* Check TLB entry and enforce page permissions.  */\n    if (!tlb_hit(env->uc, tlb_addr, addr)) {',
   '    /* The native owner already validates the current mapping and both RMW rights. */\n    if (env->uc->native_atomic_owner) {\n        void *pointer = env->uc->native_atomic_owner(env->uc->native_atomic_owner_data, addr, (size_t)1 << s_bits, UC_PROT_READ | UC_PROT_WRITE);\n        if (!pointer) {\n            env->uc->invalid_addr = addr; env->uc->invalid_error = UC_ERR_WRITE_UNMAPPED;\n            cpu_loop_exit_restore(env_cpu(env), retaddr);\n        }\n        /* This C helper is outside MAP_JIT. Restore the TCG view at cleanup. */\n        env->uc->native_atomic_guest = addr;\n        env->uc->native_atomic_host = (uintptr_t)pointer;\n        env->uc->native_atomic_length = (size_t)1 << s_bits;\n        env->uc->native_atomic_retaddr = retaddr;\n        jit_write_protect(0);\n        return pointer;\n    }\n    /* Check TLB entry and enforce page permissions.  */\n    if (!tlb_hit(env->uc, tlb_addr, addr)) {');
  // The recovery point lives inside the original C helper, not in a returned
  // pointer callback. A native backing fault resumes original QEMU exception
  // recovery outside the signal handler, including exact guest-PC restoration.
  edit('qemu/accel/tcg/cputlb.c','#define ATOMIC_MMU_LOOKUP atomic_mmu_lookup(env, addr, oi, retaddr)',
   '#define ATOMIC_MMU_LOOKUP NATIVE_ATOMIC_LOOKUP(retaddr)');
  edit('qemu/accel/tcg/cputlb.c','#define ATOMIC_MMU_LOOKUP  atomic_mmu_lookup(env, addr, oi, GETPC())',
   '#define ATOMIC_MMU_LOOKUP NATIVE_ATOMIC_LOOKUP(GETPC())');
  edit('qemu/accel/tcg/cputlb.c','#define ATOMIC_MMU_DECLS',String.raw`#define NATIVE_ATOMIC_LOOKUP(return_pc) ({ \
    void *native_pointer = atomic_mmu_lookup(env, addr, oi, return_pc); \
    if (env->uc->native_atomic_owner) { \
        if (sigsetjmp(env->uc->native_atomic_jump, 1)) { \
            env->uc->native_atomic_active = 0; \
            env->uc->invalid_error = UC_ERR_WRITE_UNMAPPED; \
            cpu_loop_exit_restore(env_cpu(env), env->uc->native_atomic_retaddr); \
        } \
        env->uc->native_atomic_active = 1; \
    } \
    native_pointer; \
})
#define ATOMIC_MMU_DECLS`);
  edit('qemu/accel/tcg/cputlb.c','#define ATOMIC_MMU_CLEANUP\n','#define ATOMIC_MMU_CLEANUP do { env->uc->native_atomic_active = 0; jit_write_protect(env->uc->current_executable); } while (0)\n');
  // Reuse QEMU's complete exclusive instruction decoder and original helper
  // ABI; only its documented value-only memory monitor is replaced on A64.
  const helperHeader=path.join(temporary,'qemu/target/arm/helper-a64.h');
  writeChanged(helperHeader,fs.readFileSync(helperHeader,'utf8')+'\nDEF_HELPER_1(native_exclusive_clear, void, env)\nDEF_HELPER_3(native_exclusive_load, void, env, i64, i32)\nDEF_HELPER_3(native_exclusive_store, void, env, i64, i32)\nDEF_HELPER_2(native_dc_zva_written, void, env, i64)\n');
  const a64=path.join(temporary,'qemu/target/arm/helper-a64.c');
  writeChanged(a64,fs.readFileSync(a64,'utf8')+'\n'+fs.readFileSync(new URL('./unicorn-exclusives.c',import.meta.url),'utf8'));
  edit('qemu/target/arm/translate-a64.c',
   '    int idx = get_mem_index(s);\n    MemOp memop = s->be_data;',
   `    if (s->uc->native_atomic_owner) {
        TCGv_i32 flags=tcg_const_i32(tcg_ctx,size|(is_pair<<2)|((s->be_data!=MO_LE)<<3)|(rt<<4)|(rt2<<9));
        gen_helper_native_exclusive_load(tcg_ctx,tcg_ctx->cpu_env,addr,flags);tcg_temp_free_i32(tcg_ctx,flags);return;
    }
    int idx = get_mem_index(s);\n    MemOp memop = s->be_data;`);
  edit('qemu/target/arm/translate-a64.c',
   '    /* if (env->exclusive_addr == addr && env->exclusive_val == [addr]',
   `    if (s->uc->native_atomic_owner) {
        TCGv_i32 flags=tcg_const_i32(tcg_ctx,size|(is_pair<<2)|((s->be_data!=MO_LE)<<3)|(rt<<4)|(rt2<<9)|(rd<<14));
        gen_helper_native_exclusive_store(tcg_ctx,tcg_ctx->cpu_env,addr,flags);tcg_temp_free_i32(tcg_ctx,flags);return;
    }
    /* if (env->exclusive_addr == addr && env->exclusive_val == [addr]`);
  edit('qemu/target/arm/translate-a64.c',
   'static void gen_clrex(DisasContext *s, uint32_t insn)\n{\n    TCGContext *tcg_ctx = s->uc->tcg_ctx;',
   'static void gen_clrex(DisasContext *s, uint32_t insn)\n{\n    TCGContext *tcg_ctx = s->uc->tcg_ctx;\n    if(s->uc->native_atomic_owner)gen_helper_native_exclusive_clear(tcg_ctx,tcg_ctx->cpu_env);');
  // Observe only the successful original helper's canonical write extent.
  // The decoded operand has already passed through TCR-driven TBI lowering.
  edit('qemu/target/arm/translate-a64.c',
   '        gen_helper_dc_zva(tcg_ctx, tcg_ctx->cpu_env, tcg_rt);',
   '        gen_helper_dc_zva(tcg_ctx, tcg_ctx->cpu_env, tcg_rt);\n        if(s->uc->native_atomic_owner)gen_helper_native_dc_zva_written(tcg_ctx,tcg_ctx->cpu_env,tcg_rt);');
  // Unicorn's virtual TLB is a flat process address space, so it bypasses
  // the system MMU's TBI translation. Reuse QEMU's original userspace address
  // lowering for this EL0 owner, retaining every architectural pointer and
  // the original TCR-derived data/instruction tag controls.
  edit('qemu/target/arm/translate-a64.c',
   '    tcg_gen_mov_i64(tcg_ctx, clean, addr);\n    return clean;',
   '    if (s->uc->native_atomic_owner && s->user) {\n        gen_top_byte_ignore(s, clean, addr, s->tbid);\n    } else {\n        tcg_gen_mov_i64(tcg_ctx, clean, addr);\n    }\n    return clean;');
  // Unicorn's single-thread default replaced several original QEMU atomic
  // operations with plain loads/stores. Use the same QEMU primitives as its
  // remaining parallel helpers, including the original compare/exchange loops.
  const atomics=path.join(temporary,'qemu/accel/tcg/atomic_template.h');
  let helpers=fs.readFileSync(atomics,'utf8');
  const change=(from,to,count=1)=>{if(helpers.split(from).length!==count+1)throw Error('Original QEMU atomic helper changed: '+from.slice(0,60));helpers=helpers.split(from).join(to);};
  change('    ret = *haddr;\n    *haddr = val;','    ret = atomic_xchg__nocheck(haddr, val);');
  change('    ret = *haddr;\n    *haddr = BSWAP(val);','    ret = atomic_xchg__nocheck(haddr, BSWAP(val));');
  change('\n    cmp = *haddr;','\n    cmp = atomic_read__nocheck(haddr);');
  change('        cmp = *haddr;                                               \\\n        if (cmp == old)                                          \\\n            *haddr = new;',
         '        cmp = atomic_cmpxchg__nocheck(haddr, old, new);');
  change('\n    ldn = *haddr;','\n    ldn = atomic_read__nocheck(haddr);');
  change('        ldn = *haddr;                                               \\\n        if (ldn == ldo)                                             \\\n            *haddr = BSWAP(new);',
         '        ldn = atomic_cmpxchg__nocheck(haddr, ldo, BSWAP(new));');
  change('    val = BSWAP(val);\n    val = BSWAP(val);','    val = BSWAP(val);');
  writeChanged(atomics,helpers);
  const api=path.join(temporary,'uc.c');writeChanged(api,fs.readFileSync(api,'utf8')+
   '\nUNICORN_EXPORT uc_err uc_native_set_memory_owner(uc_engine *, void *(*)(void *, uint64_t, size_t, unsigned), void *);\nUNICORN_EXPORT uc_err uc_native_set_memory_owner(uc_engine *uc, void *(*owner)(void *, uint64_t, size_t, unsigned), void *data)\n{\n    if (!uc) return UC_ERR_ARG;\n    uc->native_atomic_owner = owner; uc->native_atomic_owner_data = data;\n    return UC_ERR_OK;\n}\n\nUNICORN_EXPORT int uc_native_recover_memory_fault(uc_engine *, uint64_t, uint64_t *, const void *);\nUNICORN_EXPORT int uc_native_recover_memory_fault(uc_engine *uc, uint64_t address, uint64_t *guest, const void *mask)\n{\n    if (!uc || !uc->native_atomic_active || address < uc->native_atomic_host || address - uc->native_atomic_host >= uc->native_atomic_length) return 0;\n    *guest = uc->native_atomic_guest + address - uc->native_atomic_host;\n    uc->invalid_addr = *guest; uc->native_atomic_active = 0;\n    if(mask)sigprocmask(SIG_SETMASK, (const sigset_t *)mask, NULL);\n    siglongjmp(uc->native_atomic_jump, 1);\n}\n\nUNICORN_EXPORT int uc_native_exclusive_active(uc_engine *);\nUNICORN_EXPORT int uc_native_exclusive_active(uc_engine *uc){return uc->native_exclusive_active;}\nUNICORN_EXPORT int uc_native_exclusive_retry(uc_engine *);\nUNICORN_EXPORT int uc_native_exclusive_retry(uc_engine *uc){return uc->native_exclusive_retry;}\nUNICORN_EXPORT uc_err uc_native_set_written(uc_engine *, void (*)(void *, uint64_t, size_t));\nUNICORN_EXPORT uc_err uc_native_set_written(uc_engine *uc, void (*written)(void *, uint64_t, size_t)){if(!uc)return UC_ERR_ARG;uc->native_written=written;return UC_ERR_OK;}\nUNICORN_EXPORT void uc_native_exclusive_clear(uc_engine *);\nUNICORN_EXPORT void uc_native_exclusive_clear(uc_engine *uc){__asm__ volatile(\"clrex\" ::: \"memory\");uc->native_exclusive_active=0;uc->native_exclusive_retry=0;}\n');
 });
 return {source:output,revision};
}
