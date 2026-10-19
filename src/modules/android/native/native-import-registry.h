#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef void *(*KljImportResolver)(const char *name);
/* Resolvers answer only their own exports. NULL continues the one registry.
 * Registration is idempotent; the most recently added distinct owner wins. */
int klj_register_import_resolver(KljImportResolver resolver);
void *klj_resolve_import(const char *name);
#ifdef __cplusplus
}
#endif
