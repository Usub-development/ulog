//
// Created by kirill on 12/23/25.
//

#include "uvent/Uvent.h"
#include "uvent/system/Settings.h"
#include "uvent/system/SystemContext.h"

#include "ulog/ulog.h"
#include <chrono>

struct male {
    bool male_;
};

struct female {
    bool female_;
};

struct Address {
    std::string street, building;
};

struct User {
    std::string name;
    std::optional<std::string> patronymic;
    Address address;
    std::variant<male, female> sex; // primary sexual characteristics
    std::vector<std::string> roles;
};

usub::uvent::task::Awaitable<void> test_reflection()
{
    User u1{
        .name = "Jonh",
        .patronymic = std::optional<std::string>{"Jognhn"},
        .address = Address{
            .street = "Lenina",
            .building = "10A",
        },
        .sex = male{ .male_ = true },
        .roles = {"admin", "developer", "operator"},
    };

    usub::ulog::trace("user u1: {}", u1);

    User u2{
        .name = "Anna",
        .patronymic = std::nullopt,
        .address = Address{
            .street = "Nevsky Prospekt",
            .building = "24",
        },
        .sex = female{ .female_ = true },
        .roles = {"user", "viewer"},
    };

    usub::ulog::trace("user u1: {1}, user u2: {0}", u2, u1);
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

    usub::uvent::system::co_spawn(test_reflection());

    usub::ulog::debug("starting event loop...");

    {
        usub::Uvent uvent(4);
        uvent.run();
    }

    usub::ulog::warn("event loop finished, shutting down logger");
    usub::ulog::shutdown();

    return 0;
}