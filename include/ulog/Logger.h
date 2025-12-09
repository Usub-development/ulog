#ifndef LOGGER_H
#define LOGGER_H

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <chrono>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <mutex>

#include "uvent/utils/intrinsincs/optimizations.h"
#include "uvent/base/Predefines.h"
#include "uvent/utils/datastructures/DataStructuresMetadata.h"
#include "uvent/system/SystemContext.h"
#include "uvent/utils/datastructures/queue/ConcurrentQueues.h"
#include "uvent/utils/datastructures/queue/FastQueue.h"

namespace usub::ulog
{
    enum class Level : uint8_t
    {
        Trace,
        Debug,
        Info,
        Warn,
        Error,
        Critical,
        Fatal
    };

    static inline constexpr size_t LEVEL_COUNT = 7;

    inline constexpr const char* level_name(Level lvl) noexcept
    {
        switch (lvl)
        {
        case Level::Trace: return "T";
        case Level::Debug: return "D";
        case Level::Info: return "I";
        case Level::Warn: return "W";
        case Level::Error: return "E";
        case Level::Critical: return "C";
        case Level::Fatal: return "F";
        default: return "?";
        }
    }

    struct AnsiColors
    {
        const char* trace_prefix = "\x1b[90m";
        const char* debug_prefix = "\x1b[36m";
        const char* info_prefix = "\x1b[32m";
        const char* warn_prefix = "\x1b[33m";
        const char* error_prefix = "\x1b[31m";
        const char* critical_prefix = "\x1b[91m";
        const char* fatal_prefix = "\x1b[95m";
        const char* reset = "\x1b[0m";
    };

    struct LogEntry
    {
        uint64_t ts_ms{};
        uint32_t thread_id{};
        Level level{};
        std::string msg;
    };

    struct ULogInit
    {
        const char* trace_path = nullptr;
        const char* debug_path = nullptr;
        const char* info_path = nullptr;
        const char* warn_path = nullptr;
        const char* error_path = nullptr;
        const char* critical_path = nullptr;
        const char* fatal_path = nullptr;

        uint64_t flush_interval_ns = 2'000'000ULL;
        std::size_t queue_capacity = 16384;
        std::size_t batch_size = 512;
        bool enable_color_stdout = true;
        std::size_t max_file_size_bytes = 0;
        uint32_t max_files = 3;
        bool json_mode = false;
        bool track_metrics = false;
    };

    class Logger
    {
    public:
        ~Logger()
        {
            std::cout << "dtor Logger" << std::endl;
        }

        static void init_internal(const ULogInit& cfg) noexcept;
        static void shutdown_internal() noexcept;

        static inline Logger& instance() noexcept
        {
            return *global_.load(std::memory_order_acquire);
        }

        static inline Logger* try_instance() noexcept
        {
            return global_.load(std::memory_order_acquire);
        }

        static constexpr std::size_t kMaxLogLineBytes = 64 * 1024;

        static inline void enqueue_with_overflow(Level lvl, const char* msg_data, uint32_t msg_len) noexcept
        {
            Logger* lg = try_instance();
            if (!lg) return;
            if (lg->is_shutting_down()) return;

            LogEntry entry{};
            entry.ts_ms = now_ms_wallclock();
            entry.thread_id = get_thread_id_fast();
            entry.level = lvl;

            const uint32_t safe_len = utf8_safe_size(msg_data, msg_len, kMaxLogLineBytes);
            entry.msg.assign(msg_data, msg_data + safe_len);

            if (lg->queue_.try_enqueue(entry))
            {
                if (!lg->flusher_running())
                    lg->flush_once_batch();
                return;
            }

            if (lg->track_metrics_)
                lg->metric_overflows_.fetch_add(1, std::memory_order_relaxed);

            {
                std::lock_guard<std::mutex> lk(lg->fallback_mutex_);
                lg->fallback_queue_.enqueue(std::move(entry));
            }

            if (!lg->flusher_running())
                lg->flush_once_batch();
        }

        template <typename... Args>
        static inline void pushf(Level lvl, std::string_view fmt, Args&&... args) noexcept
        {
            std::string msg;
            msg.reserve(512);
            fmt_build(msg, fmt, std::forward<Args>(args)...);
            const uint32_t len = utf8_safe_size(msg.data(), msg.size(), kMaxLogLineBytes);
            enqueue_with_overflow(lvl, msg.data(), len);
        }

        static inline void push(Level lvl, const char* fmt, ...) noexcept
        {
            std::vector<char> buf(256);
            va_list ap;
            va_start(ap, fmt);
            for (;;)
            {
                va_list ap2;
                va_copy(ap2, ap);
                int needed = ::vsnprintf(buf.data(), buf.size(), fmt, ap2);
                va_end(ap2);
                if (needed < 0)
                {
                    va_end(ap);
                    return;
                }
                if (static_cast<size_t>(needed) < buf.size()) break;
                buf.resize(static_cast<size_t>(needed) + 1);
            }
            va_end(ap);
            const uint32_t len =
                utf8_safe_size(buf.data(), std::strlen(buf.data()), kMaxLogLineBytes);
            enqueue_with_overflow(lvl, buf.data(), len);
        }

