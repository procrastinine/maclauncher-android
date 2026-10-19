/** Preserve Klepton's per-thread ABI setup and own host-created callback threads. */
export function adaptGuestThreadEntry(source) {
 const from='void kl_thread_init(void) {\n    uint64_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));\n    ((uint64_t *)tp)[TLS_SLOT_STACK_GUARD] = g_canary;\n}';
 if(source.split(from).length!==2)throw Error('Native thread entry boundary changed');
 return source.replace(from,'#include <stdio.h>\n#include <stdlib.h>\nextern void klj_enter_guest_thread(void);\nextern int klj_guest_thread_pointer_enter(unsigned long long);\nvoid kl_thread_init(void) {\n    klj_enter_guest_thread();\n    if(klj_guest_thread_pointer_enter(g_canary)) { perror("Android native thread pointer"); abort(); }\n}');
}
