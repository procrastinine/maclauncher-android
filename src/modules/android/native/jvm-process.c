#include <jni.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef jint (JNICALL *CreateVM)(JavaVM **, void **, void *);
typedef void (*ExitVM)(jint);
static _Atomic(CreateVM) create_vm;
static _Atomic(ExitVM) previous_exit;

/* HotSpot invokes its Invocation API exit option after Java shutdown hooks,
 * VMDeath notifications, its final safepoint and VM-global cleanup. Native
 * Android workers can still be running there: ART's JVM_Exit uses FastExit
 * for the same reason. Explicit guest libc exit/finalization stays separate. */
static void terminal_exit(jint status) {
    ExitVM previous = atomic_load_explicit(&previous_exit, memory_order_acquire);
    if (previous) previous(status);
    _exit(status);
}

static jint JNICALL create(JavaVM **vm, void **env, void *arguments) {
    CreateVM original = atomic_load_explicit(&create_vm, memory_order_acquire);
    if (!original) return JNI_ERR;
    if (!arguments) return original(vm, env, arguments);
    JavaVMInitArgs *input = arguments;
    if (input->version < JNI_VERSION_1_2 || input->nOptions < 0 ||
        input->nOptions == INT_MAX || (input->nOptions && !input->options))
        return original(vm, env, arguments);
    /* A launcher can itself delegate through libjli's exported JNI entry.
     * An already adapted argument vector keeps that delegation unchanged. */
    ExitVM prior = NULL;
    for (jint i = 0; i < input->nOptions; ++i) {
        if (input->options[i].optionString && !strcmp(input->options[i].optionString, "exit")) {
            prior = (ExitVM)input->options[i].extraInfo;
            if (prior == terminal_exit) return original(vm, env, arguments);
        }
    }
    size_t count = (size_t)input->nOptions + 1;
    if (count > SIZE_MAX / sizeof(JavaVMOption)) return JNI_ENOMEM;
    JavaVMOption *options = malloc(count * sizeof(*options));
    if (!options) return JNI_ENOMEM;
    if (input->nOptions) memcpy(options, input->options, input->nOptions * sizeof(*options));
    options[input->nOptions] = (JavaVMOption){"exit", (void *)terminal_exit};
    JavaVMInitArgs adapted = *input;
    adapted.nOptions++;
    adapted.options = options;
    ExitVM replaced = atomic_exchange_explicit(&previous_exit, prior, memory_order_acq_rel);
    jint result = original(vm, env, &adapted);
    if (result != JNI_OK)
        atomic_compare_exchange_strong_explicit(&previous_exit, &prior, replaced,
                                               memory_order_acq_rel, memory_order_acquire);
    free(options);
    return result;
}

/* JLI resolves JNI_CreateJavaVM with dlsym rather than a symbol relocation.
 * Adapt that exact Invocation API binding; every other lookup is returned
 * verbatim. dyld excludes this image's own dlsym call from interposition. */
static void *lookup(void *handle, const char *name) {
    void *result = dlsym(handle, name);
    if (result && name && !strcmp(name, "JNI_CreateJavaVM")) {
        atomic_store_explicit(&create_vm, (CreateVM)result, memory_order_release);
        return (void *)create;
    }
    return result;
}
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } mapping = {
    (const void *)lookup, (const void *)dlsym
};
