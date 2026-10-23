#pragma once
#include <stdint.h>
typedef struct {uint32_t size,version;int64_t(*borrow)(int,int*,void**);int64_t(*release)(void*);} KljBionicPollFdApi;
/* Tokens never enter guest packets. The trusted native owner exclusively owns
 * one token until release, which consumes it exactly once. Borrowed FD must
 * not be closed; no descriptor duplication or SCM receipt decoding here. */
