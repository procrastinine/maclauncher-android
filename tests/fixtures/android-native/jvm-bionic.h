#pragma once
#include <stdint.h>
typedef struct {uint64_t dispatcher,check,expected_getpid,expected_errno,thread,depth,checks;} FixtureRequest;
typedef struct {uint64_t getpid_function,errno_function,mmap_function,munmap_function,pthread_self_function,nested,checks;} FixtureCall;
