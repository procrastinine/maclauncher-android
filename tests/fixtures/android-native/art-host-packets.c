#include "host-packets.h"
#include <assert.h>
#include <stdint.h>
static uint64_t source[2]={17,29},target[2]={17,29};
static uint32_t seen_access;static unsigned calls;static int fail;
static int64_t resolve(uint64_t address,uint64_t length,uint64_t alignment,uint32_t access,uint64_t *output){
 assert(address==(uintptr_t)source);assert(length==sizeof source);assert(alignment==_Alignof(uint64_t));
 calls++;seen_access=access;*output=(uintptr_t)target;return fail;
}
static int64_t other(uint64_t a,uint64_t n,uint64_t l,uint32_t access,uint64_t *out){(void)a;(void)n;(void)l;(void)access;(void)out;return -14;}
int main(void){
 uint64_t output=0x55;
 assert(klj_art_resolve_packet((uintptr_t)source,sizeof source,8,KLJ_ART_PACKET_READ,&output)==0&&output==(uintptr_t)source);
 assert(maclauncher_art_install_packet_resolver(2,resolve)==-22);
 assert(maclauncher_art_install_packet_resolver(1,0)==-22);
 assert(maclauncher_art_install_packet_resolver(1,resolve)==0);
 assert(maclauncher_art_install_packet_resolver(1,resolve)==0);
 assert(maclauncher_art_install_packet_resolver(1,other)==-16);
 assert(klj_art_resolve_packet((uintptr_t)source,sizeof source,8,KLJ_ART_PACKET_READ,&output)==0);
 assert(output==(uintptr_t)target&&seen_access==KLJ_ART_PACKET_READ&&calls==1);
 assert(source[0]==17&&source[1]==29&&target[0]==17&&target[1]==29);
 assert(klj_art_resolve_packet((uintptr_t)source,sizeof source,8,KLJ_ART_PACKET_READ_WRITE,&output)==0);
 assert(seen_access==KLJ_ART_PACKET_READ_WRITE&&calls==2);
 output=0x55;fail=-14;
 assert(klj_art_resolve_packet((uintptr_t)source,sizeof source,8,KLJ_ART_PACKET_READ,&output)==-14&&output==0x55);
 fail=7;assert(klj_art_resolve_packet((uintptr_t)source,sizeof source,8,KLJ_ART_PACKET_READ,&output)==-14&&output==0x55);
 assert(klj_art_resolve_packet((uintptr_t)source+1,sizeof source,8,KLJ_ART_PACKET_READ,&output)==-22);
 assert(klj_art_resolve_packet((uintptr_t)source,sizeof source,3,KLJ_ART_PACKET_READ,&output)==-22);
 assert(klj_art_resolve_packet((uintptr_t)source,sizeof source,8,2,&output)==-22);
 assert(klj_art_resolve_packet((uintptr_t)source,sizeof source,8,KLJ_ART_PACKET_READ,0)==-22);
 assert(output==0x55&&calls==4);return 0;
}
