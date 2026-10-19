#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KLVariadicHandler { void *target;unsigned fixed; } KLVariadicHandler;
const KLVariadicHandler *klj_variadic_handler(const void *entry);
#ifdef __cplusplus
}
#endif
