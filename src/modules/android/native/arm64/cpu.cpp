#include "cpu.h"
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <atomic>

struct KlA64Cpu final : Dynarmic::A64::UserCallbacks {
    KlA64Memory memory;
    Dynarmic::ExclusiveMonitor monitor{1};
    std::unique_ptr<Dynarmic::A64::Jit> cpu;
    KlA64Exit exit{};
    uint64_t ticks=0, tls=0, tls_readonly=0;

    explicit KlA64Cpu(KlA64Config supplied):memory(supplied.memory) {
        Dynarmic::A64::UserConfig config{};
        config.callbacks=this;
        config.tpidr_el0=&tls;
        config.tpidrro_el0=&tls_readonly;
        config.ctr_el0=supplied.ctr_el0;
        config.cntfrq_el0=supplied.cntfrq_el0;
        config.dczid_el0=supplied.dczid_el0;
        config.global_monitor=&monitor;
        config.check_halt_on_memory_access=true;
        config.code_cache_size=8*1024*1024;
        if(supplied.direct_memory) {
            config.fastmem_pointer=uintptr_t(0);
            config.fastmem_address_space_bits=48;
            config.silently_mirror_fastmem=false;
        }
        cpu=std::make_unique<Dynarmic::A64::Jit>(config);
    }
    void fail(unsigned kind,uint64_t address,unsigned detail=0,int error=0) {
        if(exit.kind!=KL_A64_BUDGET)return;
        exit={kind,detail,address,error};
        cpu->HaltExecution(kind==KL_A64_MEMORY?Dynarmic::HaltReason::MemoryAbort:Dynarmic::HaltReason::UserDefined1);
    }
    template<class T>T read(uint64_t address) {
        T value{};const int error=memory.read(memory.owner,address,&value,sizeof value,0);
        if(error)fail(KL_A64_MEMORY,address,0,error);
        return value;
    }
    template<class T>void write(uint64_t address,T value) {
        const int error=memory.write(memory.owner,address,&value,sizeof value);
        if(error)fail(KL_A64_MEMORY,address,1,error);
    }
    template<class T>bool exclusive(uint64_t address,T value,T expected) {
        if(!memory.exclusive){fail(KL_A64_UNSUPPORTED,address);return false;}
        const int result=memory.exclusive(memory.owner,address,&value,&expected,sizeof value);
        if(result<0)fail(KL_A64_MEMORY,address,1,result);
        return result>0;
    }
    std::optional<uint32_t> MemoryReadCode(uint64_t address) override {
        uint32_t word=0;
        const int error=memory.read(memory.owner,address,&word,4,1);
        if(error){fail(KL_A64_MEMORY,address,2,error);return std::nullopt;}
        return word;
    }
    uint8_t MemoryRead8(uint64_t a)override{return read<uint8_t>(a);}
    uint16_t MemoryRead16(uint64_t a)override{return read<uint16_t>(a);}
    uint32_t MemoryRead32(uint64_t a)override{return read<uint32_t>(a);}
    uint64_t MemoryRead64(uint64_t a)override{return read<uint64_t>(a);}
    Dynarmic::A64::Vector MemoryRead128(uint64_t a)override{return read<Dynarmic::A64::Vector>(a);}
    void MemoryWrite8(uint64_t a,uint8_t v)override{write(a,v);}
    void MemoryWrite16(uint64_t a,uint16_t v)override{write(a,v);}
    void MemoryWrite32(uint64_t a,uint32_t v)override{write(a,v);}
    void MemoryWrite64(uint64_t a,uint64_t v)override{write(a,v);}
    void MemoryWrite128(uint64_t a,Dynarmic::A64::Vector v)override{write(a,v);}
    bool MemoryWriteExclusive8(uint64_t a,uint8_t v,uint8_t e)override{return exclusive(a,v,e);}
    bool MemoryWriteExclusive16(uint64_t a,uint16_t v,uint16_t e)override{return exclusive(a,v,e);}
    bool MemoryWriteExclusive32(uint64_t a,uint32_t v,uint32_t e)override{return exclusive(a,v,e);}
    bool MemoryWriteExclusive64(uint64_t a,uint64_t v,uint64_t e)override{return exclusive(a,v,e);}
    bool MemoryWriteExclusive128(uint64_t a,Dynarmic::A64::Vector v,Dynarmic::A64::Vector e)override{return exclusive(a,v,e);}
    void InterpreterFallback(uint64_t pc,size_t count)override{cpu->SetPC(pc);fail(KL_A64_UNSUPPORTED,pc,unsigned(count));}
    void CallSVC(uint32_t number)override{fail(KL_A64_SVC,0,number);}
    void ExceptionRaised(uint64_t pc,Dynarmic::A64::Exception e)override{fail(KL_A64_EXCEPTION,pc,unsigned(e));}
    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op,uint64_t value)override{fail(KL_A64_INSTRUCTION_CACHE,value,unsigned(op));}
    void AddTicks(uint64_t count)override{ticks=count>=ticks?0:ticks-count;}
    uint64_t GetTicksRemaining()override{return std::min(ticks,uint64_t(INT64_MAX));}
    uint64_t GetCNTPCT()override{if(memory.counter)return memory.counter(memory.owner);fail(KL_A64_UNSUPPORTED,cpu->GetPC());return 0;}

    void import(const KlA64State &state) {
        std::array<uint64_t,31> registers;std::memcpy(registers.data(),state.x,sizeof state.x);
        cpu->SetRegisters(registers);cpu->SetSP(state.sp);cpu->SetPC(state.pc);
        for(unsigned i=0;i<32;i++)cpu->SetVector(i,{state.v[i][0],state.v[i][1]});
        cpu->SetPstate(state.pstate);cpu->SetFpcr(state.fpcr);cpu->SetFpsr(state.fpsr);tls=state.tpidr_el0;tls_readonly=state.tpidrro_el0;
    }
    void export_to(KlA64State &state) {
        const auto registers=cpu->GetRegisters();std::memcpy(state.x,registers.data(),sizeof state.x);
        state.sp=cpu->GetSP();state.pc=cpu->GetPC();
        for(unsigned i=0;i<32;i++){const auto vector=cpu->GetVector(i);state.v[i][0]=vector[0];state.v[i][1]=vector[1];}
        state.pstate=cpu->GetPstate();state.fpcr=cpu->GetFpcr();state.fpsr=cpu->GetFpsr();state.tpidr_el0=tls;
    }
};

