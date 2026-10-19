#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

namespace klj_timing {
// Fixed-size counters only: no frame allocation, sorting, sampling or GPU wait.
struct Window {
    uint64_t began=0,previous=0,frames=0,intervals=0,intervalNs=0,maxIntervalNs=0;
    uint64_t submitNs=0,maxSubmitNs=0;
    std::array<uint32_t,256> milliseconds{};
    void record(uint64_t start,uint64_t end) noexcept {
        if(!began)began=start;
        if(previous&&start>=previous){
            const auto interval=start-previous;
            ++intervals;intervalNs+=interval;maxIntervalNs=std::max(maxIntervalNs,interval);
            ++milliseconds[std::min<uint64_t>(interval/1000000,milliseconds.size()-1)];
        }
        previous=start;++frames;
        const auto elapsed=end>=start?end-start:0;
        submitNs+=elapsed;maxSubmitNs=std::max(maxSubmitNs,elapsed);
    }
    bool due(uint64_t now) const noexcept {return began&&now>=began&&now-began>=1000000000;}
    double fps() const noexcept {return intervalNs?double(intervals)*1e9/intervalNs:0;}
    double percentile(double fraction) const noexcept {
        if(!intervals)return 0;
        const auto threshold=std::max<uint64_t>(1,uint64_t(double(intervals)*fraction+.999999));
        uint64_t count=0;
        for(std::size_t i=0;i<milliseconds.size();++i)if((count+=milliseconds[i])>=threshold)
            return i+1==milliseconds.size()?double(maxIntervalNs)/1e6:double(i+1);
        return 0;
    }
    void reset(uint64_t now) noexcept {
        began=now;frames=intervals=intervalNs=maxIntervalNs=submitNs=maxSubmitNs=0;
        milliseconds.fill(0);
    }
};
}
