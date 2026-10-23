#include "host-packets.h"
#include <stdatomic.h>
static _Atomic(KljArtPacketResolver) resolver;
__attribute__((visibility("default")))
int64_t maclauncher_art_install_packet_resolver(uint32_t abi,KljArtPacketResolver next){
 if(abi!=KLJ_ART_PACKET_ABI||!next)return -22;
 KljArtPacketResolver expected=0;
 if(atomic_compare_exchange_strong_explicit(&resolver,&expected,next,memory_order_release,memory_order_acquire)||expected==next)return 0;
 return -16;
}
int64_t klj_art_resolve_packet(uint64_t address,uint64_t length,uint64_t alignment,uint32_t access,uint64_t *output){
 if(!address||!output||!alignment||(alignment&(alignment-1))||address%alignment||
    (access!=KLJ_ART_PACKET_READ&&access!=KLJ_ART_PACKET_READ_WRITE))return -22;
 KljArtPacketResolver current=atomic_load_explicit(&resolver,memory_order_acquire);
 if(!current){*output=address;return 0;}
 uint64_t resolved=0;int64_t error=current(address,length,alignment,access,&resolved);
 if(error)return error<0?error:-14;
 if(!resolved||resolved%alignment)return -14;
 *output=resolved;return 0;
}
