#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <unistd.h>

static int failCreate = -1;
static void commonPoolTestBeforeCreate(int index) {
    if (index == failCreate) {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
}
#define private public
#include "thread/CommonPool.h"
#undef private
#include "thread/CommonPool.cpp"

using android::uirenderer::CommonPool;
using namespace std::chrono_literals;

struct Gate {
    std::mutex lock;
    std::condition_variable condition;
    int entered = 0;
    bool open = false;
    void block() {
        std::unique_lock hold(lock);
        ++entered;
        condition.notify_all();
        condition.wait(hold, [&]{ return open; });
    }
    void waitForWorkers() {
        std::unique_lock hold(lock);
        assert(condition.wait_for(hold, 5s, [&]{ return entered == CommonPool::THREAD_COUNT; }));
    }
    void release() {
        std::lock_guard hold(lock);
        open = true;
        condition.notify_all();
    }
};

static void layout() {
    printf("{\"size\":%zu,\"align\":%zu,\"mutex\":%zu,\"condition\":%zu,\"waiting\":%zu,\"queue\":%zu,\"stopping\":%zu}\n",
        sizeof(CommonPool), alignof(CommonPool), offsetof(CommonPool,mLock),
        offsetof(CommonPool,mCondition), offsetof(CommonPool,mWaitingThreads),
        offsetof(CommonPool,mWorkQueue), offsetof(CommonPool,mIsStopping));
}

static void blockedWorkersAndQueuedTasks() {
    auto* pool = new CommonPool;
    Gate gate;
    for (int i=0; i<CommonPool::THREAD_COUNT; ++i) pool->enqueue([&]{ gate.block(); });
    gate.waitForWorkers();
    std::atomic<int> ran = 0;
    constexpr int queued = 16;
    for (int i=0; i<queued; ++i) pool->enqueue([&]{ ++ran; });
    std::promise<void> completed;
    auto future = completed.get_future();
    std::thread finalizer([&]{ delete pool; completed.set_value(); });
    if (future.wait_for(150ms) == std::future_status::ready) {
        fputs("FAIL: original destructor returned while both workers and queued tasks still used pool members\n", stderr);
        // Original detached workers must not resume through freed storage.
        std::_Exit(42);
    }
    gate.release();
    assert(future.wait_for(5s) == std::future_status::ready);
    finalizer.join();
    assert(ran == queued);
#ifdef COMMON_POOL_TESTING
    for (const auto& thread : android::uirenderer::commonPoolWorkers().threads) assert(!thread.joinable());
#endif
    puts("PASS: destructor joins both blocked workers and drains all queued tasks before member teardown");
}

static void constructorFailure() {
    failCreate = 1;
    bool failed = false;
    try { (void)new CommonPool; } catch (const std::system_error&) { failed = true; }
    assert(failed);
#ifdef COMMON_POOL_TESTING
    for (const auto& thread : android::uirenderer::commonPoolWorkers().threads) assert(!thread.joinable());
#endif
    failCreate = -1;
    auto* pool = new CommonPool;
    std::promise<void> ran;
    auto future = ran.get_future();
    pool->enqueue([&]{ ran.set_value(); });
    assert(future.wait_for(5s) == std::future_status::ready);
    delete pool;
    puts("PASS: second-thread creation failure joins the first before local handshake/member destruction; retry succeeds");
}

static void workerFinalization() {
    auto* pool = new CommonPool;
    Gate gate;
    std::promise<void> entered, exited;
    auto enteredFuture = entered.get_future();
    auto exitedFuture = exited.get_future();
    std::atomic<bool> returned = false;
    std::atomic<int> ran = 0;
    pool->enqueue([&]{ gate.block(); });
    pool->enqueue([&]{
        struct ExitWitness {
            std::promise<void>* done;
            ~ExitWitness() { done->set_value(); }
        };
        thread_local ExitWitness witness{&exited};
        entered.set_value();
        delete pool;
        returned = true;
    });
    assert(enteredFuture.wait_for(5s) == std::future_status::ready);
    // The finalizer cannot return while the other worker still holds a task.
    assert(!returned);
    gate.release();
    assert(exitedFuture.wait_for(5s) == std::future_status::ready);
    assert(returned);
#ifdef COMMON_POOL_TESTING
    for (const auto& thread : android::uirenderer::commonPoolWorkers().threads) assert(!thread.joinable());
#endif
    puts("PASS: worker-initiated finalization joins the other worker, destroys all members, and the caller exits without touching retired pool storage");
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "layout") layout();
    else if (mode == "blocked") blockedWorkersAndQueuedTasks();
    else if (mode == "constructor-failure") constructorFailure();
    else if (mode == "worker-finalization") workerFinalization();
    else if (mode == "worker-exit") {
        CommonPool::post([]{ std::exit(23); });
        std::this_thread::sleep_for(5s);
        return 43;
    }
    else return 2;
}
