#include "network-policy.h"
#include "ad-hosts.h"
#include <jni.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_int ads=ATOMIC_VAR_INIT(-1);
bool klj_network_configure_ads(bool allowed){
    int expected=-1,value=allowed?1:0;
    return atomic_compare_exchange_strong_explicit(&ads,&expected,value,memory_order_release,memory_order_acquire)||expected==value;
}
bool klj_network_ads_allowed(void){return atomic_load_explicit(&ads,memory_order_acquire)==1;}
const char *const *klj_network_ad_hosts(size_t *count){*count=sizeof klj_ad_hosts/sizeof *klj_ad_hosts;return klj_ad_hosts;}
static int compare_host(const void *value,const void *entry){return strcmp(value,*(const char *const *)entry);}
bool klj_network_host_allowed(const char *hostname){
    if(klj_network_ads_allowed()||!hostname)return true;
    size_t length=strlen(hostname);while(length&&hostname[length-1]=='.')length--;
    // Invalid/oversized hostnames retain the platform resolver's own error.
    char normalized[254];if(!length||length>=sizeof normalized)return true;
    for(size_t i=0;i<length;i++){unsigned char c=(unsigned char)hostname[i];normalized[i]=(char)(c>='A'&&c<='Z'?c+('a'-'A'):c);}
    normalized[length]=0;size_t count=sizeof klj_ad_hosts/sizeof *klj_ad_hosts;
    for(const char *suffix=normalized;suffix;){
        if(bsearch(suffix,klj_ad_hosts,count,sizeof *klj_ad_hosts,compare_host))return false;
        suffix=strchr(suffix,'.');if(suffix)suffix++;
    }
    return true;
}
JNIEXPORT void JNICALL Java_maclauncher_android_net_HostNetworkPolicy_configureNative(JNIEnv *env,jclass owner,jboolean allowed){
    (void)owner;if(klj_network_configure_ads(allowed))return;
    jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");if(type){(*env)->ThrowNew(env,type,"Android advertising policy cannot change during a session");(*env)->DeleteLocalRef(env,type);}
}
JNIEXPORT jboolean JNICALL Java_maclauncher_android_net_HostNetworkPolicy_hostAllowedNative(JNIEnv *env,jclass owner,jstring host){
    (void)owner;if(!host)return JNI_TRUE;const char *text=(*env)->GetStringUTFChars(env,host,NULL);if(!text)return JNI_FALSE;
    bool allowed=klj_network_host_allowed(text);(*env)->ReleaseStringUTFChars(env,host,text);return allowed?JNI_TRUE:JNI_FALSE;
}
