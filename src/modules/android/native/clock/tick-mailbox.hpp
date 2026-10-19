#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
namespace klclock {
struct Tick {uint64_t callback,output,interval;};
/* One CoreVideo writer, independent readers with their own cursors. Every field is atomic: ordinary seqlock
 * non-atomic payload fields would still be a C++ data race. Reader retries are
 * bounded; the realtime writer never waits or allocates. */
struct Mailbox {
    std::atomic<uint64_t> version{0},callback{0},output{0},interval{0};
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    void publish(Tick tick) noexcept {
        version.fetch_add(1,std::memory_order_seq_cst);
        callback.store(tick.callback,std::memory_order_seq_cst);
        output.store(tick.output,std::memory_order_seq_cst);
        interval.store(tick.interval,std::memory_order_seq_cst);
        version.fetch_add(1,std::memory_order_seq_cst);
    }
    bool read(uint64_t& consumed,Tick& tick,uint64_t& skipped) const noexcept {
        for(int tries=0;tries<3;++tries){
            uint64_t before=version.load(std::memory_order_seq_cst);if(before&1)continue;
            if(before==consumed||before==0)return false;
            Tick value{callback.load(std::memory_order_seq_cst),output.load(std::memory_order_seq_cst),interval.load(std::memory_order_seq_cst)};
            uint64_t after=version.load(std::memory_order_seq_cst);
            if(before!=after)continue;
            skipped=before>=consumed?(before-consumed)/2-1:0;consumed=before;tick=value;return true;
        }
        return false;
    }
    void discard(uint64_t& consumed)const noexcept {consumed=version.load(std::memory_order_seq_cst)&~uint64_t(1);}
};
inline uint64_t scale(uint64_t value,uint32_t numerator,uint32_t denominator)noexcept{
    if(!denominator)return 0;__uint128_t result=static_cast<__uint128_t>(value)*numerator/denominator;
    return result>static_cast<__uint128_t>(std::numeric_limits<int64_t>::max())?0:static_cast<uint64_t>(result);
}
}
