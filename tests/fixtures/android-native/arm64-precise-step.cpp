#include "precise.h"
#include <array>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <sys/mman.h>
#include <pthread.h>
#include <csignal>
#include <unistd.h>
#include <sys/ucontext.h>
#define OP(name, text) asm(".text\n.p2align 2\n.globl _" #name "\n_" #name ":\n" text "\nret\n"); extern "C" const uint32_t name[];
OP(scalar,"str w9, [x10, x8, lsl #2]")
OP(tagged_store,"str w9, [x10, #16]!")
OP(tagged_load,"ldr w0, [x2]")
OP(vector_pair,"stp q0, q1, [x2], #32")
OP(vector_four,"st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x2], #64")
OP(x18_store,"str x18, [sp, #16]")
OP(load_exclusive,"ldxr x0, [x2]")
OP(load_plain,"ldr x0, [x2]")
OP(add_to_store,"add x1, x0, #1")
OP(load_pair,"ldxp x0, x1, [x4]")
OP(store_pair,"stxp w5, x2, x3, [x4]")
OP(store_exclusive,"stxr w3, x1, [x2]")
OP(clear_exclusive,"clrex")
OP(retry_byte,"uxtb w16, w0\n1: ldaxrb w0, [x2]\ncmp w0, w16\nb.ne 2f\nstxrb w17, w1, [x2]\ncbnz w17, 1b\n2: nop")
OP(retry_half,"uxth w16, w0\n1: ldaxrh w0, [x2]\ncmp w0, w16\nb.ne 2f\nstxrh w17, w1, [x2]\ncbnz w17, 1b\n2: nop")
OP(retry_word,"mov w16, w0\n1: ldaxr w0, [x2]\ncmp w0, w16\nb.ne 2f\nstxr w17, w1, [x2]\ncbnz w17, 1b\n2: nop")
OP(retry_double,"mov x16, x0\n1: ldaxr x0, [x2]\ncmp x0, x16\nb.ne 2f\nstxr w17, x1, [x2]\ncbnz w17, 1b\n2: nop")
OP(retry_pair,"mov x16, x0\nmov x17, x1\n1: ldaxp x0, x1, [x4]\ncmp x0, x16\nb.ne 2f\ncmp x1, x17\nb.ne 2f\nstxp w5, x2, x3, [x4]\ncbnz w5, 1b\n2: nop")
OP(atomic_cas,"casal x0, x1, [x2]")
OP(atomic_pair,"caspal x0, x1, x2, x3, [x4]")
OP(atomic_swap,"swpal x1, x0, [x2]")
OP(atomic_add,"ldaddal x1, x0, [x2]")
OP(atomic_smin,"ldsminal x1, x0, [x2]")
OP(atomic_smax,"ldsmaxal x1, x0, [x2]")
OP(atomic_umin,"lduminal x1, x0, [x2]")
OP(atomic_umax,"ldumaxal x1, x0, [x2]")
OP(original_svc,"svc #0")
OP(tls_read,"mrs x0, tpidr_el0")
OP(ctr_read,"mrs x0, ctr_el0")
OP(zero_read,"mrs x0, dczid_el0")
OP(zero_block,"dc zva, x3")
OP(privileged_read,"mrs x0, sctlr_el1")
OP(counter_read,"mrs x0, cntvct_el0")
OP(add_1,"add x0, x0, #1")
OP(add_9,"add x0, x0, #9")
OP(store_loop,"1: str x1, [x2]\nadd x1, x1, #1\nsubs x0, x0, #1\nb.ne 1b\nnop")
OP(genuine_break,"brk #0xa100")
struct Owner{
 static constexpr uint64_t code=0x10000000,data=0x20000000;
 alignas(4096)std::array<uint32_t,1024> instructions{};
 alignas(4096)std::array<uint32_t,1024> nextInstructions{},nativeInstructions{};
 unsigned nextFlags=0;
 alignas(4096)std::array<unsigned char,4096> bytes{};
 alignas(4096)std::array<unsigned char,4096> replacement{};
 bool readOnly=false,missing=false,replaced=false,jitData=false,jitCode=false,ioCode=false,privateData=false,aba=false,cacheRX=false,aliasCode=false;
 std::atomic<int> exchange{0};
 uint64_t dirtyFirst=0;size_t dirtyLength=0;unsigned mapCalls=0;
 void *jit=nullptr,*file=nullptr;
};
static int mapping(void *opaque,uint64_t address,size_t length,unsigned access,KlA64Mapping *out){
 auto &m=*(Owner*)opaque;++m.mapCalls;if(m.missing)return -14;
 if(address>=Owner::code&&address-Owner::code<4096&&length<=4096-(address-Owner::code)){
  if(m.cacheRX&&(access&2))return -11;
  *out={Owner::code,4096,m.jitCode?m.jit:m.instructions.data(),m.cacheRX?5u:7u,(m.jitCode?KL_A64_MAPPING_JIT:(m.ioCode?KL_A64_MAPPING_IO:0u))|(m.cacheRX?KL_A64_MAPPING_EXECUTION_OWNER:0u)};return 0;
 }
 if(address>=Owner::code+4096&&address-Owner::code-4096<4096&&length<=4096-(address-Owner::code-4096)){
  if(m.cacheRX&&(access&2))return -11;
  *out={Owner::code+4096,4096,m.nextInstructions.data(),m.cacheRX?5u:7u,m.nextFlags};return 0;
 }
 if(address>=Owner::code+8192&&address-Owner::code-8192<4096&&length<=4096-(address-Owner::code-8192)){
  *out={Owner::code+8192,4096,m.nativeInstructions.data(),5,0};return 0;
 }
 if(address>=Owner::data&&address-Owner::data<4096&&length<=4096-(address-Owner::data)&&access!=4){
  if(m.readOnly&&(access&2))return -11;
  *out={Owner::data,4096,m.jitData?m.jit:(m.file?m.file:(m.aliasCode?(void*)m.instructions.data():(m.replaced?m.replacement.data():m.bytes.data()))),m.readOnly?1u:3u,(m.jitData?KL_A64_MAPPING_JIT:(m.file?KL_A64_MAPPING_IO:0u))|(m.privateData?KL_A64_MAPPING_PRIVATE_DATA:0u)};return 0;
 }return -14;
}
static int revalidate(void *opaque,uint64_t address,size_t length,KlA64Mapping *out){return mapping(opaque,address,length,address>=Owner::data?3:4,out);}
static uint64_t counter(void *opaque){if(!opaque)return 0x123456789ull;auto &m=*(Owner*)opaque;if(m.aba){m.exchange.store(1);while(m.exchange.load()!=2){};}return 0x123456789ull;}
static void written(void*opaque,uint64_t address,size_t length){auto&m=*(Owner*)opaque;m.dirtyFirst=address;m.dirtyLength=length;}
static void require(bool yes,const char *why){if(!yes){fprintf(stderr,"FAIL %s\n",why);exit(1);}}
static KlA64State initial(){
 KlA64State s{};s.pc=Owner::code;s.sp=Owner::data;s.pstate=0xa0000000;s.fpcr=0x800000;s.fpsr=0x10;s.tpidr_el0=0x12345678;s.tpidrro_el0=0x87654321;
 for(unsigned i=0;i<31;i++)s.x[i]=0xfedc0000+i;
 for(unsigned i=0;i<32;i++){s.v[i][0]=0x12340000+i;s.v[i][1]=0xabc00000+i;}
 return s;
}
static KlA64Exit step(KlA64Precise *cpu,Owner &m,KlA64State &s,const uint32_t *op,uint64_t epoch=1){
 if(m.jitCode){pthread_jit_write_protect_np(0);((uint32_t*)m.jit)[0]=op[0];((uint32_t*)m.jit)[1]=add_9[0];pthread_jit_write_protect_np(1);}
 else{m.instructions[0]=op[0];m.instructions[1]=add_9[0];}s.pc=Owner::code;return kl_a64_precise_step(cpu,&s,epoch);
}
static void fault(int number,siginfo_t *info,void *context){
 if(!kl_a64_precise_native_fault(number,info->si_code,(uint64_t)info->si_addr,&((ucontext_t*)context)->uc_sigmask))_Exit(90);
}
int main(){
 struct sigaction handler{};handler.sa_sigaction=fault;handler.sa_flags=SA_SIGINFO;sigemptyset(&handler.sa_mask);
 require(!sigaction(SIGBUS,&handler,nullptr)&&!sigaction(SIGSEGV,&handler,nullptr),"native recovery signal owner");
 require(!kl_a64_precise_native_fault(SIGBUS,0,0,nullptr),"software signal is not native access");
 Owner m;KlA64PreciseConfig config{&m,mapping,counter,0x8444c004,1000000000,4,written};
 auto *cpu=kl_a64_precise_create(config);require(cpu,"original CPU creation");
 auto s=initial();s.x[8]=3;s.x[9]=0xd004da40;s.x[10]=Owner::data;
 auto out=step(cpu,m,s,scalar);uint32_t word;uint64_t value;memcpy(&word,m.bytes.data()+12,4);
 require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+4&&word==0xd004da40,"one original register-offset store");
 require(s.x[0]==0xfedc0000&&s.x[18]==0xfedc0012&&s.sp==Owner::data&&s.pstate==0xa0000000&&s.fpcr==0x800000&&s.fpsr==0x10,"next instruction and scalar status remain unchanged");
 for(unsigned i=0;i<32;i++)require(s.v[i][0]==0x12340000+i&&s.v[i][1]==0xabc00000+i,"all SIMD state retained");
 for(uint64_t tag:{0x0200000000000000ull,0xab00000000000000ull,0xff00000000000000ull}){
  m.bytes.fill(0);s=initial();s.x[9]=0x12345678;s.x[10]=tag|Owner::data;out=step(cpu,m,s,tagged_store);memcpy(&word,m.bytes.data()+16,4);
  require(out.kind==KL_A64_BUDGET&&word==0x12345678&&s.x[10]==(tag|Owner::data)+16,"original tagged store and writeback retain architectural pointer");
  s=initial();s.x[2]=tag|(Owner::data+16);out=step(cpu,m,s,tagged_load);require(out.kind==KL_A64_BUDGET&&s.x[0]==0x12345678&&s.x[2]==(tag|(Owner::data+16)),"original tagged read uses canonical owner without changing pointer");
  s=initial();s.x[2]=tag|Owner::data;s.x[1]=7;out=step(cpu,m,s,atomic_add);memcpy(&value,m.bytes.data(),8);
  require(out.kind==KL_A64_BUDGET&&s.x[0]==0&&value==7&&s.x[2]==(tag|Owner::data),"original tagged atomic uses canonical source-owner address");
  require(m.dirtyFirst==Owner::data&&m.dirtyLength==8,"tagged atomic publishes canonical write generation");
  m.bytes.fill(0);s=initial();s.x[2]=tag|Owner::data;out=step(cpu,m,s,vector_pair);require(out.kind==KL_A64_BUDGET&&s.x[2]==(tag|Owner::data)+32&&!memcmp(m.bytes.data(),s.v,32),"original tagged SIMD writeback retains pointer and bytes");
  s=initial();s.x[10]=tag|(Owner::code+496);s.x[9]=add_1[0];out=step(cpu,m,s,tagged_store);
  require(out.kind==KL_A64_BUDGET&&m.instructions[128]==add_1[0]&&m.dirtyFirst==Owner::code+512&&m.dirtyLength==4&&s.x[10]==(tag|(Owner::code+512)),"tagged source-code write publishes its exact canonical generation");
 }
 for(auto op:{vector_pair,vector_four}){
  m.bytes.fill(0);s=initial();s.x[2]=Owner::data;out=step(cpu,m,s,op);size_t n=op==vector_pair?32:64;
  require(out.kind==KL_A64_BUDGET&&s.x[2]==Owner::data+n&&!memcmp(m.bytes.data(),s.v,n),"original SIMD stores and writeback");
 }
 m.bytes.fill(0);s=initial();out=step(cpu,m,s,x18_store);memcpy(&value,m.bytes.data()+16,8);
 require(out.kind==KL_A64_BUDGET&&value==s.x[18]&&s.sp==Owner::data,"architectural x18 store");
 uint64_t old=7;memcpy(m.bytes.data(),&old,8);s=initial();s.x[2]=Owner::data;s.x[1]=11;
 require(step(cpu,m,s,load_exclusive).kind==KL_A64_BUDGET&&s.x[0]==7,"exclusive load");
 out=step(cpu,m,s,store_exclusive);memcpy(&value,m.bytes.data(),8);require(out.kind==KL_A64_BUDGET&&s.x[3]!=0&&value==7,"returning to the native owner loses the exclusive reservation");
 old=11;memcpy(m.bytes.data(),&old,8);
 // A native peer writes a different value and restores it between the pair.
 // Value-only compare/exchange incorrectly permits this exclusive store.
 s=initial();s.x[2]=Owner::data;s.x[1]=19;step(cpu,m,s,load_exclusive);
 std::thread aba([&]{auto &native=*(std::atomic<uint64_t>*)m.bytes.data();native.store(99);native.store(11);});aba.join();
 out=step(cpu,m,s,store_exclusive);memcpy(&value,m.bytes.data(),8);
 require(out.kind==KL_A64_BUDGET&&s.x[3]!=0&&value==11,"native ABA invalidates exclusive reservation");
 for(bool explicit_clear:{false,true}){
  s=initial();s.x[2]=Owner::data;s.x[1]=19;step(cpu,m,s,load_exclusive);
  if(explicit_clear)require(kl_a64_precise_clear_exclusive(cpu)==0,"native fault clears reservation");else require(step(cpu,m,s,clear_exclusive).kind==KL_A64_BUDGET,"original CLREX");
  out=step(cpu,m,s,store_exclusive);memcpy(&value,m.bytes.data(),8);require(out.kind==KL_A64_BUDGET&&s.x[3]!=0&&value==11,"cleared reservation never writes");
 }
 // A bounded call retains only the original exclusive interval. The
 // interpreter's public return is always a reservation-loss boundary.
 m.instructions[0]=load_exclusive[0];m.instructions[1]=add_to_store[0];m.instructions[2]=store_exclusive[0];
 bool progressed=false;
 for(unsigned retry=0;retry<1000&&!progressed;retry++){
  s=initial();s.x[2]=Owner::data;s.pc=Owner::code;out=kl_a64_precise_run(cpu,&s,1);
  require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+12,"exclusive interval ends before the following instruction");progressed=s.x[3]==0;
 }
 memcpy(&value,m.bytes.data(),8);require(progressed&&value==12,"real host exclusive retry loop makes progress");
 // Original libgcc compare/exchange retries cross a conditional TCG block
 // between their native exclusive load and store. Every supported width must
 // make progress through the real instruction sequence, including IO fetch.
 void *retryJit=mmap(nullptr,16384,7,MAP_PRIVATE|MAP_ANON|MAP_JIT,-1,0);require(retryJit!=MAP_FAILED,"exclusive JIT allocation");
 for(unsigned mode=0;mode<5;mode++)for(uint64_t tag:{0ull,0x0200000000000000ull}){
  m.ioCode=mode==2;m.jitCode=mode==3;m.jitData=mode==4;m.jit=retryJit;m.file=mode==1||mode==2||mode==3?m.bytes.data():nullptr;
  unsigned index=0;
  for(auto op:{retry_byte,retry_half,retry_word,retry_double,retry_pair}){
   const unsigned width=1u<<index++,words=width==16?10:7;
   if(m.jitCode){pthread_jit_write_protect_np(0);memcpy(m.jit,op,words*4);pthread_jit_write_protect_np(1);}else memcpy(m.instructions.data(),op,words*4);
   unsigned char *data=m.jitData?(unsigned char*)m.jit:m.bytes.data();if(m.jitData)pthread_jit_write_protect_np(0);memset(data,0,4096);if(m.jitData)pthread_jit_write_protect_np(1);
   s=initial();s.x[0]=0;s.x[1]=width==16?0:1;s.x[2]=width==16?1:(tag|Owner::data);s.x[3]=2;s.x[4]=tag|Owner::data;
   unsigned calls=0;const uint64_t end=Owner::code+(words-1)*4;
   while(s.pc!=end&&calls<8){out=kl_a64_precise_run_owned(cpu,&s,1,Owner::code,end,64);require(out.kind==KL_A64_BUDGET,"original conditional exclusive result");calls++;}
   uint64_t low=0,high=0;memcpy(&low,data,width<8?width:8);if(width==16)memcpy(&high,data+8,8);
   printf("exclusive-retry mode=%u width=%u calls=%u pc=%llx status=%llu value=%llu/%llu\n",mode,width,calls,(unsigned long long)s.pc,(unsigned long long)s.x[width==16?5:17],(unsigned long long)low,(unsigned long long)high);fflush(stdout);
   require(s.pc==end&&low==1&&(width!=16||high==2),"original conditional exclusive retry makes progress at every width");
   require(m.dirtyFirst==Owner::data&&m.dirtyLength==width,"successful exclusive store reports exactly its actual canonical extent");
   require(s.x[width==16?4:2]==(tag|Owner::data),"exclusive translation retains its architectural tagged base");
  }
 }
 m.ioCode=false;m.file=nullptr;m.jitCode=false;m.jitData=false;m.jit=nullptr;munmap(retryJit,16384);
 m.instructions[0]=load_exclusive[0];m.instructions[1]=add_to_store[0];m.instructions[2]=store_exclusive[0];old=12;memcpy(m.bytes.data(),&old,8);
 // Force an actual native peer ABA inside the same uninterrupted CPU call.
 m.instructions[1]=counter_read[0];m.aba=true;m.exchange=0;m.dirtyLength=0;
 std::thread during([&]{while(m.exchange.load()!=1){};auto &native=*(std::atomic<uint64_t>*)m.bytes.data();native.store(99);native.store(12);m.exchange.store(2);});
 s=initial();s.x[2]=Owner::data;s.x[1]=88;out=kl_a64_precise_run(cpu,&s,1);during.join();m.aba=false;
 memcpy(&value,m.bytes.data(),8);require(out.kind==KL_A64_BUDGET&&s.x[3]!=0&&value==12,"native ABA during an exclusive interval cannot report success");require(m.dirtyLength==0,"failed original STXR does not publish a write generation");
 uint64_t originals[]={12,13};memcpy(m.bytes.data(),originals,16);m.instructions[0]=load_pair[0];m.instructions[1]=store_pair[0];
 progressed=false;for(unsigned retry=0;retry<1000&&!progressed;retry++){s=initial();s.x[4]=Owner::data;s.x[2]=21;s.x[3]=22;out=kl_a64_precise_run(cpu,&s,1);require(out.kind==KL_A64_BUDGET,"pair interval");progressed=s.x[5]==0;}
 uint64_t pair_result[2];memcpy(pair_result,m.bytes.data(),16);require(progressed&&pair_result[0]==21&&pair_result[1]==22,"native paired exclusive retry loop makes progress");
 old=11;memcpy(m.bytes.data(),&old,8);
 s=initial();s.x[2]=Owner::data;s.x[0]=11;s.x[1]=23;
 out=step(cpu,m,s,atomic_cas);memcpy(&value,m.bytes.data(),8);require(out.kind==KL_A64_BUDGET&&s.x[0]==11&&value==23,"CASAL argument and result");
 s=initial();s.x[2]=Owner::data;s.x[1]=29;out=step(cpu,m,s,atomic_swap);memcpy(&value,m.bytes.data(),8);require(out.kind==KL_A64_BUDGET&&s.x[0]==23&&value==29,"SWPAL argument and result");
 s=initial();s.x[2]=Owner::data;s.x[1]=5;out=step(cpu,m,s,atomic_add);memcpy(&value,m.bytes.data(),8);require(out.kind==KL_A64_BUDGET&&s.x[0]==29&&value==34,"LDADDAL argument and result");
 uint64_t pair[]={34,35};memcpy(m.bytes.data(),pair,16);s=initial();s.x[0]=34;s.x[1]=35;s.x[2]=40;s.x[3]=41;s.x[4]=Owner::data;out=step(cpu,m,s,atomic_pair);memcpy(pair,m.bytes.data(),16);require(out.kind==KL_A64_BUDGET&&s.x[0]==34&&s.x[1]==35&&pair[0]==40&&pair[1]==41,"original pair compare/exchange");
 s=initial();require(step(cpu,m,s,tls_read).kind==KL_A64_BUDGET&&s.x[0]==s.tpidr_el0,"original TLS read");
 require(step(cpu,m,s,ctr_read).kind==KL_A64_BUDGET&&s.x[0]==config.ctr_el0,"existing guest cache geometry");
 require(step(cpu,m,s,zero_read).kind==KL_A64_BUDGET&&s.x[0]==4,"original DCZID agrees with enabled cache-zero access");
 m.bytes.fill(0xa5);m.dirtyLength=0;s=initial();s.x[3]=Owner::data+71;out=step(cpu,m,s,zero_block);
 require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+4&&s.x[3]==Owner::data+71,"original EL0 DC ZVA executes one instruction");
 require(m.dirtyFirst==Owner::data+64&&m.dirtyLength==64,"DC ZVA reports its complete original write extent");
 for(unsigned n=0;n<m.bytes.size();n++)require(m.bytes[n]==(n>=64&&n<128?0:0xa5),"DC ZVA zeros exactly its original aligned block");
 for(uint64_t tag:{0x0200000000000000ull,0xab00000000000000ull}){
  m.bytes.fill(0xa5);m.dirtyLength=0;s=initial();s.x[3]=tag|(Owner::data+71);out=step(cpu,m,s,zero_block);
  require(out.kind==KL_A64_BUDGET&&s.x[3]==(tag|(Owner::data+71)),"original tagged DC ZVA preserves operand register");
  require(m.dirtyFirst==Owner::data+64&&m.dirtyLength==64,"tagged DC ZVA publishes canonical original source extent");
  for(unsigned n=0;n<m.bytes.size();n++)require(m.bytes[n]==(n>=64&&n<128?0:0xa5),"tagged DC ZVA zeros only original aligned block");
 }
 s=initial();out=step(cpu,m,s,privileged_read);require(out.kind==KL_A64_EXCEPTION&&s.pc==Owner::code,"EL0 still denies privileged system-register access");
 auto disabled=config;disabled.dczid_el0=20;auto *deniedCpu=kl_a64_precise_create(disabled);require(deniedCpu,"original disabled cache-zero CPU");
 s=initial();require(step(deniedCpu,m,s,zero_read).kind==KL_A64_BUDGET&&s.x[0]==20,"original DCZID reports denied cache-zero access");
 m.bytes.fill(0xa5);s=initial();s.x[3]=Owner::data+71;out=step(deniedCpu,m,s,zero_block);
 require(out.kind==KL_A64_EXCEPTION&&s.pc==Owner::code,"denied DC ZVA preserves original exception PC");for(auto byte:m.bytes)require(byte==0xa5,"denied DC ZVA leaves backing unchanged");kl_a64_precise_destroy(deniedCpu);
 auto inconsistent=config;inconsistent.dczid_el0=5;require(!kl_a64_precise_create(inconsistent),"unsupported cache-zero geometry is never misreported");
 require(step(cpu,m,s,counter_read).kind==KL_A64_BUDGET&&s.x[0]==counter(nullptr),"existing clock owner");
 s=initial();out=step(cpu,m,s,original_svc);require(out.kind==KL_A64_SVC&&out.detail==0&&s.pc==Owner::code+4,"original SVC continuation");
 out=step(cpu,m,s,genuine_break);require(out.kind==KL_A64_EXCEPTION&&out.address==Owner::code,"genuine original BRK remains an exception");
 s=initial();s.x[8]=0;s.x[10]=1;out=step(cpu,m,s,scalar);require(out.kind==KL_A64_MEMORY&&out.owner_error==-14&&out.address==1&&s.pc==Owner::code,"precise failed access");
 s=initial();s.x[0]=0;require(step(cpu,m,s,add_1).kind==KL_A64_BUDGET&&s.x[0]==1,"first original generation");require(step(cpu,m,s,add_9).kind==KL_A64_BUDGET&&s.x[0]==10,"changed source executes before next fetch");
 m.replaced=true;s=initial();s.x[8]=0;s.x[9]=91;s.x[10]=Owner::data;out=step(cpu,m,s,scalar,2);memcpy(&word,m.replacement.data(),4);require(out.kind==KL_A64_BUDGET&&word==91,"replacement retires the old native pointer");
 m.missing=true;out=step(cpu,m,s,scalar,3);require(out.kind==KL_A64_MEMORY&&out.address==Owner::code,"unmapped generation cannot use a stale pointer");m.missing=false;
 // A separate process may truncate a leased file; mapping admission cannot
 // turn that into a host crash or swallow an unrelated native signal.
 char filename[]="/tmp/android-a64-backing-XXXXXX";int fd=mkstemp(filename);require(fd>=0,"private backing file");unlink(filename);
 require(!ftruncate(fd,16384),"initial backing length");m.file=mmap(nullptr,16384,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);require(m.file!=MAP_FAILED,"actual file mapping");
 for(auto op:{load_plain,scalar,atomic_add}){
  require(!ftruncate(fd,0),"concurrent backing truncation");s=initial();s.x[2]=Owner::data;s.x[10]=Owner::data;s.x[8]=0;s.x[1]=1;
  out=step(cpu,m,s,op);require(out.kind==KL_A64_MEMORY&&out.owner_error==-5&&out.address==Owner::data&&s.pc==Owner::code,"backing SIGBUS is original memory exception at original instruction");
  sigset_t mask;require(!pthread_sigmask(SIG_SETMASK,nullptr,&mask)&&!sigismember(&mask,SIGBUS),"fault return restores original signal mask");
  require(!ftruncate(fd,16384),"backing restored");s=initial();s.x[2]=Owner::data;s.x[10]=Owner::data;s.x[8]=0;s.x[1]=1;
  require(step(cpu,m,s,op).kind==KL_A64_BUDGET,"precise owner remains usable after native fault");
 }
 munmap(m.file,16384);m.file=nullptr;close(fd);
 m.replaced=false;new(m.bytes.data())std::atomic<uint64_t>(0);auto &shared=*(std::atomic<uint64_t>*)m.bytes.data();std::atomic<bool> ready{false},done{false};uint64_t native=0;
 std::thread writer([&]{ready=true;while(!done.load()){shared.fetch_add(1);native++;}});while(!ready.load()){}
 constexpr uint64_t count=20000;s=initial();s.x[1]=1;s.x[2]=Owner::data;
 for(uint64_t i=0;i<count;i++){out=step(cpu,m,s,atomic_add,4);require(out.kind==KL_A64_BUDGET,"mixed-native atomic step");}
 done=true;writer.join();require(shared.load()==native+count,"native and emulated atomic updates are both retained");
 // Exchange conserves every submitted value, including values returned to a
 // native peer. Plain load/store SWP loses that invariant under contention.
 shared.store(0);ready=false;done=false;uint64_t nativeInputs=0,nativeOutputs=0,emulatedInputs=0,emulatedOutputs=0;
 std::thread exchanges([&]{uint64_t n=0;ready=true;while(!done.load()){uint64_t next=++n;nativeInputs+=next;nativeOutputs+=shared.exchange(next);}});while(!ready.load()){}
 s=initial();s.x[2]=Owner::data;
 for(uint64_t i=0;i<count;i++){s.x[1]=(1ull<<40)+i;emulatedInputs+=s.x[1];out=step(cpu,m,s,atomic_swap,4);require(out.kind==KL_A64_BUDGET,"mixed-native exchange step");emulatedOutputs+=s.x[0];}
 done=true;exchanges.join();require(shared.load()+nativeOutputs+emulatedOutputs==nativeInputs+emulatedInputs,"native and emulated exchanges preserve all values");
 for(auto op:{atomic_smin,atomic_smax,atomic_umin,atomic_umax}){
  shared.store(34);s=initial();s.x[2]=Owner::data;s.x[1]=op==atomic_smin||op==atomic_umin?7:91;
  out=step(cpu,m,s,op,4);require(out.kind==KL_A64_BUDGET&&s.x[0]==34&&shared.load()==s.x[1],"original signed/unsigned min/max helpers");
 }
 m.jit=mmap(nullptr,16384,7,MAP_PRIVATE|MAP_ANON|MAP_JIT,-1,0);require(m.jit!=MAP_FAILED,"original JIT allocation");m.jitData=true;
 s=initial();s.x[8]=0;s.x[9]=91;s.x[10]=Owner::data;out=step(cpu,m,s,scalar,5);require(out.kind==KL_A64_BUDGET&&*(uint32_t*)m.jit==91,"ordinary store into existing JIT source");
 s=initial();s.x[1]=7;s.x[2]=Owner::data;out=step(cpu,m,s,atomic_add,5);require(out.kind==KL_A64_BUDGET&&s.x[0]==91&&*(uint64_t*)m.jit==98,"original atomic into existing JIT source");
 m.instructions[0]=load_exclusive[0];m.instructions[1]=add_to_store[0];m.instructions[2]=store_exclusive[0];progressed=false;
 for(unsigned retry=0;retry<1000&&!progressed;retry++){s=initial();s.x[2]=Owner::data;out=kl_a64_precise_run(cpu,&s,5);require(out.kind==KL_A64_BUDGET,"JIT exclusive interval");progressed=s.x[3]==0;}
 require(progressed&&*(uint64_t*)m.jit==99,"native exclusive loop preserves the actual MAP_JIT write view and makes progress");
 m.jitData=false;m.jitCode=true;s=initial();s.x[8]=0;s.x[9]=73;s.x[10]=Owner::code+512;
 out=step(cpu,m,s,scalar,6);require(out.kind==KL_A64_BUDGET&&*(uint32_t*)((char*)m.jit+512)==73&&s.pc==Owner::code+4,"original self store fetched from JIT source");
 m.jitCode=false;munmap(m.jit,16384);
 // A cached read admission must not bypass the original write/pager owner.
 m.bytes.fill(0);m.readOnly=true;m.instructions[0]=load_exclusive[0];m.instructions[1]=scalar[0];s=initial();s.x[2]=Owner::data;s.x[8]=0;s.x[9]=73;s.x[10]=Owner::data;
 out=kl_a64_precise_run_owned(cpu,&s,7,Owner::code,Owner::code+8,64);
 require(out.kind==KL_A64_MEMORY&&out.owner_error==-11&&s.pc==Owner::code+4&&*(uint32_t*)m.bytes.data()==0,"read-only cached view preserves actual owner write admission and error");m.readOnly=false;
 // Compare the exact same original register loop through one-step and
 // bounded owned execution; timing is evidence, never an acceptance threshold.
 m.instructions[0]=0x91000421;m.instructions[1]=0xf1000400;m.instructions[2]=0x54ffffc1;
 auto executeLoop=[&](bool batched){auto state=initial();state.x[0]=2048;state.x[1]=0;unsigned calls=0;auto begin=std::chrono::steady_clock::now();
  while(state.pc<Owner::code+12){auto result=batched?kl_a64_precise_run_owned(cpu,&state,7,Owner::code,Owner::code+12,64):kl_a64_precise_step(cpu,&state,7);require(result.kind==KL_A64_BUDGET&&++calls<=6144,"owned loop makes bounded forward progress");}
  require(state.pc==Owner::code+12&&state.x[0]==0&&state.x[1]==2048,"owned loop has the identical original result");auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();printf("owned-loop batch=%d calls=%u microseconds=%lld\n",batched,calls,(long long)us);
 };
 executeLoop(false);executeLoop(true);
 m.instructions[0]=zero_block[0];m.instructions[1]=add_9[0];m.bytes.fill(0xa5);m.dirtyLength=0;s=initial();s.x[0]=7;s.x[3]=Owner::data+71;
 out=kl_a64_precise_run_owned(cpu,&s,8,Owner::code,Owner::code+8,64);require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+4&&s.x[0]==7&&m.dirtyFirst==Owner::data+64&&m.dirtyLength==64,"owned run stops after full original DC ZVA write before following instruction");
 m.instructions[0]=add_1[0];m.instructions[1]=scalar[0];m.instructions[2]=add_9[0];s=initial();s.x[0]=0;s.x[8]=0;s.x[9]=73;s.x[10]=Owner::data;
 out=kl_a64_precise_run_owned(cpu,&s,8,Owner::code,Owner::code+12,64);require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+8&&s.x[0]==1&&*(uint32_t*)m.bytes.data()==73,"owned run stops after ordinary store");
 m.instructions[1]=original_svc[0];s=initial();s.x[0]=0;out=kl_a64_precise_run_owned(cpu,&s,8,Owner::code,Owner::code+12,64);require(out.kind==KL_A64_SVC&&s.pc==Owner::code+8&&s.x[0]==1,"owned run preserves original syscall continuation");require(kl_a64_precise_instruction_address(cpu)==Owner::code+4,"original syscall instruction address is distinct from its continuation");
 m.instructions[1]=genuine_break[0];s=initial();s.x[0]=0;out=kl_a64_precise_run_owned(cpu,&s,8,Owner::code,Owner::code+12,64);require(out.kind==KL_A64_EXCEPTION&&out.address==Owner::code+4&&s.x[0]==1,"owned run preserves original exception boundary");
 m.instructions[0]=load_exclusive[0];m.instructions[1]=add_to_store[0];m.instructions[2]=store_exclusive[0];old=12;memcpy(m.bytes.data(),&old,8);progressed=false;
 for(unsigned retry=0;retry<1000&&!progressed;retry++){s=initial();s.x[2]=Owner::data;out=kl_a64_precise_run_owned(cpu,&s,8,Owner::code,Owner::code+8,1);require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+12,"owned budget retains complete exclusive interval beyond range edge");progressed=s.x[3]==0;}
 require(progressed&&*(uint64_t*)m.bytes.data()==13,"owned exclusive interval makes real progress");
 // Only the actual mapping owner can admit known unique private data.
 // Compare the identical store-heavy original loop with the default barrier.
 for(unsigned budget:{64u,1024u})for(bool privateData:{false,true}){
  m.mapCalls=0;m.privateData=privateData;m.dirtyLength=0;m.bytes.fill(0);memcpy(m.instructions.data(),store_loop,20);s=initial();s.x[0]=1024;s.x[1]=0;s.x[2]=Owner::data;
  unsigned calls=0;auto begin=std::chrono::steady_clock::now();
  while(s.pc<Owner::code+16){out=kl_a64_precise_run_owned(cpu,&s,9,Owner::code,Owner::code+16,budget);require(out.kind==KL_A64_BUDGET&&++calls<=1025,"store-heavy original loop bounded progress");}
  require(s.x[0]==0&&s.x[1]==1024&&*(uint64_t*)m.bytes.data()==1023,"private-data batching preserves exact original state and bytes");
  require(privateData?(calls==4096/budget&&m.dirtyLength==0):(calls>=1024&&m.dirtyLength==8),"only admitted private data omits executable generation barriers");
  require(m.mapCalls==(privateData?calls*2:calls+1024),"each interval admits accessed code and data exactly once without duplicate owner callbacks");
  auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();printf("private-data loop admitted=%d budget=%u calls=%u maps=%u microseconds=%lld\n",privateData,budget,calls,m.mapCalls,(long long)us);
 }
 m.privateData=false;
 // Compare an original call/return path across two independently admitted
 // code pages. Every map retires with a TB invalidation at public return.
 for(bool follow:{false,true}){
  m.instructions[0]=add_1[0];m.instructions[1]=0x140003ff;
  m.nextInstructions[0]=add_9[0];m.nextInstructions[1]=0x17fffbff;
  m.nextFlags=follow?KL_A64_MAPPING_EXECUTION_OWNER:0;m.mapCalls=0;s=initial();s.x[0]=0;
  unsigned calls=0;auto begin=std::chrono::steady_clock::now();
  while(s.x[0]<2560){const uint64_t start=s.pc<Owner::code+4096?Owner::code:Owner::code+4096;out=kl_a64_precise_run_owned(cpu,&s,10,start,start+8,1024);require(out.kind==KL_A64_BUDGET&&++calls<=512,"owned cross-view bounded progress");}
  const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
  require(s.pc==Owner::code&&s.x[0]==2560&&calls==(follow?1u:512u)&&m.mapCalls==calls*2,"positive owned views preserve exact state and1024-instruction bound");
  printf("owned-view follow=%d instructions=1024 calls=%u maps=%u map-retirement-TB-invalidations=%u microseconds=%lld\n",follow,calls,m.mapCalls,m.mapCalls,(long long)us);
 }
 m.instructions[0]=add_1[0];m.instructions[1]=0x140003ff;m.nextInstructions[0]=add_9[0];m.nextInstructions[1]=0x140003ff;m.nativeInstructions[0]=add_9[0];m.nextFlags=KL_A64_MAPPING_EXECUTION_OWNER;s=initial();s.x[0]=0;
 out=kl_a64_precise_run_owned(cpu,&s,11,Owner::code,Owner::code+8,1024);
 require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+8192&&s.x[0]==10,"site-free native page returns before its first instruction");
 m.nextInstructions[0]=scalar[0];m.nextInstructions[1]=add_9[0];m.dirtyLength=0;s=initial();s.x[0]=0;s.x[8]=0;s.x[9]=73;s.x[10]=Owner::code+512;
 out=kl_a64_precise_run_owned(cpu,&s,12,Owner::code,Owner::code+8,1024);
 require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+4100&&s.x[0]==1&&m.instructions[128]==73&&m.dirtyFirst==Owner::code+512&&m.dirtyLength==4,"cross-view source store stops before following instruction");
 m.nextFlags=0;
 // The caller can inspect attention immediately after this exact bounded
 // original-instruction interval, without requiring an SVC or source store.
 m.instructions[0]=add_1[0];m.instructions[1]=0x17ffffff;s=initial();s.x[0]=0;
 auto attentionBegin=std::chrono::steady_clock::now();
 out=kl_a64_precise_run_owned(cpu,&s,10,Owner::code,Owner::code+8,1024);
 const auto attentionUs=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-attentionBegin).count();
 require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code&&s.x[0]==512,"attention returns after exactly 1024 ordinary original instructions");
 printf("attention-bound instructions=1024 microseconds=%lld\n",(long long)attentionUs);
 // Across public returns, original RX tables are inert until the current
 // owner reacquires every page and verifies its bytes. Ordinary callers keep
 // the original all-retired behavior. No eligibility or instruction changes.
 m.cacheRX=true;m.nextFlags=KL_A64_MAPPING_EXECUTION_OWNER;
 m.instructions[0]=add_1[0];m.instructions[1]=0x140003ff;
 m.nextInstructions[0]=add_9[0];m.nextInstructions[1]=0x17fffbff;
 for(bool cached:{false,true}){
  require(kl_a64_precise_set_owned_cache(cpu,cached?revalidate:nullptr)==0,"explicit original RX cache selection");
  const auto before=kl_a64_precise_counters(cpu);s=initial();s.x[0]=0;const auto begin=std::chrono::steady_clock::now();
  for(unsigned interval=0;interval<16;interval++){
   out=kl_a64_precise_run_owned(cpu,&s,100+interval,Owner::code,Owner::code+8,1024);
   require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code&&s.x[0]==2560*(interval+1),"revalidated cross-page execution preserves each exact1024 bound");
  }
  const auto after=kl_a64_precise_counters(cpu);const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
  require(after.mapped-before.mapped==(cached?2u:32u)&&after.retired-before.retired==(cached?0u:32u),"only current leased original RX tables avoid remap/TB retirement");
  require(after.revalidated-before.revalidated==(cached?30u:0u),"each cached mapping is re-admitted before every later execution");
  printf("revalidated-code cached=%d instructions=16384 intervals=16 mapped=%llu retired=%llu validated=%llu microseconds=%lld\n",cached,(unsigned long long)(after.mapped-before.mapped),(unsigned long long)(after.retired-before.retired),(unsigned long long)(after.revalidated-before.revalidated),(long long)us);
 }
 require(kl_a64_precise_clear_exclusive(cpu)==0,"start real native-crossing cache check");
 m.instructions[0]=add_1[0];m.instructions[1]=0x140007ff;s=initial();s.x[0]=0;
 const auto beforeNative=kl_a64_precise_counters(cpu);const auto nativeBegin=std::chrono::steady_clock::now();
 for(unsigned interval=0;interval<16;interval++){
  s.pc=Owner::code;require(kl_a64_precise_reset(cpu)==0,"owned-fault entry resets original CPU state");
  out=kl_a64_precise_run_owned(cpu,&s,300+interval,Owner::code,Owner::code+8,1024);
  require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+8192&&s.x[0]==10*interval+1,"untouched native target returns before its first instruction");
  require(kl_a64_precise_reset(cpu)==0,"native continuation resets the real reservation and CPU state");
  s.x[0]=((uint64_t(*)(uint64_t))(void*)add_9)(s.x[0]);
  require(s.x[0]==10*(interval+1),"real native instruction result retained for the next owned entry");
 }
 const auto afterNative=kl_a64_precise_counters(cpu);const auto nativeUs=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-nativeBegin).count();
 require(afterNative.mapped-beforeNative.mapped==17&&afterNative.retired-beforeNative.retired==16&&afterNative.revalidated-beforeNative.revalidated==15,"native crossings preserve only inert revalidated RX tables");
 printf("revalidated-native-crossing intervals=16 mapped=%llu retired=%llu validated=%llu microseconds=%lld\n",(unsigned long long)(afterNative.mapped-beforeNative.mapped),(unsigned long long)(afterNative.retired-beforeNative.retired),(unsigned long long)(afterNative.revalidated-beforeNative.revalidated),(long long)nativeUs);
 m.instructions[1]=0x140003ff;
 require(kl_a64_precise_reset(cpu)==0,"native source change starts without authority from the prior lease");
 const auto original=kl_a64_precise_counters(cpu);m.instructions[0]=add_9[0];s=initial();s.x[0]=0;
 out=kl_a64_precise_run_owned(cpu,&s,200,Owner::code,Owner::code+8,4);
 require(out.kind==KL_A64_BUDGET&&s.x[0]==18&&kl_a64_precise_counters(cpu).source_changed==original.source_changed+1,"same-pointer RX bytes change invalidates the original translation before fetch");
 m.cacheRX=false;m.instructions[0]=add_1[0];s=initial();s.x[0]=0;
 out=kl_a64_precise_run_owned(cpu,&s,201,Owner::code,Owner::code+8,4);
 require(out.kind==KL_A64_BUDGET&&s.x[0]==10&&kl_a64_precise_counters(cpu).retired>=original.retired+2,"RX to writable permission change retires cached authority");
 m.cacheRX=true;s=initial();s.x[0]=0;out=kl_a64_precise_run_owned(cpu,&s,202,Owner::code,Owner::code+8,4);require(out.kind==KL_A64_BUDGET,"current RX replacement admitted");
 m.missing=true;s=initial();s.x[0]=0;out=kl_a64_precise_run_owned(cpu,&s,203,Owner::code,Owner::code+8,4);
 require(out.kind==KL_A64_MEMORY&&s.x[0]==0&&out.address==Owner::code,"removed source never executes an inert cached pointer");m.missing=false;
 m.instructions[0]=scalar[0];m.instructions[1]=add_9[0];s=initial();s.x[0]=0;s.x[8]=0;s.x[9]=77;s.x[10]=Owner::data;
 const auto beforeStore=kl_a64_precise_counters(cpu);
 for(unsigned interval=0;interval<16;interval++){
  s.pc=Owner::code;out=kl_a64_precise_run_owned(cpu,&s,204+interval,Owner::code,Owner::code+8,1024);
  require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+4&&s.x[0]==0&&*(uint32_t*)m.bytes.data()==77,"every conservative data store returns before its next instruction");
 }
 const auto afterStore=kl_a64_precise_counters(cpu);
 require(afterStore.mapped-beforeStore.mapped==17&&afterStore.retired-beforeStore.retired==16&&afterStore.revalidated-beforeStore.revalidated==15,"unchanged leased RX tables survive conservative store barriers while each writable view retires");
 printf("revalidated-source-barriers intervals=16 mapped=%llu retired=%llu validated=%llu\n",(unsigned long long)(afterStore.mapped-beforeStore.mapped),(unsigned long long)(afterStore.retired-beforeStore.retired),(unsigned long long)(afterStore.revalidated-beforeStore.revalidated));
 // Positively admitted private RW/non-X IO tables retain only inert shape.
 // Re-admission happens before execution, while every IO callback still reads
 // or writes the current physical bytes under the new owner interval.
 for(bool cached:{false,true}){
  require(kl_a64_precise_set_owned_cache(cpu,cached?revalidate:nullptr)==0,"explicit private-data table cache selection");
  m.file=m.bytes.data();m.privateData=true;m.bytes.fill(0);memcpy(m.instructions.data(),store_loop,20);s=initial();s.x[0]=100000;s.x[1]=0;s.x[2]=Owner::data;
  const auto before=kl_a64_precise_counters(cpu);const auto begin=std::chrono::steady_clock::now();
  for(unsigned interval=0;interval<16;interval++){
   out=kl_a64_precise_run_owned(cpu,&s,400+interval,Owner::code,Owner::code+20,1024);
   require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code&&s.x[0]==100000-256*(interval+1)&&s.x[1]==256*(interval+1),"private IO store-heavy state preserves every exact1024 bound");
   memcpy(&value,m.bytes.data(),8);require(value==256*(interval+1)-1,"live private IO stores commit exact bytes");
  }
  const auto after=kl_a64_precise_counters(cpu);const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
  require(after.mapped-before.mapped==(cached?2u:32u)&&after.retired-before.retired==(cached?0u:32u)&&after.revalidated-before.revalidated==(cached?30u:0u),"private data and code tables each require fresh admission while avoiding repeated map retirement");
  printf("revalidated-private-data cached=%d instructions=16384 intervals=16 mapped=%llu retired=%llu validated=%llu microseconds=%lld\n",cached,(unsigned long long)(after.mapped-before.mapped),(unsigned long long)(after.retired-before.retired),(unsigned long long)(after.revalidated-before.revalidated),(long long)us);
 }
 m.instructions[0]=load_plain[0];m.instructions[1]=0x17ffffff;s=initial();s.x[2]=Owner::data;
 value=0x34567890abcdef12ull;memcpy(m.bytes.data(),&value,8);out=kl_a64_precise_run_owned(cpu,&s,420,Owner::code,Owner::code+8,1024);
 require(out.kind==KL_A64_BUDGET&&s.x[0]==value,"cached private IO reads current native-updated bytes without snapshots");
 value=0x1234;memcpy(m.bytes.data(),&value,8);s.pc=Owner::code;out=kl_a64_precise_run_owned(cpu,&s,421,Owner::code,Owner::code+8,1024);
 require(out.kind==KL_A64_BUDGET&&s.x[0]==value,"later private IO callbacks observe a new native write immediately");
 m.instructions[0]=scalar[0];m.instructions[1]=add_9[0];m.privateData=false;s=initial();s.x[0]=0;s.x[8]=0;s.x[9]=99;s.x[10]=Owner::data;
 const auto beforePrivateLost=kl_a64_precise_counters(cpu);out=kl_a64_precise_run_owned(cpu,&s,422,Owner::code,Owner::code+8,1024);
 require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+4&&s.x[0]==0&&*(uint32_t*)m.bytes.data()==99&&kl_a64_precise_counters(cpu).retired>=beforePrivateLost.retired+2,"lost unique-private admission retires the table and restores conservative store boundaries");
 m.privateData=true;m.file=m.bytes.data();s=initial();s.x[8]=0;s.x[9]=88;s.x[10]=Owner::data;out=kl_a64_precise_run_owned(cpu,&s,423,Owner::code,Owner::code+8,1);require(out.kind==KL_A64_BUDGET,"fresh private IO store admitted");
 m.readOnly=true;s=initial();s.x[8]=0;s.x[9]=77;s.x[10]=Owner::data;out=kl_a64_precise_run_owned(cpu,&s,424,Owner::code,Owner::code+8,1024);
 require(out.kind==KL_A64_MEMORY&&out.address==Owner::data&&out.owner_error==-11&&s.pc==Owner::code&&*(uint32_t*)m.bytes.data()==88,"readonly replacement rejects cached writes before changing bytes");m.readOnly=false;
 m.instructions[0]=load_plain[0];m.instructions[1]=0x17ffffff;s=initial();s.x[2]=Owner::data;out=kl_a64_precise_run_owned(cpu,&s,425,Owner::code,Owner::code+8,4);require(out.kind==KL_A64_BUDGET,"private data read lease restored");
 value=0x98765432;memcpy(m.replacement.data(),&value,8);m.file=m.replacement.data();s.pc=Owner::code;out=kl_a64_precise_run_owned(cpu,&s,426,Owner::code,Owner::code+8,4);
 require(out.kind==KL_A64_BUDGET&&s.x[0]==value,"data pointer replacement reacquires the new physical object");
 m.file=m.instructions.data();m.privateData=false;s.pc=Owner::code;out=kl_a64_precise_run_owned(cpu,&s,427,Owner::code,Owner::code+8,2);
 require(out.kind==KL_A64_BUDGET&&s.x[0]==((uint64_t)m.instructions[1]<<32|m.instructions[0]),"shared or executable alias data uses current uncached IO bytes");
 m.file=m.bytes.data();m.privateData=true;s.pc=Owner::code;out=kl_a64_precise_run_owned(cpu,&s,428,Owner::code,Owner::code+8,2);require(out.kind==KL_A64_BUDGET,"private data restored before removal");
 m.missing=true;s=initial();s.x[2]=Owner::data;out=kl_a64_precise_run_owned(cpu,&s,429,Owner::code,Owner::code+8,1024);
 require(out.kind==KL_A64_MEMORY&&out.owner_error==-14&&s.pc==Owner::code,"removed mapping never reads a retained data pointer");m.missing=false;m.file=nullptr;m.privateData=false;m.instructions[0]=scalar[0];m.instructions[1]=add_9[0];
 // A legitimate writable alias changes the bytes backing the cached RX view.
 // The source store still stops before the next instruction; identical host
 // pointers and permissions are insufficient to reuse its old translation.
 m.aliasCode=true;s=initial();s.x[0]=0;s.x[8]=0;s.x[9]=add_9[0];s.x[10]=Owner::data;
 const auto beforeAlias=kl_a64_precise_counters(cpu);out=kl_a64_precise_run_owned(cpu,&s,230,Owner::code,Owner::code+8,1024);
 require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+4&&m.instructions[0]==add_9[0]&&s.x[0]==0,"writable alias commits original opcode and stops before continuation");
 m.aliasCode=false;s=initial();s.x[0]=0;out=kl_a64_precise_run_owned(cpu,&s,231,Owner::code,Owner::code+8,1);
 require(out.kind==KL_A64_BUDGET&&s.pc==Owner::code+4&&s.x[0]==9&&kl_a64_precise_counters(cpu).source_changed==beforeAlias.source_changed+1,"aliased code write is retranslated before the very next original fetch");
 m.instructions[0]=original_svc[0];s=initial();const auto beforeSvc=kl_a64_precise_counters(cpu);
 out=kl_a64_precise_run_owned(cpu,&s,240,Owner::code,Owner::code+8,1024);const auto afterSvc=kl_a64_precise_counters(cpu);
 require(out.kind==KL_A64_SVC&&afterSvc.retired-beforeSvc.retired>=1,"architectural syscall exit still flushes cached original-code tables");
 m.instructions[0]=add_1[0];m.instructions[1]=0x17ffffff;s=initial();s.x[0]=0;out=kl_a64_precise_run_owned(cpu,&s,205,Owner::code,Owner::code+8,1024);
 const auto beforeClear=kl_a64_precise_counters(cpu);require(kl_a64_precise_clear_exclusive(cpu)==0&&kl_a64_precise_counters(cpu).retired==beforeClear.retired+1,"native continuation drops inactive code cache");
 kl_a64_precise_destroy(cpu);puts("PASS precise original A64 state, memory generations, source visibility, SIMD, atomics/native peers, exclusive boundaries, SVC and exceptions");
}
