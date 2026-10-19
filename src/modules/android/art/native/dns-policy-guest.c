#include <stdbool.h>
#include <stdatomic.h>
#include <errno.h>
#include <DnsHelperPublic.h>
#include "bridge.h"

/* Complete dns_helper physical policy interface. Android's original resolver
 * calls this before DNS traffic. The profile sandbox already owns network
 * isolation for every enrolled process; no BPF/UID policy table is copied. */
static atomic_bool initialized;
static int allowed(void){
 struct KljArtNetwork value={.operation=9};
 int64_t status=klj_art_host(6,&value,sizeof(value));
 return status?(int)status:(int)value.allowed;
}
int ADnsHelper_init(void){
 int result=allowed();if(result<0)return result;
 atomic_store_explicit(&initialized,true,memory_order_release);return 0;
}
int ADnsHelper_isUidNetworkingBlocked(uid_t uid,bool metered){
 (void)uid;(void)metered;
 if(!atomic_load_explicit(&initialized,memory_order_acquire))return -EUNATCH;
 int result=allowed();return result<0?result:!result;
}
