//
// Created by root on 10/28/25.
//

#ifndef CONFIG_H
#define CONFIG_H

#include <cstdint>

namespace usub::uvent::log
{
    struct LoggerConfig
    {
        const char* filepath;
        uint64_t    flush_interval_ns;
        size_t      queue_capacity_pow2;
        size_t      batch_size;
    };
}

#endif //CONFIG_H
