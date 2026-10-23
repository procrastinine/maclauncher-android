#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
static _Thread_local uint64_t token;
void kl_native_writer_test_store(volatile uint64_t *out,uint64_t value){*out=value;}
void kl_native_writer_test_copy(void *out,const void *input,size_t length){memcpy(out,input,length);}
uint64_t kl_native_writer_test_tls(volatile uint64_t *out,uint64_t value){token=value;*out=value;return token;}
void kl_native_writer_test_add(_Atomic uint64_t *out,unsigned count){for(unsigned n=0;n<count;n++)atomic_fetch_add_explicit(out,1,memory_order_seq_cst);}
void kl_native_writer_test_lrsc(uint64_t *out){
 __asm__ volatile("1: ldaxr x8,[%0]\nadd x8,x8,#1\nstlxr w9,x8,[%0]\ncbnz w9,1b"::"r"(out):"x8","x9","memory","cc");
}
