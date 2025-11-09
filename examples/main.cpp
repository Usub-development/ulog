#include "uvent/Uvent.h"
#include "uvent/system/Settings.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"
#include <chrono>

using namespace usub;
using namespace usub::uvent;

uvent::task::Awaitable<void> fake_worker(int id)
{
    for (int i = 0; i < 500; ++i)
    {
        ulog::trace("worker={} tick={}", id, i);
        ulog::debug("worker={} recv req_id={}", id, 1000 + i);
        ulog::info("worker={} handled request size={}B", id, 512UL);

        if (i == 2)
            ulog::warn("worker={} slow op >= {} ms", id, 17.4);

        if (i == 4)
            ulog::error("worker={} backend fail code={}", id, -104);

        co_await system::this_coroutine::sleep_for(
            std::chrono::milliseconds(10)
        );
    }

    ulog::info("worker={} done", id);
    co_return;
}

int main()
{
    usub::ulog::ULogInit cfg{
        .trace_path = nullptr,
        .debug_path = nullptr,
        .info_path = nullptr,
        .warn_path = nullptr,
        .error_path = nullptr,
        .flush_interval_ns = 2'000'000ULL, // 2ms
        .queue_capacity = 14, // 2^14 = 16384
        .batch_size = 512,
        .enable_color_stdout = true,
        .max_file_size_bytes = 10 * 1024 * 1024, // rotate at 10MB
        .max_files = 3, // keep file.log.1..file.log.3
        .json_mode = false, // human-readable
        .track_metrics = true // enable contention stats
    };

    usub::ulog::init(cfg);

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
