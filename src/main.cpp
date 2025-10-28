#include "uvent/Uvent.h"
#include "uvent/system/Settings.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"
#include <chrono>

using namespace usub;
using namespace usub::uvent;

uvent::task::Awaitable<void> fake_worker(int id)
{
    for (int i = 0; i < 5; ++i)
    {
        ulog::trace("worker={} tick={}", id, i);
        ulog::debug("worker={} recv req_id={}", id, 1000 + i);
        ulog::info ("worker={} handled request size={}B", id, 512UL);

        if (i == 2)
            ulog::warn("worker={} slow op >= {} ms", id, 17.4);

        if (i == 4)
            ulog::error("worker={} backend fail code={}", id, -104);

        co_await system::this_coroutine::sleep_for(
            std::chrono::milliseconds(200)
        );
    }

    ulog::info("worker={} done", id);
    co_return;
}

int main()
{
    settings::timeout_duration_ms = 5000;

    usub::ulog::ULogInit cfg{
        .trace_path          = "./trace.log",
        .debug_path          = "./debug.log",
        .info_path           = "./info.log",
        .warn_path           = "./warn.log",
        .error_path          = "./error.log",
        .flush_interval_ns   = 2'000'000ULL,
        .queue_capacity_pow2 = 14,
        .batch_size          = 512,
        .enable_color_stdout = true
    };

    usub::ulog::init(cfg);

    ulog::info("bootstrap start, workers={}, timeout_ms={}",
               4, settings::timeout_duration_ms);

    for (int wid = 0; wid < 4; ++wid)
        uvent::system::co_spawn(fake_worker(wid));

    ulog::debug("starting event loop...");

    {
        usub::Uvent uvent(4);
        uvent.run();
    }

    ulog::warn("event loop finished, shutting down logger");
    usub::ulog::shutdown();

    return 0;
}
