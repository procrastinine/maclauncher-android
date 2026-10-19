#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <dispatch/dispatch.h>
#include <vector>

// One physical callback produces; the device's reader mutex serializes callers.
// Slots belong to the producer until write is published, and to the reader
// until read is published. Neither side overwrites a slot the other is using.
class KljAudioInputFifo {
    const size_t capacity_,channels_;
    std::vector<float> samples_;
    std::atomic<uint64_t> read_{0},write_{0};
    dispatch_semaphore_t ready_=dispatch_semaphore_create(0);
    std::atomic<bool> notified_{false};
public:
    KljAudioInputFifo(size_t capacity,size_t channels):capacity_(capacity),channels_(channels),samples_(capacity*channels){}
    ~KljAudioInputFifo(){
#if !__has_feature(objc_arc)
        dispatch_release(ready_);
#endif
    }
    KljAudioInputFifo(const KljAudioInputFifo&)=delete;
    KljAudioInputFifo& operator=(const KljAudioInputFifo&)=delete;

    size_t push(const float* source,size_t frames){
        const uint64_t write=write_.load(std::memory_order_relaxed),read=read_.load(std::memory_order_acquire);
        const size_t count=std::min(frames,capacity_-size_t(write-read));
        if(!count)return 0;
        const size_t at=size_t(write%capacity_),first=std::min(count,capacity_-at);
        std::memcpy(samples_.data()+at*channels_,source,first*channels_*sizeof(float));
        if(first<count)std::memcpy(samples_.data(),source+first*channels_,(count-first)*channels_*sizeof(float));
        write_.store(write+count,std::memory_order_release);
        wake();
        return count;
    }
    size_t pop(float* output,size_t frames){
        const uint64_t read=read_.load(std::memory_order_relaxed),write=write_.load(std::memory_order_acquire);
        const size_t count=std::min(frames,size_t(write-read));
        if(!count)return 0;
        const size_t at=size_t(read%capacity_),first=std::min(count,capacity_-at);
        std::memcpy(output,samples_.data()+at*channels_,first*channels_*sizeof(float));
        if(first<count)std::memcpy(output+first*channels_,samples_.data(),(count-first)*channels_*sizeof(float));
        read_.store(read+count,std::memory_order_release);
        return count;
    }
    size_t queued() const {
        const uint64_t read=read_.load(std::memory_order_relaxed);
        return size_t(write_.load(std::memory_order_acquire)-read);
    }
    // A sticky single notification prevents lost wakeups without acquiring the
    // reader's mutex. There can never be more than one semaphore token pending.
    void wake(){if(!notified_.exchange(true,std::memory_order_acq_rel))dispatch_semaphore_signal(ready_);}
    void wait(){dispatch_semaphore_wait(ready_,DISPATCH_TIME_FOREVER);notified_.store(false,std::memory_order_release);}
    // Only control, with the callback stopped and the reader mutex held.
    void reset(){read_.store(0,std::memory_order_relaxed);write_.store(0,std::memory_order_relaxed);}
};
