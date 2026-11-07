// The worker threads (vk_jobs.h). Upstream ran a third of the hardware threads (2 to 4): the game itself runs six virtual
// cores, and the replay thread is one of the busiest.
// (tameem) On a 16-thread CPU that left an area's new pipelines queued behind 4 workers while 8 threads idled: half the
// threads less two, 2 to 8 (6 on a 7800X3D). RX_VK_WORKERS=<n>: that many. Background jobs (the optimised builds that
// replace quick ones) wait behind all other work and never take more than all but two workers, so a pipeline a draw
// needs always finds a worker free.
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "vk_jobs.h"
#include "vk_internal.h"

namespace mkwvk {
namespace {

std::mutex mu;
std::condition_variable work_cv, done_cv;
std::deque<std::function<void()>> queue, background;
std::atomic<int> pending{0}, bg_pending{0};
int bg_running = 0, bg_cap = 1;   // (under mu)
bool started = false;

unsigned workers_wanted() {
    if (const char* e = getenv("RX_VK_WORKERS")) {
        const int n = atoi(e);
        if (n > 0) return (unsigned)std::min(n, 32);
    }
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(hw / 2 > 4 ? hw / 2 - 2 : 2u, 2u, 8u);
}

void worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"mkw_vk worker");
    for (;;) {
        std::function<void()> fn;
        bool bg = false;
        {
            std::unique_lock lk(mu);
            work_cv.wait(lk, [] { return !queue.empty() || (!background.empty() && bg_running < bg_cap); });
            if (!queue.empty()) {
                fn = std::move(queue.front());
                queue.pop_front();
            } else {
                fn = std::move(background.front());
                background.pop_front();
                bg = true;
                bg_running++;
            }
        }
        fn();
        if (bg) {
            {
                std::lock_guard lk(mu);
                bg_running--;
            }
            bg_pending--;
            work_cv.notify_one();   // (a background job may go now)
        } else {
            pending--;
        }
        jobs_notify();
    }
}

void start() {
    const unsigned n = workers_wanted();
    bg_cap = (int)std::max(1u, n > 2 ? n - 2 : 1u);
    for (unsigned i = 0; i < n; i++) std::thread(worker).detach();
    started = true;
    logf("vk: %u background workers (shader translation, pipelines; up to %d on optimised rebuilds)", n, bg_cap);
}

} // namespace

void jobs_push(std::function<void()> fn, bool urgent) {
    std::lock_guard lk(mu);
    if (!started) start();
    pending++;
    if (urgent) queue.push_front(std::move(fn));
    else queue.push_back(std::move(fn));
    work_cv.notify_one();
}
void jobs_push_background(std::function<void()> fn) {
    std::lock_guard lk(mu);
    if (!started) start();
    bg_pending++;
    background.push_back(std::move(fn));
    work_cv.notify_one();
}
void jobs_notify() {
    std::lock_guard lk(mu);
    done_cv.notify_all();
}
void jobs_wait(const std::atomic<bool>& flag) {
    std::unique_lock lk(mu);
    done_cv.wait(lk, [&] { return flag.load(std::memory_order_acquire); });
}
int jobs_pending() {
    return pending.load();
}
int jobs_background_pending() {
    return bg_pending.load();
}

} // namespace mkwvk
