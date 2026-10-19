#pragma once
#include <time.h>
#include <cstdint>
namespace klj_target {
// A factual marker is required in addition to checking the actual resolved
// guest function. Zero/nearby boot offsets alone cannot identify clock semantics.
inline bool clockContract(int(*marker)(),int(*guest)(int,struct timespec*),uint64_t(*uptime)()){
 if(!marker||!guest||!uptime||marker()!=1)return false;
 uint64_t before=uptime();timespec now{};int result=guest(1,&now);uint64_t after=uptime();
 if(result||now.tv_sec<0||now.tv_nsec<0||now.tv_nsec>=1000000000L||after<before||after-before>100000000ULL)return false;
 uint64_t sec=uint64_t(now.tv_sec);if(sec>(UINT64_MAX-uint64_t(now.tv_nsec))/1000000000ULL)return false;
 uint64_t ns=sec*1000000000ULL+uint64_t(now.tv_nsec);
 return ns>=before&&ns<=after;
}
}
