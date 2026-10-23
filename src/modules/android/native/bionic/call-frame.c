#include "call-packet.h"
#include <assert.h>
#include <alloca.h>
/* Reuse the original descriptor parser, record cache and signed-page publisher
 * with a separate Bionic dispatcher; normal JNI entry symbols stay unchanged. */
#define klj_native_abi klj_bionic_native_abi
#define klj_native_env_abi klj_bionic_native_env_abi
#define klj_native_abi_dispatch klj_bionic_native_abi_dispatch
#include "../nativeabi.c"
#undef klj_native_abi
#undef klj_native_env_abi
#undef klj_native_abi_dispatch
#include "call-frame.h"
void klj_bionic_call_frame_dispatch(const NativeABI *record,KljCallPacket *p,const unsigned char *darwin_stack){
 assert(record&&p&&record->count<=256&&record->stackBytes<=KLJ_PACKET_MAX_STACK);
 p->size=sizeof(*p);p->version=KLJ_CALL_PACKET_VERSION;p->target=(uintptr_t)record->target;p->argument_padding=0;p->reserved=0;
 unsigned char *stack=alloca(record->stackBytes?record->stackBytes:16);memset(stack,0,record->stackBytes);
 for(unsigned i=0;i<record->count;i++)memcpy(stack+i*8,darwin_stack+record->slots[i].offset,record->slots[i].size);
 p->stack=stack;p->stack_bytes=record->stackBytes;memset(p->result_x,0,sizeof p->result_x);memset(p->result_v,0,sizeof p->result_v);
 p->status=klj_bionic_call_frame_invoke(p);
}
