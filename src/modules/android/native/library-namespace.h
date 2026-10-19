#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KljLibraryNamespaces KljLibraryNamespaces;
typedef struct KljLibraryNamespace KljLibraryNamespace;
/* Namespace handles belong to the existing native image registry. This
 * component owns search/access policy, never another ELF or platform loader. */
KljLibraryNamespaces *klj_namespaces_create(void);
void klj_namespaces_destroy(KljLibraryNamespaces *);
KljLibraryNamespace *klj_namespace_create(KljLibraryNamespaces *,const char *name,
    const char *library_path,const char *default_path,uint64_t original_type,
    const char *permitted_path,KljLibraryNamespace *parent,void *native_owner);
int klj_namespace_paths(KljLibraryNamespace *,const char *library_path,const char *default_path);
int klj_namespace_link(KljLibraryNamespace *,KljLibraryNamespace *,const char *sonames,int allow_all);
void klj_namespace_export(KljLibraryNamespace *);
KljLibraryNamespace *klj_namespace_exported(KljLibraryNamespaces *,const char *name);
void *klj_namespace_native(KljLibraryNamespace *);
int klj_namespace_accessible(KljLibraryNamespace *,const char *canonical_path);
/* Candidate returns the real path in output; absent candidates return zero.
 * RUNPATH is already expanded by the original image metadata owner. Linked
 * namespaces are searched once, with their declared soname relation. */
typedef int (*KljLibraryCandidate)(void *,const char *,char *,size_t);
KljLibraryNamespace *klj_namespace_resolve(KljLibraryNamespace *,const char *name,
    const char *runpath,KljLibraryCandidate,void *,char *output,size_t capacity);
KljLibraryNamespace *klj_namespace_resolve_from(KljLibraryNamespace *,const char *name,
    const char *requester,const char *runpath,unsigned elf_class,KljLibraryCandidate,
    void *,char *output,size_t capacity);
/* Membership refers to the registry's existing image handles. Shared namespace
 * creation snapshots parent membership; links do not copy ownership. */
int klj_namespace_publish(KljLibraryNamespace *,void *image,int global);
int klj_namespace_contains(KljLibraryNamespace *,void *image);
int klj_namespace_global(KljLibraryNamespace *,void *image);
const char *klj_namespace_error(void);
#ifdef __cplusplus
}
#endif
