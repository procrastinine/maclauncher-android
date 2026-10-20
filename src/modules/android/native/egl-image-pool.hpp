#pragma once
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace klj_egl {
// Backing images belong to the existing EGL window generation. A submitted
// image remains unavailable until its presentation/readback lease retires.
template<class Image> class ImagePool {
    struct Admission {std::mutex mutex;std::condition_variable released;};
    struct Lease {
        std::shared_ptr<Admission> admission;
        std::shared_ptr<Image> image;
        Lease(std::shared_ptr<Admission> gate,std::shared_ptr<Image> value):admission(std::move(gate)),image(std::move(value)){}
        ~Lease(){std::lock_guard lock(admission->mutex);image.reset();admission->released.notify_all();}
    };
    std::shared_ptr<Admission> admission=std::make_shared<Admission>();
public:
    std::array<std::shared_ptr<Image>,3> images;
    template<class Create> int next(int current,Create create){
        std::unique_lock lock(admission->mutex);
        auto available=[&]{for(int i=0;i<3;++i)if(i!=current&&(!images[i]||images[i].use_count()==1))return i;return -1;};
        int slot=available();
        if(slot<0){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            if(!admission->released.wait_until(lock,deadline,[&]{return (slot=available())>=0;}))return -1;}
        if(!images[slot])images[slot]=create();
        return images[slot]?slot:-1;
    }
    std::shared_ptr<void> retain(int slot){return std::make_shared<Lease>(admission,images[slot]);}
};
}
