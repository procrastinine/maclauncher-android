#include "native-writer.h"
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <new>
#include <pthread.h>
#include <unistd.h>
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
namespace {
uint64_t native_tls(){uint64_t value;asm volatile("mrs %0,tpidrro_el0":"=r"(value));return value;}
uint64_t native_tp(){uint64_t value;asm volatile("mrs %0,tpidr_el0":"=r"(value));return value;}
bool transfer(int descriptor,unsigned char &value,bool writing){
 ssize_t count;do{count=writing?write(descriptor,&value,1):read(descriptor,&value,1);}while(count<0&&errno==EINTR);
 return count==1;
}
KlA64State capture(const __darwin_mcontext64 &frame){
 KlA64State state{};for(unsigned n=0;n<29;n++)state.x[n]=frame.__ss.__x[n];
 state.x[29]=__darwin_arm_thread_state64_get_fp(frame.__ss);state.x[30]=__darwin_arm_thread_state64_get_lr(frame.__ss);
 state.sp=__darwin_arm_thread_state64_get_sp(frame.__ss);state.pc=__darwin_arm_thread_state64_get_pc(frame.__ss);
 for(unsigned n=0;n<32;n++){state.v[n][0]=(uint64_t)frame.__ns.__v[n];state.v[n][1]=(uint64_t)(frame.__ns.__v[n]>>64);}
 state.tpidr_el0=native_tp();state.tpidrro_el0=native_tls();state.pstate=frame.__ss.__cpsr&0xf0000000u;state.fpcr=frame.__ns.__fpcr;state.fpsr=frame.__ns.__fpsr;return state;
}
void restore(__darwin_mcontext64 &frame,const KlA64State &state){
 for(unsigned n=0;n<29;n++)frame.__ss.__x[n]=state.x[n];
 __darwin_arm_thread_state64_set_fp(frame.__ss,state.x[29]);
 __darwin_arm_thread_state64_set_lr_fptr(frame.__ss,(void(*)())state.x[30]);
 __darwin_arm_thread_state64_set_sp(frame.__ss,state.sp);
 __darwin_arm_thread_state64_set_pc_fptr(frame.__ss,(void(*)())state.pc);
 for(unsigned n=0;n<32;n++)frame.__ns.__v[n]=(__uint128_t)state.v[n][0]|((__uint128_t)state.v[n][1]<<64);
 frame.__ss.__cpsr=(frame.__ss.__cpsr&~0xf0000000u)|(state.pstate&0xf0000000u);frame.__ns.__fpcr=state.fpcr;frame.__ns.__fpsr=state.fpsr;
}
}
struct KlNativeWriter {
 struct Slot {KlA64State state{};uint64_t address=0;int result=0;std::atomic<unsigned> ready{0};};
 const pid_t process=getpid();KlNativeWriterConfig config;Slot slots[KL_NATIVE_WRITER_SLOTS];pthread_t helper{};std::atomic<uint64_t> helperTls{0};
 explicit KlNativeWriter(KlNativeWriterConfig value):config(value){}
 static void *work(void *opaque){
  auto &owner=*(KlNativeWriter*)opaque;owner.helperTls.store(native_tls(),std::memory_order_release);
  sigset_t faults;sigemptyset(&faults);sigaddset(&faults,SIGBUS);sigaddset(&faults,SIGSEGV);pthread_sigmask(SIG_UNBLOCK,&faults,nullptr);
  for(;;){
   unsigned char index;if(!transfer(owner.config.requests[0],index,false))std::abort();if(index==255)return nullptr;if(index>=KL_NATIVE_WRITER_SLOTS)std::abort();
   auto &slot=owner.slots[index];if(slot.ready.load(std::memory_order_acquire)!=1)std::abort();
   slot.result=owner.config.execute(owner.config.owner,&slot.state,slot.address);
   slot.ready.store(2,std::memory_order_release);
   if(!transfer(owner.config.completion[index][1],index,true))std::abort();
  }
 }
};
extern "C" KlNativeWriter *kl_native_writer_create(KlNativeWriterConfig config){
 if(!config.execute){errno=EINVAL;return nullptr;}
 auto *owner=new(std::nothrow)KlNativeWriter(config);if(!owner){errno=ENOMEM;return nullptr;}
 for(unsigned char n=0;n<KL_NATIVE_WRITER_SLOTS;n++)if(!transfer(config.free_slots[1],n,true)){delete owner;return nullptr;}
 int error=pthread_create(&owner->helper,nullptr,KlNativeWriter::work,owner);if(error){delete owner;errno=error;return nullptr;}return owner;
}
extern "C" void kl_native_writer_destroy(KlNativeWriter *owner){
 if(!owner)return;unsigned char quit=255;if(!transfer(owner->config.requests[1],quit,true))std::abort();
 if(pthread_join(owner->helper,nullptr))std::abort();delete owner;
}
extern "C" int kl_native_writer_fault(KlNativeWriter *owner,int signal,const siginfo_t *info,void *opaque){
 if(!owner||owner->process!=getpid()||!info||!opaque||(signal!=SIGBUS&&signal!=SIGSEGV)||info->si_code<=0||info->si_code>=0x10000||native_tls()==owner->helperTls.load(std::memory_order_acquire))return 0;
 auto &frame=*static_cast<ucontext_t*>(opaque)->uc_mcontext;const auto esr=frame.__es.__esr;
 if(((esr>>26)!=0x24&&(esr>>26)!=0x25)||!(esr&(1u<<6)))return 0;
 const int saved=errno;unsigned char index;
 if(!transfer(owner->config.free_slots[0],index,false)||index>=KL_NATIVE_WRITER_SLOTS){errno=saved;return 0;}
 auto &slot=owner->slots[index];slot.state=capture(frame);slot.address=(uintptr_t)info->si_addr;slot.result=0;slot.ready.store(1,std::memory_order_release);
 if(!transfer(owner->config.requests[1],index,true))std::abort();
 unsigned char completed;if(!transfer(owner->config.completion[index][0],completed,false)||completed!=index||slot.ready.load(std::memory_order_acquire)!=2)std::abort();
 const int handled=slot.result==0;if(handled)restore(frame,slot.state);
 slot.ready.store(0,std::memory_order_release);if(!transfer(owner->config.free_slots[1],index,true))std::abort();
 errno=saved;return handled;
}

extern "C" int kl_native_writer_region(uint64_t address,uint64_t *start,uint64_t *end,int *protection){
 unsigned depth=0;
 for(;;){
  mach_vm_address_t at=address;mach_vm_size_t length=0;vm_region_submap_info_data_64_t info{};mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
  const auto result=mach_vm_region_recurse(mach_task_self(),&at,&length,&depth,(vm_region_recurse_info_t)&info,&count);
  if(result!=KERN_SUCCESS||at>address||address-at>=length||length>UINT64_MAX-at)return -14;
  if(!info.is_submap){*start=at;*end=at+length;*protection=info.protection;return 0;}
  if(depth==UINT32_MAX)return -14;++depth;
 }
}
