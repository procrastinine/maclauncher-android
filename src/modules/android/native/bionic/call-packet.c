#include "call-packet.h"
extern void klj_call_packet_assembly(KljCallPacket *);
/* Only trusted JNI descriptor-derived storage is passed to the assembly gate.
 * Rejections leave the target and all result storage untouched. */
uint64_t klj_call_packet(uint64_t raw){
 KljCallPacket *p=(void*)(uintptr_t)raw;
 if(!p)return (uint64_t)(int64_t)-22;
 if(p->size!=sizeof(*p)||p->version!=KLJ_CALL_PACKET_VERSION||!p->target||
    p->stack_bytes>KLJ_PACKET_MAX_STACK||(p->stack_bytes&15)||
    (p->stack_bytes&&!p->stack)){p->status=-22;return (uint64_t)p->status;}
 klj_call_packet_assembly(p);return (uint64_t)p->status;
}
