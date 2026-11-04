#include "precise.h"
#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <limits>
#include <memory>
#include <cstring>
#include <pthread.h>
#include <csignal>
#include <csetjmp>
extern "C" int uc_native_recover_memory_fault(uc_engine *,uint64_t,uint64_t *,const void *);
extern "C" int uc_native_exclusive_active(uc_engine *);
extern "C" int uc_native_exclusive_retry(uc_engine *);
extern "C" uc_err uc_native_set_written(uc_engine *,void (*)(void *,uint64_t,size_t));
extern "C" void uc_native_exclusive_clear(uc_engine *);
extern "C" uc_err uc_native_set_memory_owner(uc_engine *,void *(*)(void *,uint64_t,size_t,unsigned),void *);

struct CopyScope {sigjmp_buf jump;uint64_t host,guest,length;volatile sig_atomic_t error=0;CopyScope *previous;};
static thread_local CopyScope *active_copy=nullptr;
static thread_local KlA64Precise *active_cpu=nullptr;

struct KlA64Precise {
    KlA64PreciseConfig config;
    uc_engine *uc=nullptr;
    uc_context *initial=nullptr;
    struct Mapped {KlA64Mapping view;std::vector<unsigned char> source;bool used=false;};
    std::vector<std::unique_ptr<Mapped>> mappings;
    KlA64Revalidate revalidate=nullptr;
    KlA64PreciseCounters counters{};
    uint64_t lastPage=UINT64_MAX;
    static bool executable_cacheable(const KlA64Mapping &m){return m.length<=16384&&m.permissions==5&&(m.flags&KL_A64_MAPPING_EXECUTION_OWNER)&&!(m.flags&(KL_A64_MAPPING_IO|KL_A64_MAPPING_JIT));}
    // A non-executable IO table carries no direct RAM or instruction-cache
    // authority: each access still uses the live, fault-contained callbacks.
    // Reusing that inert table is independent of PRIVATE_DATA's stronger
    // assertion that a store cannot alias executable source.
    static bool io_data_cacheable(const KlA64Mapping &m){return m.length<=16384&&(m.permissions==1||m.permissions==3)&&
        (m.flags==KL_A64_MAPPING_IO||(m.permissions==3&&m.flags==(KL_A64_MAPPING_IO|KL_A64_MAPPING_PRIVATE_DATA)));}
    static bool cacheable(const KlA64Mapping &m){return executable_cacheable(m)||io_data_cacheable(m);}
    bool unmap(const KlA64Mapping &m){if(uc_mem_unmap(uc,m.address,m.length)!=UC_ERR_OK)return false;++counters.retired;return true;}
    bool retire_all(){for(const auto &m:mappings)if(!unmap(m->view))return false;mappings.clear();return true;}
    bool admit_cache(bool enabled){
        for(auto at=mappings.begin();at!=mappings.end();){auto &m=**at;KlA64Mapping current{};
            if(!enabled||!revalidate||revalidate(config.owner,m.view.address,m.view.length,&current)||!cacheable(current)||
               current.address!=m.view.address||current.length!=m.view.length||current.host!=m.view.host||current.permissions!=m.view.permissions||current.flags!=m.view.flags){
                return retire_all();
            }
            ++counters.revalidated;
            // Executable source bytes belong to a newly acquired original
            // object. Non-executable IO views use live callbacks and
            // never snapshot data or read it during re-admission.
            if(executable_cacheable(current)){
                if(m.source.size()!=current.length)return false;
                if(std::memcmp(m.source.data(),current.host,current.length)){
                    if(uc_ctl_remove_cache(uc,current.address,current.address+current.length)!=UC_ERR_OK)return false;
                    ++counters.source_changed;std::memcpy(m.source.data(),current.host,current.length);
                }
            }
            m.used=false;++at;
        }return true;
    }
    bool retire_interval(bool retain){
        size_t kept=0;
        for(auto at=mappings.end();at!=mappings.begin();){--at;auto &m=**at;
            if(retain&&revalidate&&m.used&&cacheable(m.view)&&kept<64){++kept;continue;}
            if(!unmap(m.view))return false;at=mappings.erase(at);
        }return true;
    }
    std::vector<KlA64Mapping> nativeMappings;
    uint64_t epoch=std::numeric_limits<uint64_t>::max(),pc=0;
    uint32_t instruction=0;
    KlA64Exit exit{};
    unsigned executed=0,budget=1;bool interval=false,owned=false,wrote=false;uint64_t ownedBegin=0,ownedEnd=0;
    uint64_t writtenFirst=UINT64_MAX,writtenLast=0;
    bool private_data(uint64_t address,size_t length){
        if(!owned||!length)return false;
        const auto covered=[&](const KlA64Mapping &m){return address>=m.address&&address-m.address<m.length&&length<=m.length-(address-m.address);};
        const auto admitted=[&](const KlA64Mapping &m){return (m.permissions&7)==3&&(m.flags&KL_A64_MAPPING_PRIVATE_DATA);};
        for(const auto &m:nativeMappings)if(covered(m))return admitted(m);
        for(const auto &m:mappings)if(covered(m->view)){m->used=true;return admitted(m->view);}
        // The write hook can precede Unicorn's first mapping callback. Admit
        // the exact object once under the caller's VM/transaction lease.
        KlA64Mapping m{};if(config.map(config.owner,address,length,2,&m)||!m.host||!covered(m))return false;
        try{nativeMappings.push_back(m);}catch(const std::bad_alloc&){return false;}
        return admitted(m);
    }
    bool owned_execution(uint64_t address)const{
        for(const auto &mapping:mappings){const auto &m=mapping->view;
            if((m.permissions&4)&&(m.flags&KL_A64_MAPPING_EXECUTION_OWNER)&&address>=m.address&&address-m.address<m.length&&m.length-(address-m.address)>=4)return true;
        }
        return false;
    }
    void dirty(uint64_t address,size_t length){
        if(!length||private_data(address,length))return;
        wrote=true;writtenFirst=std::min(writtenFirst,address);writtenLast=std::max(writtenLast,address+uint64_t(length));
    }
    explicit KlA64Precise(KlA64PreciseConfig value):config(value){}
    ~KlA64Precise(){if(initial)uc_context_free(initial);if(uc)uc_close(uc);}
    static int reg(unsigned i){return i<29?UC_ARM64_REG_X0+int(i):i==29?UC_ARM64_REG_X29:UC_ARM64_REG_X30;}
    void fail(unsigned kind,uint64_t address,unsigned detail,int error){
        if(exit.kind==KL_A64_BUDGET)exit={kind,detail,address,error};
        uc_emu_stop(uc);
    }
    static bool map(uc_engine *,uc_mem_type type,uint64_t address,int length,int64_t,void *opaque){
        auto &cpu=*(KlA64Precise*)opaque;unsigned access;
        switch(type){case UC_MEM_FETCH_UNMAPPED:case UC_MEM_FETCH_PROT:access=4;break;case UC_MEM_WRITE_UNMAPPED:case UC_MEM_WRITE_PROT:access=2;break;default:access=1;}
        try{
        KlA64Mapping mapping{};bool admitted=false;
        // A first write's observer may already have admitted this exact object.
        // Reuse only this interval's retained view; public return clears it.
        for(const auto &known:cpu.nativeMappings)if(address>=known.address&&address-known.address<known.length&&(known.permissions&access)==access){mapping=known;admitted=true;break;}
        int error=admitted?0:cpu.config.map(cpu.config.owner,address,size_t(length),access,&mapping);
        if(error||!mapping.host||!mapping.length||(mapping.address&4095)||(mapping.length&4095)||
           mapping.address>address||address-mapping.address>=mapping.length||
           (mapping.permissions&access)!=access){cpu.fail(KL_A64_MEMORY,address,access,error?error:-14);return false;}
        cpu.mappings.reserve(cpu.mappings.size()+1);
        auto retained=std::make_unique<Mapped>();retained->view=mapping;
        if(cpu.revalidate&&cpu.owned&&executable_cacheable(mapping))retained->source.assign((unsigned char*)mapping.host,(unsigned char*)mapping.host+mapping.length);
        auto result=mapping.flags&(KL_A64_MAPPING_JIT|KL_A64_MAPPING_IO)?uc_mmio_map(cpu.uc,mapping.address,mapping.length,io_read,retained.get(),io_write,retained.get()):uc_mem_map_ptr(cpu.uc,mapping.address,mapping.length,mapping.permissions,mapping.host);
        if(!result&&(mapping.flags&(KL_A64_MAPPING_JIT|KL_A64_MAPPING_IO))){result=uc_mem_protect(cpu.uc,mapping.address,mapping.length,mapping.permissions);if(result)uc_mem_unmap(cpu.uc,mapping.address,mapping.length);}
        if(result){cpu.fail(KL_A64_MEMORY,address,access,-14);return false;}
        ++cpu.counters.mapped;cpu.mappings.push_back(std::move(retained));return true;
        }catch(const std::bad_alloc&){cpu.fail(KL_A64_MEMORY,address,access,-12);return false;}
    }
    static void *atomic(void *opaque,uint64_t address,size_t length,unsigned access){
        auto &cpu=*(KlA64Precise*)opaque;
        // Private original-decoder metadata distinguishes an attempted STXR
        // from an actual LSE write; only the helper can report its success.
        constexpr unsigned exclusiveStoreAttempt=0x10000u;
        const bool exclusive=(access&exclusiveStoreAttempt)!=0;access&=7;
        if(address&(length-1)){cpu.fail(KL_A64_MEMORY,address,access,-5);return nullptr;}
        const KlA64Mapping *known=nullptr;
        for(const auto &mapping:cpu.nativeMappings)if(address>=mapping.address&&address-mapping.address<mapping.length&&length<=mapping.length-(address-mapping.address)){known=&mapping;break;}
        if(!known){
            KlA64Mapping mapping{};int error=cpu.config.map(cpu.config.owner,address,length,access,&mapping);
            if(error||!mapping.host||address<mapping.address||address-mapping.address>=mapping.length||length>mapping.length-(address-mapping.address)){cpu.fail(KL_A64_MEMORY,address,access,error?error:-14);return nullptr;}
            try{cpu.nativeMappings.push_back(mapping);}catch(const std::bad_alloc&){cpu.fail(KL_A64_MEMORY,address,access,-12);return nullptr;}
            known=&cpu.nativeMappings.back();
        }
        if((known->permissions&access)!=access){cpu.fail(KL_A64_MEMORY,address,access,-14);return nullptr;}
        // No owner callback, mutex, allocation or Mach query may replace the
        // real reservation between its native load and store. The VM epoch
        // keeps this admitted object stable for the whole bounded interval.
        if(access&2){if(!exclusive)cpu.dirty(address,length);if(known->flags&KL_A64_MAPPING_JIT)pthread_jit_write_protect_np(0);}
        return (char*)known->host+(address-known->address);
    }
    static void native_written(void *opaque,uint64_t address,size_t length){
        ((KlA64Precise*)opaque)->dirty(address,length);
    }
    static bool copy(KlA64Precise &cpu,KlA64Mapping &mapping,uint64_t offset,void *buffer,unsigned length,bool writing){
        if(length>8||offset>mapping.length||length>mapping.length-offset){cpu.fail(KL_A64_MEMORY,mapping.address+offset,writing?2:1,-14);return false;}
        CopyScope scope{};scope.host=(uint64_t)mapping.host+offset;scope.guest=mapping.address+offset;scope.length=length;scope.previous=active_copy;
        active_copy=&scope;
        if(!sigsetjmp(scope.jump,0)){
            if(writing&&(mapping.flags&KL_A64_MAPPING_JIT))pthread_jit_write_protect_np(0);
            if(writing)std::memcpy((void*)scope.host,buffer,length);else std::memcpy(buffer,(void*)scope.host,length);
        }
        active_copy=scope.previous;
        if(writing&&(mapping.flags&KL_A64_MAPPING_JIT))pthread_jit_write_protect_np(1);
        if(scope.error){cpu.fail(KL_A64_MEMORY,scope.guest,writing?2:1,scope.error);return false;}return true;
    }
    static uint64_t io_read(uc_engine *,uint64_t offset,unsigned length,void *opaque){
        auto &mapping=*(Mapped*)opaque;mapping.used=true;uint64_t value=0;
        copy(*active_cpu,mapping.view,offset,&value,length,false);return value;
    }
    static void io_write(uc_engine *,uint64_t offset,unsigned length,uint64_t value,void *opaque){
        auto &mapping=*(Mapped*)opaque;mapping.used=true;copy(*active_cpu,mapping.view,offset,&value,length,true);
    }
    static void write(uc_engine *,uc_mem_type,uint64_t address,int length,int64_t,void *opaque){
        auto &cpu=*(KlA64Precise*)opaque;if(length>0)cpu.dirty(address,size_t(length));
    }
    static void code(uc_engine *,uint64_t address,uint32_t,void *opaque){
        auto &cpu=*(KlA64Precise*)opaque;
        if(cpu.executed&&(cpu.wrote||cpu.executed>=4096||!cpu.interval||
           (!uc_native_exclusive_active(cpu.uc)&&!(cpu.owned&&uc_native_exclusive_retry(cpu.uc))&&(!cpu.owned||cpu.executed>=cpu.budget||((address<cpu.ownedBegin||address>=cpu.ownedEnd)&&!cpu.owned_execution(address)))))){uc_emu_stop(cpu.uc);return;}
        // Instruction bytes are observed once after the run, outside the native
        // reservation window. uc_mem_read may enter IO/TCG internals.
        if(cpu.revalidate&&cpu.owned&&(address&~4095ull)!=cpu.lastPage){
            cpu.lastPage=address&~4095ull;
            for(const auto &m:cpu.mappings)if(address>=m->view.address&&address-m->view.address<m->view.length){m->used=true;break;}
        }
        ++cpu.executed;cpu.pc=address;
    }
    static void interrupt(uc_engine *,uint32_t number,void *opaque){
        auto &cpu=*(KlA64Precise*)opaque;
        uc_mem_read(cpu.uc,cpu.pc,&cpu.instruction,4);
        if(number==2){uint32_t instruction=0;if(uc_mem_read(cpu.uc,cpu.pc,&instruction,4)!=UC_ERR_OK){cpu.fail(KL_A64_MEMORY,cpu.pc,4,-14);return;}
            cpu.fail(KL_A64_SVC,0,(instruction>>5)&0xffff,0);
        }else cpu.fail(KL_A64_EXCEPTION,cpu.pc,number,0);
    }
    static uint32_t mrs(uc_engine *uc,uc_arm64_reg reg,const uc_arm64_cp_reg *cp,void *opaque){
        auto &cpu=*(KlA64Precise*)opaque;
        if(cp->op0!=3||cp->op1!=3)return 0;
        uint64_t value;
        if(cp->crn==0&&cp->crm==0&&cp->op2==1)value=cpu.config.ctr_el0;
        else if(cp->crn==14&&cp->crm==0&&cp->op2==0)value=cpu.config.cntfrq_el0;
        else if(cp->crn==14&&cp->crm==0&&(cp->op2==1||cp->op2==2)&&cpu.config.counter)value=cpu.config.counter(cpu.config.owner);
        else return 0;
        if(uc_reg_write(uc,reg,&value)!=UC_ERR_OK)cpu.fail(KL_A64_UNSUPPORTED,cpu.pc,0,-5);
        return 1;
    }
    bool transfer(KlA64State &s,bool to_cpu){
        auto move=[&](int reg,void *value){return (to_cpu?uc_reg_write(uc,reg,value):uc_reg_read(uc,reg,value))==UC_ERR_OK;};
        for(unsigned i=0;i<31;i++)if(!move(reg(i),&s.x[i]))return false;
        for(unsigned i=0;i<32;i++)if(!move(UC_ARM64_REG_Q0+int(i),s.v[i]))return false;
        uint64_t nzcv=s.pstate;
        if(!move(UC_ARM64_REG_SP,&s.sp)||!move(UC_ARM64_REG_PC,&s.pc)||!move(UC_ARM64_REG_NZCV,&nzcv)||
           !move(UC_ARM64_REG_FPCR,&s.fpcr)||!move(UC_ARM64_REG_FPSR,&s.fpsr)||
           !move(UC_ARM64_REG_TPIDR_EL0,&s.tpidr_el0)||!move(UC_ARM64_REG_TPIDRRO_EL0,&s.tpidrro_el0))return false;
        s.pstate=uint32_t(nzcv);return true;
    }
};
extern "C" KlA64Precise *kl_a64_precise_create(KlA64PreciseConfig config){
    if(!config.map)return nullptr;
    try{
        auto cpu=std::make_unique<KlA64Precise>(config);
        if(uc_open(UC_ARCH_ARM64,UC_MODE_ARM,&cpu->uc)!=UC_ERR_OK||uc_ctl_set_cpu_model(cpu->uc,UC_CPU_ARM64_MAX)!=UC_ERR_OK)return nullptr;
        if(uc_native_set_memory_owner(cpu->uc,KlA64Precise::atomic,cpu.get())!=UC_ERR_OK||
           uc_native_set_written(cpu->uc,KlA64Precise::native_written)!=UC_ERR_OK)return nullptr;
        uint64_t pstate=0;if(uc_reg_write(cpu->uc,UC_ARM64_REG_PSTATE,&pstate)!=UC_ERR_OK)return nullptr;
        // Linux/Darwin A64 userspace retains tagged pointer values while the
        // original CPU removes the top byte during address translation.
        uc_arm64_cp_reg addressControl{2,0,3,0,2,0};
        if(uc_reg_read(cpu->uc,UC_ARM64_REG_CP_REG,&addressControl)!=UC_ERR_OK)return nullptr;
        addressControl.val|=1ull<<37;
        if(uc_reg_write(cpu->uc,UC_ARM64_REG_CP_REG,&addressControl)!=UC_ERR_OK)return nullptr;
        // Keep the original CPU's EL0 cache-zero permission and advertised
        // block geometry consistent. DC ZVA executes QEMU's complete helper;
        // the MRS hook must not advertise access denied by its real SCTLR.
        uc_arm64_cp_reg control{1,0,3,0,0,0},zero{0,0,3,3,7,0};
        if(uc_reg_read(cpu->uc,UC_ARM64_REG_CP_REG,&control)!=UC_ERR_OK)return nullptr;
        // Linux userspace permits VA-based cache maintenance. Keep the
        // original decoder's EL0 access check enabled; privileged whole-cache
        // operations retain their original exception behavior. Source writes
        // and executable generations remain governed by the memory owner.
        constexpr uint64_t dze=1ull<<14,uci=1ull<<26;
        control.val=(control.val&~dze)|((config.dczid_el0&16)?0:dze)|uci;
        if(uc_reg_write(cpu->uc,UC_ARM64_REG_CP_REG,&control)!=UC_ERR_OK||
           uc_reg_read(cpu->uc,UC_ARM64_REG_CP_REG,&zero)!=UC_ERR_OK)return nullptr;
        if(zero.val!=config.dczid_el0){errno=EINVAL;return nullptr;}
        uc_hook mapping,interrupt,mrs,writes,code;
        if(uc_hook_add(cpu->uc,&mapping,UC_HOOK_MEM_INVALID,(void*)KlA64Precise::map,cpu.get(),1,0)!=UC_ERR_OK||
           uc_hook_add(cpu->uc,&interrupt,UC_HOOK_INTR,(void*)KlA64Precise::interrupt,cpu.get(),1,0)!=UC_ERR_OK||
           uc_hook_add(cpu->uc,&mrs,UC_HOOK_INSN,(void*)KlA64Precise::mrs,cpu.get(),1,0,UC_ARM64_INS_MRS)!=UC_ERR_OK||
           uc_hook_add(cpu->uc,&writes,UC_HOOK_MEM_WRITE,(void*)KlA64Precise::write,cpu.get(),1,0)!=UC_ERR_OK||
           uc_hook_add(cpu->uc,&code,UC_HOOK_CODE,(void*)KlA64Precise::code,cpu.get(),1,0)!=UC_ERR_OK||
           uc_context_alloc(cpu->uc,&cpu->initial)!=UC_ERR_OK||uc_context_save(cpu->uc,cpu->initial)!=UC_ERR_OK)return nullptr;
        return cpu.release();
    }catch(...){return nullptr;}
}
extern "C" void kl_a64_precise_destroy(KlA64Precise *cpu){delete cpu;}
extern "C" int kl_a64_precise_set_owned_cache(KlA64Precise *cpu,KlA64Revalidate callback){
    if(!cpu||!cpu->retire_all())return -14;cpu->revalidate=callback;return 0;
}
extern "C" KlA64PreciseCounters kl_a64_precise_counters(const KlA64Precise *cpu){return cpu?cpu->counters:KlA64PreciseCounters{};}
static KlA64Exit execute(KlA64Precise *cpu,KlA64State *state,uint64_t epoch,bool interval,bool owned=false,uint64_t begin=0,uint64_t end=0,unsigned budget=1){
    try{
        cpu->exit={};cpu->pc=state->pc;cpu->instruction=0;cpu->executed=0;cpu->interval=interval;cpu->owned=owned;cpu->ownedBegin=begin;cpu->ownedEnd=end;cpu->budget=budget;cpu->wrote=false;cpu->writtenFirst=UINT64_MAX;cpu->writtenLast=0;cpu->nativeMappings.clear();cpu->lastPage=UINT64_MAX;
        uc_native_exclusive_clear(cpu->uc);
        // Every retained UC table is inert until all of its source views have
        // been re-acquired in the new caller epoch. Other mappings never cache.
        if(!cpu->admit_cache(owned))return {KL_A64_MEMORY,4,state->pc,-14};
        cpu->epoch=epoch;
        if(!cpu->transfer(*state,true))return {KL_A64_UNSUPPORTED,0,state->pc,-5};
        if(!(owned&&cpu->revalidate)&&uc_ctl_remove_cache(cpu->uc,state->pc,state->pc+4)!=UC_ERR_OK)return {KL_A64_UNSUPPORTED,0,state->pc,-5};
        auto *previous=active_cpu;active_cpu=cpu;
        const auto error=uc_emu_start(cpu->uc,state->pc,0,0,0);
        uc_native_exclusive_clear(cpu->uc);
        uc_mem_read(cpu->uc,cpu->pc,&cpu->instruction,4);active_cpu=previous;
        if(cpu->wrote&&cpu->config.write)cpu->config.write(cpu->config.owner,cpu->writtenFirst,size_t(cpu->writtenLast-cpu->writtenFirst));
        cpu->nativeMappings.clear();
        if(!cpu->transfer(*state,false))return {KL_A64_UNSUPPORTED,0,cpu->pc,-5};
        if(error&&cpu->exit.kind==KL_A64_BUDGET){
            cpu->exit={error==UC_ERR_INSN_INVALID?KL_A64_UNSUPPORTED:KL_A64_MEMORY,0,cpu->pc,error==UC_ERR_NOMEM?-12:-14};
        }
        // The owner drops all leases after this return. Only inert, bounded
        // original RX and non-executable IO table entries may remain; no UC access is permitted
        // until the next full admission. Architectural exits retire them; a source-store barrier still
        // returns before execution, then the next full byte check invalidates
        // any changed cached source (including writes through another alias).
        // Writable direct RAM, JIT and foreign executable maps still retire.
        if(!cpu->retire_interval(owned&&cpu->exit.kind==KL_A64_BUDGET))return {KL_A64_MEMORY,4,cpu->pc,-14};
        return cpu->exit;
    }catch(...){return {KL_A64_UNSUPPORTED,0,state->pc,-12};}
}
extern "C" KlA64Exit kl_a64_precise_step(KlA64Precise *cpu,KlA64State *state,uint64_t epoch){return execute(cpu,state,epoch,false);}
extern "C" KlA64Exit kl_a64_precise_run(KlA64Precise *cpu,KlA64State *state,uint64_t epoch){return execute(cpu,state,epoch,true);}
extern "C" KlA64Exit kl_a64_precise_run_owned(KlA64Precise *cpu,KlA64State *state,uint64_t epoch,uint64_t begin,uint64_t end,unsigned budget){
    if(!cpu||!state||begin>=end||state->pc<begin||state->pc>=end||budget==0||budget>4096)return {KL_A64_UNSUPPORTED,0,state?state->pc:0,-22};
    return execute(cpu,state,epoch,true,true,begin,end,budget);
}
extern "C" int kl_a64_precise_reset(KlA64Precise *cpu){uc_native_exclusive_clear(cpu->uc);return uc_context_restore(cpu->uc,cpu->initial)==UC_ERR_OK?0:-5;}
extern "C" int kl_a64_precise_clear_exclusive(KlA64Precise *cpu){uc_native_exclusive_clear(cpu->uc);if(!cpu->retire_all())return -14;return uc_context_restore(cpu->uc,cpu->initial)==UC_ERR_OK?0:-5;}

extern "C" uint32_t kl_a64_precise_instruction(const KlA64Precise *cpu){return cpu->instruction;}
extern "C" uint64_t kl_a64_precise_instruction_address(const KlA64Precise *cpu){return cpu->pc;}

extern "C" int kl_a64_precise_native_fault(int signal,int code,uint64_t address,const void *mask){
    if((signal!=SIGBUS&&signal!=SIGSEGV)||code<=0||code>=0x10000)return 0;
    if(auto *scope=active_copy;scope&&address>=scope->host&&address-scope->host<scope->length){
        scope->error=signal==SIGBUS?-5:-14;
        if(mask)sigprocmask(SIG_SETMASK,(const sigset_t*)mask,nullptr);
        siglongjmp(scope->jump,1);
    }
    if(auto *cpu=active_cpu){
        auto previous=cpu->exit;cpu->exit={KL_A64_MEMORY,3,address,signal==SIGBUS?-5:-14};
        uc_native_recover_memory_fault(cpu->uc,address,&cpu->exit.address,mask);cpu->exit=previous;
    }
    return 0;
}