extern "C" KlA64Cpu *kl_a64_create(KlA64Config config) {
    if(!config.memory.read||!config.memory.write)return nullptr;
    try{return new KlA64Cpu(config);}catch(...){return nullptr;}
}
extern "C" void kl_a64_destroy(KlA64Cpu *cpu){delete cpu;}
extern "C" KlA64Exit kl_a64_run(KlA64Cpu *cpu,KlA64State *state,uint64_t budget) {
    cpu->import(*state);cpu->ticks=budget;cpu->exit={};
    cpu->cpu->ClearHalt(Dynarmic::HaltReason::UserDefined1|Dynarmic::HaltReason::MemoryAbort);
    if(budget&&Dynarmic::Has(cpu->cpu->Run(),Dynarmic::HaltReason::UserDefined2)&&cpu->exit.kind==KL_A64_BUDGET)cpu->exit.kind=KL_A64_STOP;
    cpu->export_to(*state);return cpu->exit;
}
extern "C" void kl_a64_invalidate(KlA64Cpu *cpu,uint64_t address,size_t length){if(length)cpu->cpu->InvalidateCacheRange(address,length);}
extern "C" void kl_a64_clear_cache(KlA64Cpu *cpu){cpu->cpu->ClearCache();}
extern "C" void kl_a64_stop(KlA64Cpu *cpu){cpu->cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2);}
template<class T>static int compare_exchange(uint64_t address,const void *value,const void *original) {
    T desired,expected;std::memcpy(&desired,value,sizeof desired);std::memcpy(&expected,original,sizeof expected);
    return std::atomic_ref(*reinterpret_cast<T*>(address)).compare_exchange_strong(expected,desired);
}
extern "C" int kl_a64_compare_exchange(uint64_t address,const void *value,const void *expected,size_t size) {
    if(!size||address%size)return -1;
    switch(size){case 1:return compare_exchange<uint8_t>(address,value,expected);case 2:return compare_exchange<uint16_t>(address,value,expected);case 4:return compare_exchange<uint32_t>(address,value,expected);case 8:return compare_exchange<uint64_t>(address,value,expected);case 16:return compare_exchange<__uint128_t>(address,value,expected);default:return -1;}
}
