#ifndef MACLAUNCHER_NATIVE_RESOLVER_IMPORTS_H
#define MACLAUNCHER_NATIVE_RESOLVER_IMPORTS_H
// Bionic exposes resolver error state per thread. Reuse Darwin's resolver and
// copy its result before releasing the shared legacy resolver lock.
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "network-policy.h"
static pthread_mutex_t klj_resolver_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct {
    int error;struct hostent result;char bytes[16384];
    char *aliases[129],*addresses[129];size_t used;
} klj_resolver;
static int *klj_get_h_errno(void){return &klj_resolver.error;}
static char *klj_host_bytes(const void *bytes,size_t length){
    if(length>sizeof klj_resolver.bytes-klj_resolver.used)return NULL;
    char *out=klj_resolver.bytes+klj_resolver.used;memcpy(out,bytes,length);klj_resolver.used+=length;return out;
}
static char *klj_host_string(const char *value){
    if(!value)value="";size_t length=strnlen(value,sizeof klj_resolver.bytes);
    return length==sizeof klj_resolver.bytes?NULL:klj_host_bytes(value,length+1);
}
static struct hostent *klj_host_copy(struct hostent *source){
    klj_resolver.error=h_errno;
    if(!source)return NULL;
    klj_resolver.used=0;struct hostent *out=&klj_resolver.result;
    out->h_name=klj_host_string(source->h_name);if(!out->h_name)goto full;
    out->h_addrtype=source->h_addrtype==AF_INET6?10:source->h_addrtype;
    out->h_length=source->h_length;out->h_aliases=klj_resolver.aliases;out->h_addr_list=klj_resolver.addresses;
    if(source->h_length!=4&&source->h_length!=16)goto full;
    size_t n=0;
    for(;source->h_aliases&&source->h_aliases[n];++n){if(n==128)goto full;out->h_aliases[n]=klj_host_string(source->h_aliases[n]);if(!out->h_aliases[n])goto full;}
    out->h_aliases[n]=NULL;
    for(n=0;source->h_addr_list&&source->h_addr_list[n];++n){if(n==128)goto full;out->h_addr_list[n]=klj_host_bytes(source->h_addr_list[n],(size_t)source->h_length);if(!out->h_addr_list[n])goto full;}
    out->h_addr_list[n]=NULL;return out;
full:errno=ERANGE;klj_resolver.error=NETDB_INTERNAL;return NULL;
}
static int klj_resolver_family(int family){return family==2?AF_INET:family==10?AF_INET6:-1;}
// Addresses are byte-compatible; the family argument is not. Use the same
// translation as forward/reverse DNS before entering the host codec.
static int klj_inet_pton(int family,const char *text,void *address){
    int host=klj_resolver_family(family);if(host<0){errno=EAFNOSUPPORT;return -1;}
    return inet_pton(host,text,address);
}
static const char *klj_inet_ntop(int family,const void *address,char *text,socklen_t length){
    int host=klj_resolver_family(family);if(host<0){errno=EAFNOSUPPORT;return NULL;}
    return inet_ntop(host,address,text,length);
}
static struct hostent *klj_gethostbyname2(const char *name,int family){
    int host=klj_resolver_family(family);
    if(!name||host<0){errno=host<0?EAFNOSUPPORT:EINVAL;klj_resolver.error=NETDB_INTERNAL;return NULL;}
    if(!klj_network_host_allowed(name)){klj_resolver.error=HOST_NOT_FOUND;return NULL;}
    pthread_mutex_lock(&klj_resolver_lock);struct hostent *answer=klj_host_copy(gethostbyname2(name,host));int error=errno;
    pthread_mutex_unlock(&klj_resolver_lock);errno=error;return answer;
}
static struct hostent *klj_gethostbyname(const char *name){return klj_gethostbyname2(name,2);}
static struct hostent *klj_gethostbyaddr(const void *address,socklen_t length,int family){
    int host=klj_resolver_family(family);
    if(!address||host<0||length!=(socklen_t)(host==AF_INET?4:16)){errno=host<0?EAFNOSUPPORT:EINVAL;klj_resolver.error=NETDB_INTERNAL;return NULL;}
    pthread_mutex_lock(&klj_resolver_lock);struct hostent *answer=klj_host_copy(gethostbyaddr(address,length,host));int error=errno;
    pthread_mutex_unlock(&klj_resolver_lock);errno=error;return answer;
}
static void klj_herror(const char *prefix){fprintf(stderr,"%s%s%s\n",prefix?prefix:"",prefix&&*prefix?": ":"",hstrerror(klj_resolver.error));}
static int klj_filtered_getaddrinfo(const char *node,const char *service,const void *hints,void *result){
    if(!klj_network_host_allowed(node)){if(result)*(void **)result=NULL;return EAI_NONAME;}
    // Bionic's addrinfo layout matches Darwin's, but AF_INET6 and
    // AI_NUMERICSERV do not. Klepton converts the nested sockaddr storage;
    // translate the enclosing metadata on the same allocation so its original
    // freeaddrinfo owner remains authoritative.
    struct addrinfo host_hints;
    if(hints){
        host_hints=*(const struct addrinfo *)hints;
        switch(host_hints.ai_family){
            case 0:host_hints.ai_family=AF_UNSPEC;break;
            case 2:host_hints.ai_family=AF_INET;break;
            case 10:host_hints.ai_family=AF_INET6;break;
            default:if(result)*(void **)result=NULL;return EAI_FAMILY;
        }
        // Do not interpret Darwin-only flag bits as a different Android API.
        // These are all AI_* flags declared by Bionic's netdb.h.
        if(host_hints.ai_flags&~0x0f0f){if(result)*(void **)result=NULL;return EAI_BADFLAGS;}
        if(host_hints.ai_flags&8)host_hints.ai_flags=(host_hints.ai_flags&~8)|AI_NUMERICSERV;
        hints=&host_hints;
    }
    int error=kl_getaddrinfo(node,service,hints,result);
    if(!error&&result)for(struct addrinfo *at=*(struct addrinfo **)result;at;at=at->ai_next){
        if(at->ai_family==AF_INET6)at->ai_family=10;
        if(at->ai_flags&AI_NUMERICSERV)at->ai_flags=(at->ai_flags&~AI_NUMERICSERV)|8;
    }
    return error;
}
static int klj_android_getaddrinfofornet(const char *node,const char *service,const void *hints,unsigned network,unsigned mark,void *result){
    (void)network;(void)mark;return klj_filtered_getaddrinfo(node,service,hints,result);
}
static int klj_getnameinfo(const struct sockaddr *address,socklen_t length,char *host,size_t host_length,char *service,size_t service_length,int flags){
    // Android has a 16-bit family at byte zero; Darwin stores length/family as
    // separate bytes. Forward resolution returns Android addresses, so reverse
    // resolution must cross the same ABI boundary without changing that list.
    if(!address||length<sizeof(uint16_t))return EAI_FAMILY;
    uint16_t family;memcpy(&family,address,sizeof family);
    int native_family=klj_resolver_family(family);
    socklen_t native_length=native_family==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6);
    if(native_family<0||length<native_length)return EAI_FAMILY;
    const int guest_flags[]={1,2,4,8,16};
    const int native_flags[]={NI_NOFQDN,NI_NUMERICHOST,NI_NAMEREQD,NI_NUMERICSERV,NI_DGRAM};
    int remaining=flags,native_flags_value=0;
    for(size_t i=0;i<sizeof guest_flags/sizeof guest_flags[0];i++)if(remaining&guest_flags[i]){
        native_flags_value|=native_flags[i];remaining&=~guest_flags[i];
    }
    if(remaining)return EAI_BADFLAGS;
    struct sockaddr_storage native_address={0};memcpy(&native_address,address,native_length);
    native_address.ss_len=(uint8_t)native_length;native_address.ss_family=(sa_family_t)native_family;
    // Bionic accidentally published size_t for both output lengths. Darwin's
    // socklen_t is narrower; a large valid buffer must not wrap to a small one.
    socklen_t host_capacity=host_length>UINT_MAX?UINT_MAX:(socklen_t)host_length;
    socklen_t service_capacity=service_length>UINT_MAX?UINT_MAX:(socklen_t)service_length;
    return getnameinfo((const struct sockaddr *)&native_address,native_length,host,host_capacity,service,service_capacity,native_flags_value);
}
static struct hostent *klj_android_gethostbynamefornet(const char *name,int family,unsigned network,unsigned mark){
    (void)network;(void)mark;return klj_gethostbyname2(name,family);
}
static void *klj_resolver_lookup(const char *name){
    if(!strcmp(name,"inet_pton"))return (void *)klj_inet_pton;
    if(!strcmp(name,"inet_ntop"))return (void *)klj_inet_ntop;
    if(!strcmp(name,"getaddrinfo"))return (void *)klj_filtered_getaddrinfo;
    if(!strcmp(name,"getnameinfo"))return (void *)klj_getnameinfo;
    if(!strcmp(name,"android_getaddrinfofornet"))return (void *)klj_android_getaddrinfofornet;
    if(!strcmp(name,"android_gethostbynamefornet"))return (void *)klj_android_gethostbynamefornet;
    if(!strcmp(name,"__get_h_errno"))return (void *)klj_get_h_errno;
    if(!strcmp(name,"gethostbyname"))return (void *)klj_gethostbyname;
    if(!strcmp(name,"gethostbyname2"))return (void *)klj_gethostbyname2;
    if(!strcmp(name,"gethostbyaddr"))return (void *)klj_gethostbyaddr;
    if(!strcmp(name,"herror"))return (void *)klj_herror;
    return NULL;
}
#endif
