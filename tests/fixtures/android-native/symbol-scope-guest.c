#include <stddef.h>
#if defined(ROOT_IMAGE)
extern void *malloc(size_t);
extern void free(void *);
int root_value(void){void *bytes=malloc(64);if(!bytes)return -1;free(bytes);return 47;}
int precedence(void){return 101;}
__attribute__((visibility("hidden"))) int hidden_value(void){return 99;}
#elif defined(DEPENDENCY_A)
int dependency_a(void){return 21;}
int precedence(void){return 201;}
#elif defined(DEPENDENCY_B)
int dependency_b(void){return 22;}
int breadth_precedence(void){return 222;}
#elif defined(DEEP_IMAGE)
int deep_value(void){return 31;}
int breadth_precedence(void){return 333;}
#elif defined(UNRELATED_IMAGE)
int unrelated_value(void){return 77;}
#else
int bare_value(void){return 17;}
#endif
