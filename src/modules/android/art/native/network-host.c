#include "bridge.h"
#include "connectivity-core.h"
#include "network-policy.h"
#include <stdatomic.h>

/* One physical observer and the existing immutable advertising policy. JNI
 * remains in the guest. Waiting uses the monitor's condition, not polling or
 * a second network-state table. */
static int64_t network_error(int value){return -(value==ECANCELED?KLJ_ART_NETWORK_ECANCELED:value==EILSEQ?KLJ_ART_NETWORK_EILSEQ:value);}
static atomic_int networkAccess=-1;
int64_t maclauncher_art_network(uint64_t address,uint64_t size){
 if(!address||size!=sizeof(struct KljArtNetwork))return -EINVAL;
 struct KljArtNetwork *value=(void*)(uintptr_t)address;
 if(value->operation==8){int unset=-1,allowed=value->allowed!=0;if(!atomic_compare_exchange_strong(&networkAccess,&unset,allowed)&&unset!=allowed)return -EPERM;return 0;}
 if(value->operation==9){int allowed=atomic_load(&networkAccess);if(allowed<0)return -ENXIO;value->allowed=(unsigned)allowed;return 0;}
 if(value->operation==5)return klj_network_configure_ads(value->allowed!=0)?0:-EPERM;
 if(value->operation==6){
  char *text=network_resolver_configuration();if(!text)return network_error(errno?errno:ENOMEM);
  value->length=strlen(text)+1;
  if(value->capacity>=value->length&&value->buffer)memcpy((void*)(uintptr_t)value->buffer,text,value->length);
  free(text);return 0;
 }
 if(value->operation==7){
  const char*host=(const void*)(uintptr_t)value->buffer;
  if(host&&(!value->capacity||strnlen(host,value->capacity)==value->capacity))return -EINVAL;
  value->allowed=klj_network_host_allowed(host);return 0;
 }
 if(value->operation==4){network_close();return 0;}
 int error=network_observe();if(error)return network_error(error);
 pthread_mutex_lock(&lock);
 switch(value->operation){
  case 0:break;
  case 1:if(!value->token){error=EINVAL;break;}epoch=value->token;pthread_cond_broadcast(&condition);break;
  case 2:while(!closed&&epoch==value->token&&packed()==value->snapshot)pthread_cond_wait(&condition,&lock);if(closed||epoch!=value->token)error=ECANCELED;break;
  case 3:if(epoch==value->token){epoch=0;pthread_cond_broadcast(&condition);}break;
  default:error=EINVAL;
 }
 value->snapshot=packed();pthread_mutex_unlock(&lock);return network_error(error);
}
