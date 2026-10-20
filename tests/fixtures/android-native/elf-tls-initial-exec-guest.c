__thread int ie_initialized=37;
__thread unsigned char ie_zero[73] __attribute__((aligned(256)));
__thread int ie_hidden __attribute__((visibility("hidden")))=11;
extern __thread int tls_dependency;
extern __thread int ie_weak __attribute__((weak));

int *ie_address(void){return &ie_initialized;}
int *ie_hidden_address(void){return &ie_hidden;}
int *ie_dependency_address(void){return &tls_dependency;}
int *ie_weak_address(void){return &ie_weak;}
unsigned char *ie_zero_address(void){return ie_zero;}
void *ie_thread_pointer(void){void *value;__asm__("mrs %0,tpidr_el0":"=r"(value));return value;}
