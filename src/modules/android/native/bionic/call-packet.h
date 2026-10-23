#pragma once
#define KLJ_CALL_PACKET_VERSION 1
#define KLJ_PACKET_SIZE 336
#define KLJ_PACKET_TARGET 8
#define KLJ_PACKET_X 16
#define KLJ_PACKET_V 96
#define KLJ_PACKET_STACK 224
#define KLJ_PACKET_STACK_BYTES 232
#define KLJ_PACKET_RESULT_X 240
#define KLJ_PACKET_RESULT_V 256
#define KLJ_PACKET_STATUS 320
#define KLJ_PACKET_RESERVED 328
/* NativeABI's complete descriptor layout has at most 256 eight-byte spills. */
#define KLJ_PACKET_MAX_STACK 2048
#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stddef.h>
typedef struct __attribute__((aligned(16))) {
 uint32_t size,version;
 uint64_t target,x[9],argument_padding;
 unsigned char v[8][16];
 const unsigned char *stack;
 uint64_t stack_bytes,result_x[2];
 unsigned char result_v[4][16];
 int64_t status;
 uint64_t reserved;
} KljCallPacket;
_Static_assert(sizeof(KljCallPacket)==KLJ_PACKET_SIZE,"packet size");
#define KLJ_PACKET_ASSERT(field,offset) _Static_assert(offsetof(KljCallPacket,field)==offset,"packet " #field)
KLJ_PACKET_ASSERT(target,KLJ_PACKET_TARGET);KLJ_PACKET_ASSERT(x,KLJ_PACKET_X);KLJ_PACKET_ASSERT(v,KLJ_PACKET_V);
KLJ_PACKET_ASSERT(stack,KLJ_PACKET_STACK);KLJ_PACKET_ASSERT(stack_bytes,KLJ_PACKET_STACK_BYTES);
KLJ_PACKET_ASSERT(result_x,KLJ_PACKET_RESULT_X);KLJ_PACKET_ASSERT(result_v,KLJ_PACKET_RESULT_V);KLJ_PACKET_ASSERT(status,KLJ_PACKET_STATUS);
uint64_t klj_call_packet(uint64_t);
#endif
