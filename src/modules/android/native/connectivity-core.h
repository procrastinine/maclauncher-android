#pragma once
/* Passive physical network observation shared by the JVM and ART transports.
 * Android request/network identity remains in HostConnectivityManager. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <Network/Network.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <netinet/in.h>
// One passive observer while the host is live. Final close is terminal and
// releases both listener references and the monitor/queue owned by this host.
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int closed;
static nw_path_monitor_t monitor;
static dispatch_queue_t queue;
static uint64_t revision,epoch;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static void (*listener)(uint64_t,uint64_t);
static SCDynamicStoreRef dnsStore;
static int state=-1;
/* The same observer owns route state and physical transport availability.
 * Availability survives a disconnected route and never implies Internet access.
 * Bits 2..4 describe the active path; bits 10..12 describe installed transports.
 */
static int transports(void){
 int mask=0;CFArrayRef interfaces=SCNetworkInterfaceCopyAll();if(!interfaces)return 0;
 for(CFIndex i=0;i<CFArrayGetCount(interfaces);i++){
  SCNetworkInterfaceRef interface=(SCNetworkInterfaceRef)CFArrayGetValueAtIndex(interfaces,i);
  CFStringRef type=SCNetworkInterfaceGetInterfaceType(interface);if(!type)continue;
  if(CFEqual(type,kSCNetworkInterfaceTypeIEEE80211))mask|=4;
  else if(CFEqual(type,kSCNetworkInterfaceTypeEthernet))mask|=8;
  else if(CFEqual(type,kSCNetworkInterfaceTypeWWAN))mask|=16;
 }
 CFRelease(interfaces);return mask;
}
/* NWPathMonitor's first callback is asynchronous. A synchronous Android query
 * must observe the current route, rather than report a fabricated disconnected
 * state while that callback is in flight. Reachability reads local routing
 * configuration; it neither resolves a hostname nor sends a network probe. */
static int initial_state(void){
 int value=transports()<<8;
 struct sockaddr_in ipv4={.sin_len=sizeof(ipv4),.sin_family=AF_INET};
 struct sockaddr_in6 ipv6={.sin6_len=sizeof(ipv6),.sin6_family=AF_INET6};
 const struct sockaddr *addresses[]={(const struct sockaddr *)&ipv4,(const struct sockaddr *)&ipv6};
 /* NWPathMonitor has no synchronous query, so the deprecated reachability
  * read stays the initial-state source. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
 for(unsigned i=0;i<sizeof(addresses)/sizeof(*addresses);i++){
  SCNetworkReachabilityRef route=SCNetworkReachabilityCreateWithAddress(NULL,addresses[i]);
  SCNetworkReachabilityFlags flags=0;
  if(route){if(SCNetworkReachabilityGetFlags(route,&flags)&&(flags&kSCNetworkReachabilityFlagsReachable)&&!(flags&kSCNetworkReachabilityFlagsConnectionRequired))value|=1;CFRelease(route);}
 }
#pragma clang diagnostic pop
 SCDynamicStoreRef store=SCDynamicStoreCreate(NULL,CFSTR("MacLauncher Android connectivity"),NULL,NULL);
 if(store){
  CFStringRef keys[]={CFSTR("State:/Network/Global/IPv4"),CFSTR("State:/Network/Global/IPv6")};
  CFArrayRef interfaces=SCNetworkInterfaceCopyAll();
  for(unsigned k=0;k<sizeof(keys)/sizeof(*keys);k++){
   CFPropertyListRef state=SCDynamicStoreCopyValue(store,keys[k]);
   CFStringRef primary=state&&CFGetTypeID(state)==CFDictionaryGetTypeID()?CFDictionaryGetValue(state,kSCDynamicStorePropNetPrimaryInterface):NULL;
   if(primary&&interfaces)for(CFIndex i=0;i<CFArrayGetCount(interfaces);i++){
    SCNetworkInterfaceRef interface=(SCNetworkInterfaceRef)CFArrayGetValueAtIndex(interfaces,i);CFStringRef name=SCNetworkInterfaceGetBSDName(interface),type=SCNetworkInterfaceGetInterfaceType(interface);
    if(!name||!type||!CFEqual(name,primary))continue;
    if(CFEqual(type,kSCNetworkInterfaceTypeIEEE80211))value|=4;
    else if(CFEqual(type,kSCNetworkInterfaceTypeEthernet))value|=8;
    else if(CFEqual(type,kSCNetworkInterfaceTypeWWAN))value|=16|2;
   }
   if(state)CFRelease(state);
  }
  if(interfaces)CFRelease(interfaces);CFRelease(store);
 }
 return value;
}

static uint64_t packed(void){return revision<<32|(uint32_t)state;}
static void network_publish(int value,int force){
 pthread_mutex_lock(&lock);if(closed||(!force&&state==value)){pthread_mutex_unlock(&lock);return;}
 state=value;revision++;uint64_t snapshot=packed(),token=epoch;void(*notify)(uint64_t,uint64_t)=listener;
 pthread_cond_broadcast(&condition);pthread_mutex_unlock(&lock);if(notify)notify(token,snapshot);
}
static void network_dns_changed(SCDynamicStoreRef store,CFArrayRef keys,void*context){
 (void)store;(void)keys;(void)context;network_publish(initial_state(),1);
}
static int network_observe(void){pthread_mutex_lock(&lock);if(closed){pthread_mutex_unlock(&lock);return ECANCELED;}if(!monitor){
 state=initial_state();revision++;
 monitor=nw_path_monitor_create();queue=dispatch_queue_create("org.maclauncher.android.connectivity",DISPATCH_QUEUE_SERIAL);
 if(!monitor||!queue){if(monitor)nw_release(monitor);if(queue)dispatch_release(queue);monitor=NULL;queue=NULL;pthread_mutex_unlock(&lock);return ENOMEM;}
 nw_path_monitor_set_queue(monitor,queue);nw_path_monitor_set_update_handler(monitor,^(nw_path_t path){
  int value=(transports()<<8)|(nw_path_get_status(path)==nw_path_status_satisfied?1:0);
  if(nw_path_is_expensive(path)||nw_path_is_constrained(path))value|=2;
  if(nw_path_uses_interface_type(path,nw_interface_type_wifi))value|=4;
  if(nw_path_uses_interface_type(path,nw_interface_type_wired))value|=8;
  if(nw_path_uses_interface_type(path,nw_interface_type_cellular))value|=16;
  network_publish(value,0);
 });nw_path_monitor_start(monitor);
 dnsStore=SCDynamicStoreCreate(NULL,CFSTR("MacLauncher Android resolver"),network_dns_changed,NULL);
 if(dnsStore){const void*keys[]={CFSTR("State:/Network/Global/DNS")};const void*patterns[]={CFSTR("State:/Network/Service/.*/DNS")};
  CFArrayRef names=CFArrayCreate(NULL,keys,1,&kCFTypeArrayCallBacks),filters=CFArrayCreate(NULL,patterns,1,&kCFTypeArrayCallBacks);
  SCDynamicStoreSetNotificationKeys(dnsStore,names,filters);SCDynamicStoreSetDispatchQueue(dnsStore,queue);CFRelease(names);CFRelease(filters);
 }
}pthread_mutex_unlock(&lock);return 0;}
static void network_close(void){
 pthread_mutex_lock(&lock);closed=1;listener=NULL;epoch=0;nw_path_monitor_t previous=monitor;dispatch_queue_t previousQueue=queue;SCDynamicStoreRef previousStore=dnsStore;
 monitor=NULL;queue=NULL;dnsStore=NULL;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&lock);
 if(previousStore){SCDynamicStoreSetDispatchQueue(previousStore,NULL);CFRelease(previousStore);}
 if(previous){nw_path_monitor_cancel(previous);nw_release(previous);}if(previousQueue)dispatch_release(previousQueue);
}
/* Original resolver configuration only. No invented DNS addresses or fallback
 * to a public resolver, and no network probe or host name lookup. */
