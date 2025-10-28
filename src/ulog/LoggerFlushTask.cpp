//
// Created by root on 10/28/25.
//

#include "ulog/LoggerFlushTask.h"
#include "uvent/system/SystemContext.h"

namespace usub::ulog
{
    uvent::task::Awaitable<void> logger_flush_task()
    {
        auto& lg = Logger::instance();

        for (;;)
        {
            lg.flush_once_batch();

            co_await uvent::system::this_coroutine::sleep_for(
                std::chrono::nanoseconds(lg.flush_interval_ns())
            );
        }

        co_return;
    }
}
