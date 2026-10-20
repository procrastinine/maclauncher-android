#include <jni.h>
#include <jvmti.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void host_finalizer(void) { puts("HOST_FINALIZER"); fflush(stdout); }

#ifdef PROCESS_AGENT
static void JNICALL death(jvmtiEnv *tooling, JNIEnv *env) {
    (void)tooling; (void)env;
    puts("VM_DEATH"); fflush(stdout);
}
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *options, void *reserved) {
    (void)options; (void)reserved;
    jvmtiEnv *tooling = NULL;
    if ((*vm)->GetEnv(vm, (void **)&tooling, JVMTI_VERSION_1_2) != JNI_OK) return JNI_ERR;
    jvmtiEventCallbacks callbacks = {.VMDeath = death};
    if ((*tooling)->SetEventCallbacks(tooling, &callbacks, sizeof callbacks) != JVMTI_ERROR_NONE ||
        (*tooling)->SetEventNotificationMode(tooling, JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, NULL) != JVMTI_ERROR_NONE)
        return JNI_ERR;
    atexit(host_finalizer);
    return JNI_OK;
}
#else
typedef jint (JNICALL *CreateVM)(JavaVM **, void **, void *);
static void existing_exit(jint status) { printf("EXISTING_EXIT %d\n", status); fflush(stdout); }
static void failed_exit(jint status) { (void)status; puts("FAILED_CREATE_EXIT"); fflush(stdout); }
int main(int argc, char **argv) {
    atexit(host_finalizer);
    if (argc == 2 && !strcmp(argv[1], "native")) exit(23);
    if (argc != 4) return 2;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!library) { fputs(dlerror(), stderr); return 3; }
    CreateVM create = (CreateVM)dlsym(library, "JNI_CreateJavaVM");
    if (!create) return 4;
    JavaVMOption options[] = {{argv[2], NULL}, {argv[3], NULL}, {"exit", (void *)existing_exit}};
    JavaVMInitArgs arguments = {JNI_VERSION_1_8, 3, options, JNI_FALSE};
    JavaVM *vm; JNIEnv *env;
    if (create(&vm, (void **)&env, &arguments) != JNI_OK) return 5;
    JavaVMOption failed = {"exit", (void *)failed_exit};
    JavaVMInitArgs duplicate = {JNI_VERSION_1_8, 1, &failed, JNI_FALSE};
    JavaVM *other; void *other_env;
    if (create(&other, &other_env, &duplicate) != JNI_EEXIST) return 6;
    jclass type = (*env)->FindClass(env, "ExitProbe");
    jmethodID method = type ? (*env)->GetStaticMethodID(env, type, "main", "([Ljava/lang/String;)V") : NULL;
    if (!method) return 7;
    (*env)->CallStaticVoidMethod(env, type, method, NULL);
    return 8;
}
#endif
