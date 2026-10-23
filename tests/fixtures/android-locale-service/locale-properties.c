#include <jni.h>
#include <dlfcn.h>
/* Fixture-only selection of the original optional IME-query policy. The real
 * property owner is reused; production Java's read-only boundary is untouched. */
JNIEXPORT void JNICALL Java_maclauncher_android_framework_LocaleServiceProbe_imeQuery(JNIEnv *env,jclass type,jboolean enabled){
    (void)type;
    void *(*lookup)(const char *)=dlsym(RTLD_DEFAULT,"kl_shim_lookup");
    int (*set)(const char *,const char *)=lookup?lookup("__system_property_set"):NULL;
    if(!set||set("i18n.feature.allow_ime_query_app_locale",enabled?"true":"false")){
        jclass error=(*env)->FindClass(env,"java/lang/AssertionError");if(error)(*env)->ThrowNew(env,error,"Original property policy could not be selected");
    }
}
