#ifndef ULOG_H
#define ULOG_H

#include <string_view>
#include "ulog/Logger.h"
#include "ulog/LoggerFlushTask.h"
#include "uvent/system/SystemContext.h"

namespace usub::ulog
{
    inline void init(const ULogInit& cfg = ULogInit{})
    {
        Logger::init_internal(cfg);
        uvent::system::co_spawn(logger_flush_task());
    }

    inline void shutdown()
    {
        Logger::shutdown_internal();
    }

    template <typename... Args>
    inline void trace(std::string_view fmt, Args&&... args) noexcept
    {
        Logger::pushf(Level::Trace, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    inline void debug(std::string_view fmt, Args&&... args) noexcept
    {
        Logger::pushf(Level::Debug, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    inline void info(std::string_view fmt, Args&&... args) noexcept
    {
        Logger::pushf(Level::Info, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    inline void warn(std::string_view fmt, Args&&... args) noexcept
    {
        Logger::pushf(Level::Warn, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    inline void error(std::string_view fmt, Args&&... args) noexcept
    {
        Logger::pushf(Level::Error, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    inline void critical(std::string_view fmt, Args&&... args) noexcept
    {
        Logger::pushf(Level::Critical, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    inline void fatal(std::string_view fmt, Args&&... args) noexcept
    {
        Logger::pushf(Level::Fatal, fmt, std::forward<Args>(args)...);
    }
}

#endif // ULOG_H