        void flush_once_batch() noexcept;

        inline uint64_t flush_interval_ns() const noexcept
        {
            return flush_interval_ns_;
        }

        inline uint64_t get_overflow_events() const noexcept
        {
            return metric_overflows_.load(std::memory_order_relaxed);
        }

        inline bool is_shutting_down() const noexcept
        {
            return shutting_down_.load(std::memory_order_acquire);
        }

        inline void mark_flusher_started() noexcept
        {
            flusher_started_.store(true, std::memory_order_release);
        }

        inline bool flusher_running() const noexcept
        {
            return flusher_started_.load(std::memory_order_acquire);
        }

    private:
        struct Sink
        {
            int fd{};
            const char* path{};
            size_t bytes_written{};
            bool color_enabled{};
        };

        Logger(Sink sinks_init[LEVEL_COUNT],
               std::size_t queue_capacity,
               std::size_t batch_size,
               uint64_t flush_interval_ns,
               std::size_t max_file_size_bytes,
               uint32_t max_files,
               bool json_mode,
               bool track_metrics) noexcept;

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        static inline uint64_t now_ms_wallclock() noexcept
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(
                    system_clock::now().time_since_epoch())
                .count();
        }

        static inline uint32_t get_thread_id_fast() noexcept
        {
            static thread_local uint32_t tls_thread_id_cache = 0;
            if (tls_thread_id_cache != 0) return tls_thread_id_cache;
            uint32_t rt = uvent::system::this_thread::detail::t_id;
            bool valid = (rt != 0u) && (rt != 0xFFFFFFFFu);
            if (!valid)
            {
                rt = (uint32_t)((reinterpret_cast<uintptr_t>(&tls_thread_id_cache)) & 0xFFFFu);
                if (rt == 0u) rt = 1u;
            }
            tls_thread_id_cache = rt;
            return rt;
        }

        static std::string build_timestamp_string(uint64_t ts_ms);
        static std::string format_prefix_plain(const LogEntry& e);
        static void color_codes_for(Level lvl, const char*& start, const char*& end, bool enabled) noexcept;

        template <typename T>
        static inline void append_one(std::string& out, const T& v) noexcept
        {
            if constexpr (std::is_same_v<T, std::string>)
            {
                out.append(v);
            }
            else if constexpr (std::is_convertible_v<T, std::string_view>)
            {
                std::string_view sv(v);
                out.append(sv.data(), sv.size());
            }
            else if constexpr (std::is_integral_v<T>)
            {
                char b[64];
                int n = ::snprintf(b, sizeof(b), "%lld", (long long)v);
                if (n > 0) out.append(b, (size_t)n);
            }
            else if constexpr (std::is_floating_point_v<T>)
            {
                char b[64];
                int n = ::snprintf(b, sizeof(b), "%g", (double)v);
                if (n > 0) out.append(b, (size_t)n);
            }
            else
            {
                char b[64];
                int n = ::snprintf(b, sizeof(b), "%p", (const void*)&v);
                if (n > 0) out.append(b, (size_t)n);
            }
        }

        static inline void fmt_build(std::string& out, std::string_view fmt) noexcept
        {
            out.append(fmt);
        }

        template <typename Arg, typename... Rest>
        static inline void fmt_build(std::string& out,
                                     std::string_view fmt,
                                     Arg&& a,
                                     Rest&&... rest) noexcept
        {
            size_t p = fmt.find("{}");
            if (p == std::string_view::npos)
            {
                out.append(fmt);
                return;
            }
            out.append(fmt.substr(0, p));
            append_one(out, a);
            fmt_build(out, fmt.substr(p + 2), std::forward<Rest>(rest)...);
        }

        static inline uint32_t utf8_safe_size(const char* data, size_t len, size_t max_bytes)
        {
            size_t i = std::min(len, max_bytes);
            if (i == 0) return 0;
            i--;
            while (i > 0 && (static_cast<unsigned char>(data[i]) & 0xC0) == 0x80) --i;
            return static_cast<uint32_t>(i + 1);
        }

        void maybe_rotate_sink(size_t idx, size_t incoming_bytes) noexcept;
        static void rotate_files(const char* path, uint32_t max_files) noexcept;

    private:
        static inline std::atomic<Logger*> global_{nullptr};

        Sink sinks_[LEVEL_COUNT];

        std::size_t batch_size_;
        uint64_t flush_interval_ns_;
        std::size_t max_file_size_bytes_;
        uint32_t max_files_;
        bool json_mode_;
        bool track_metrics_;

        std::atomic<bool> shutting_down_;
        std::atomic<bool> flusher_started_{false};
        std::atomic<uint64_t> metric_overflows_{0};

        queue::concurrent::MPMCQueue<LogEntry> queue_;
        queue::single_thread::Queue<LogEntry> fallback_queue_;
        std::mutex fallback_mutex_;
    };
} // namespace usub::ulog

#endif // LOGGER_H