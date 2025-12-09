#include "uvent/Uvent.h"
#include "uvent/system/Settings.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"
#include <chrono>

using namespace usub;
using namespace usub::uvent;

uvent::task::Awaitable<void> fake_worker(int id)
{
    for (int i = 0; i < 2000; ++i)
    {
        ulog::trace("burst worker={} tick={}", id, i);
        ulog::debug("burst worker={} recv req_id={}", id, 1000 + i);
        ulog::info("burst worker={} handled request size={}B", id, 512UL);
        if (i % 10 == 0)
            ulog::error("burst worker={} backend fail code={}", id, -104);
    }

    for (int i = 0; i < 500; ++i)
    {
        ulog::trace("worker={} tick={}", id, i);
        ulog::debug("worker={} recv req_id={}", id, 10'000 + i);
        ulog::info("worker={} handled request size={}B", id, 256UL);

        if (i == 2)
            ulog::warn("worker={} slow op >= {} ms", id, 17.4);

        if (i == 4)
            ulog::error("worker={} backend fail code={}", id, -204);

        co_await system::this_coroutine::sleep_for(
            std::chrono::milliseconds(10)
        );
    }

    ulog::info("worker={} done", id);
    co_return;
}

uvent::task::Awaitable<void> fallback_logger()
{
    using namespace std::chrono_literals;
    ulog::debug("fallback logger started");
    co_await system::this_coroutine::sleep_for(60s);
    if (auto* lg = usub::ulog::Logger::try_instance())
    {
        auto overflows = lg->get_overflow_events();
        ulog::info("logger overflows (mpmc full -> mutex fallback) = {}", overflows);
        ulog::info("test 1");
        ulog::info("logger overflows after (mpmc full -> mutex fallback) = {}", overflows);
    }
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
        .critical_path = nullptr,
        .fatal_path = nullptr,

        .flush_interval_ns = 50'000'000ULL,
        .queue_capacity = 64,
        .batch_size = 256,
        .enable_color_stdout = true,
        .max_file_size_bytes = 0,
        .max_files = 3,
        .json_mode = false,
        .track_metrics = true
    };

    usub::ulog::init(cfg);

    usub::uvent::system::co_spawn(fallback_logger());
    {
        for (int wid = 0; wid < 16; ++wid)
            uvent::system::co_spawn(fake_worker(wid));

        ulog::debug("starting event loop...");

        Uvent uvent(4);
        uvent.run();
    }

    ulog::warn("event loop finished, shutting down logger");

    if (auto* lg = usub::ulog::Logger::try_instance())
    {
        auto overflows = lg->get_overflow_events();
        ulog::info("logger overflows (mpmc full -> mutex fallback) = {}", overflows);
    }

    usub::ulog::shutdown();
    return 0;
}
