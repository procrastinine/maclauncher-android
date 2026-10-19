#pragma once
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#ifdef KLJ_BINDER_DESCRIPTOR_OWNER
#include "../pollable-fds/bridge.h"
#define close klj_pollable_close
#define fcntl klj_pollable_fcntl
#define dup klj_pollable_dup
#define dup2 klj_pollable_dup2
#endif
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expression) ({ decltype(expression) _binder_result; do { _binder_result=(expression); } while (_binder_result == -1 && errno == EINTR); _binder_result; })
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x80000
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x800
#endif
extern "C" int klj_binder_socket(int,int,int);
extern "C" int klj_binder_socketpair(int,int,int,int*);
extern "C" int klj_binder_accept4(int,struct sockaddr*,socklen_t*,int);
#ifndef KLJ_BINDER_DARWIN_IMPLEMENTATION
#define socket klj_binder_socket
#define socketpair klj_binder_socketpair
#define accept4 klj_binder_accept4
#endif
