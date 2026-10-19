#pragma once
#include <cstdint>
#include <mutex>
namespace klj_target {
// One state per existing presenter; no process map or per-swap allocation.
class Surface {
 mutable std::mutex mutex;
 void* display=nullptr;void* surface=nullptr;
 bool set=false;int64_t nanos=0;
public:
 void bind(void*d,void*s){std::lock_guard lock(mutex);display=d;surface=s;set=false;nanos=0;}
 void retire(){bind(nullptr,nullptr);}
 unsigned replace(void*d,void*s,int64_t value){std::lock_guard lock(mutex);if(!d||display!=d)return 0x3008;if(!s||surface!=s)return 0x300D;nanos=value;set=true;return 0;}
 bool read(int64_t&value)const{std::lock_guard lock(mutex);if(!set)return false;value=nanos;return true;}
};
}
