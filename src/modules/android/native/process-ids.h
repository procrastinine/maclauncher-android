#pragma once
#include <sys/types.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One profile namespace, shared by Android process and thread IDs. The kernel
 * owns each lease; its descriptor stays open until that identity retires. */
enum { KLJ_PROCESS_ID_MIN=2, KLJ_PROCESS_ID_MAX=65535 };
/* A VM adopts newly allocated descriptors through its existing private FD
 * owner, before any lease locks or borrowed references exist. adopt consumes
 * its input and returns the final owned descriptor, or closes it on failure.
 * Published/locked descriptors never pass through adopt again. */
typedef struct {void *(*enter)(void);int (*adopt)(int);void (*closed)(int);void (*leave)(void *);} KljProcessDescriptors;
int klj_process_descriptors_owned(const KljProcessDescriptors *);
/* Duplicate a physical descriptor through that same private allocator. */
int klj_process_descriptor_duplicate(int);
int klj_process_ids_configure(const char *directory);
int klj_process_id(void);
int klj_process_parent_id(void);
/* Physical exec retains these existing kernel leases. Descriptors are borrowed
 * until exec succeeds; a failed exec must restore their original FD flags. */
typedef struct {int32_t id,descriptor,child;} KljProcessExecLease;
typedef struct {int32_t directory,leader,parent,host_parent;uint32_t count;} KljProcessExecIdentity;
int klj_process_exec_snapshot(KljProcessExecIdentity *,KljProcessExecLease **);
int klj_process_exec_adopt(const KljProcessExecIdentity *,const KljProcessExecLease *);
typedef struct {int (*prepare)(void);int (*child)(int);} KljProcessForkTransport;
int klj_process_fork_transport(const KljProcessForkTransport *);
pid_t klj_process_fork(void);
/* Retained physical children only; self and nonpositive PIDs have no child ID. */
int klj_process_child_id(pid_t host);
/* Retain an already-started physical child before its startup ACK. The live
 * child keeps byte zero; this parent keeps the zombie reservation byte. */
int klj_process_child_retain(pid_t host);
int klj_process_child_reaped(pid_t host);
pid_t klj_process_child_host(int logical_id);
/* Authenticated principal and signal state live on the same kernel lease. */
int klj_process_publish_principal(uid_t uid);
/* Command metadata belongs to the same live process lease, never host argv or
 * an independently retained process catalog. Returned IDs are Android IDs. */
int klj_process_publish_name(const char *name);
int klj_process_find_commands(const char *const *names,size_t count,int **pids,size_t *length);
int klj_process_publish_security(int id,const void *value,size_t length);
ssize_t klj_process_read_security(int id,void *value,size_t length);
int klj_process_signal_owner(int id,pid_t *host,int *kind,int *process,uid_t *uid);
/* CPU accounting in nanoseconds for a logical process (zero means self).
 * Sampling retains its existing lease, never accepts a physical host PID. */
int klj_process_cpu_time(int id,uint64_t *user,uint64_t *system);
int klj_process_signal_targets(pid_t selector,int (*send)(int,void *),void *context);
/* Native wait signal encoding; the RT range (32..64) has no Darwin alias. */
__attribute__((noreturn)) void klj_process_signal_exit(int wait_signal);
int klj_process_child_signal(pid_t host,int native_signal);
pid_t klj_process_getpgid(pid_t id);
pid_t klj_process_getsid(pid_t id);
int klj_process_setpgid(pid_t id,pid_t group);
pid_t klj_process_setsid(void);
int klj_process_signal(pid_t id,int native_signal);
int klj_process_group_signal(pid_t group,int native_signal);
/* Native wait status/siginfo layout; every returned PID is Android logical. */
pid_t klj_process_wait_native(pid_t selector,int *status,int options,struct rusage *usage);
int klj_process_waitid_native(idtype_t type,id_t id,siginfo_t *info,int options);
int klj_thread_id_allocate(void);
void klj_thread_id_release(int id);
/* Inspect a lease without opening/closing an inode already locked by this
 * process. kind is 1 for a process leader, 2 for a worker thread. */
int klj_process_id_owner(const char *directory,int id,pid_t *host_pid,int *kind,int *process_id);
/* Explicit reverse lookup of a live process leader. Host PIDs and guest IDs
 * are separate domains, even when their integer values happen to coincide. */
int klj_process_id_from_host(pid_t host_pid);
int klj_process_id_query(int id,pid_t *host_pid,int *kind,int *process_id);
#ifdef __cplusplus
}
#endif
