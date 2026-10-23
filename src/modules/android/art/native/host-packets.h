#pragma once
#include <stdint.h>
#define KLJ_ART_PACKET_ABI 1u
#define KLJ_ART_PACKET_READ 1u
#define KLJ_ART_PACKET_READ_WRITE 3u
#ifdef __cplusplus
extern "C" {
#define KLJ_ART_PACKET_ALIGN(type) alignof(type)
#else
#define KLJ_ART_PACKET_ALIGN(type) _Alignof(type)
#endif
/* Only the typed outer packet is resolved. All fields retain their guest ABI.
 * Successful pointers remain leased by the caller's current host-call scope. */
typedef int64_t (*KljArtPacketResolver)(uint64_t,uint64_t,uint64_t,uint32_t,uint64_t*);
int64_t maclauncher_art_install_packet_resolver(uint32_t,KljArtPacketResolver);
int64_t klj_art_resolve_packet(uint64_t,uint64_t,uint64_t,uint32_t,uint64_t*);
#ifdef __cplusplus
}
#endif