static inline char *network_resolver_configuration(void){
 SCDynamicStoreRef store=SCDynamicStoreCreate(NULL,CFSTR("MacLauncher Android resolver configuration"),NULL,NULL);if(!store){errno=EIO;return NULL;}
 CFMutableStringRef text=CFStringCreateMutable(NULL,0);if(!text){CFRelease(store);errno=ENOMEM;return NULL;}
 CFPropertyListRef value=SCDynamicStoreCopyValue(store,CFSTR("State:/Network/Global/DNS"));
 if(value&&CFGetTypeID(value)==CFDictionaryGetTypeID()){
  const CFStringRef keys[]={kSCPropNetDNSServerAddresses,kSCPropNetDNSSearchDomains};const CFStringRef kinds[]={CFSTR("server"),CFSTR("domain")};
  for(unsigned k=0;k<2;k++){CFArrayRef values=CFDictionaryGetValue(value,keys[k]);if(!values||CFGetTypeID(values)!=CFArrayGetTypeID())continue;
   for(CFIndex i=0;i<CFArrayGetCount(values);i++){CFStringRef item=CFArrayGetValueAtIndex(values,i);if(CFGetTypeID(item)!=CFStringGetTypeID())continue;
    CFCharacterSetRef separators=CFCharacterSetCreateWithCharactersInString(NULL,CFSTR("\t\n\r"));Boolean invalid=CFStringFindCharacterFromSet(item,separators,CFRangeMake(0,CFStringGetLength(item)),0,NULL);CFRelease(separators);if(!invalid)CFStringAppendFormat(text,NULL,CFSTR("%@\t%@\n"),kinds[k],item);
   }
  }
 }
 if(value)CFRelease(value);CFRelease(store);CFIndex size=CFStringGetMaximumSizeForEncoding(CFStringGetLength(text),kCFStringEncodingUTF8)+1;
 char*bytes=malloc((size_t)size);if(bytes&&!CFStringGetCString(text,bytes,size,kCFStringEncodingUTF8)){free(bytes);bytes=NULL;errno=EILSEQ;}CFRelease(text);return bytes;
}
