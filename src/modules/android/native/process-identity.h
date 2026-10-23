#pragma once
#include <sys/types.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
uid_t klj_process_uid(void);
gid_t klj_process_gid(void);
int klj_process_setuid(uid_t);
int klj_process_setgid(gid_t);
int klj_process_getgroups(int,gid_t *);
int klj_process_setgroups(size_t,const gid_t *);
uid_t klj_process_uid_to_host(uid_t);
gid_t klj_process_gid_to_host(gid_t);
uid_t klj_process_uid_from_host(uid_t);
gid_t klj_process_gid_from_host(gid_t);
void *klj_process_identity_symbol(const char *);
int klj_process_name(const char *);
/* 0: unrelated path; 1: read-only snapshot published; -1: publication error. */
int klj_process_cmdline(const char *,char *,size_t);
#ifdef __cplusplus
}
#endif

/* Shared original Bionic account database; no Darwin account lookup. */
#ifdef __cplusplus
extern "C" {
#endif
int klj_database_name_id(const char *, int);
#ifdef __cplusplus
}
#endif
