#pragma once

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
#include <algorithm>

#include "uvent/utils/intrinsincs/optimizations.h"
#include "uvent/base/Predefines.h"
#include "uvent/utils/datastructures/DataStructuresMetadata.h"
#include "uvent/system/SystemContext.h"
#include "uvent/utils/datastructures/queue/ConcurrentQueues.h"

namespace usub::ulog
{
    enum class Level : uint8_t
    {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4
    };

    static inline constexpr size_t LEVEL_COUNT = 5;

    inline constexpr const char* level_name(Level lvl) noexcept
    {
        switch (lvl)
        {
        case Level::Trace: return "T";
        case Level::Debug: return "D";
        case Level::Info: return "I";
        case Level::Warn: return "W";
        default:
        case Level::Error: return "E";
        }
    }

    struct AnsiColors
    {
        const char* trace_prefix = "\x1b[90m"; // gray
        const char* debug_prefix = "\x1b[36m"; // cyan
        const char* info_prefix = "\x1b[32m"; // green
        const char* warn_prefix = "\x1b[33m"; // yellow
        const char* error_prefix = "\x1b[31m"; // red
        const char* reset = "\x1b[0m";
    };

    struct LogEntry
    {
        uint64_t ts_ms;
        uint32_t thread_id;
        Level level;
        uint16_t size;
        char msg[256];
    };

    struct ULogInit
    {
        const char* trace_path;
        const char* debug_path;
        const char* info_path;
        const char* warn_path;
        const char* error_path;

        uint64_t flush_interval_ns;
        std::size_t queue_capacity_pow2;
        std::size_t batch_size;

        bool enable_color_stdout;
    };

    class Logger
    {
    public:
        static void init_internal(const ULogInit& cfg) noexcept;
        static void shutdown_internal() noexcept;

        static inline Logger& instance() noexcept { return *global_; }

        static inline void enqueue_with_overflow(Level lvl,
                                                 const char* msg_data,
                                                 uint16_t msg_len) noexcept
        {
            thread_local ThreadOverflowRing<64> overflow;

            LogEntry entry;
            entry.ts_ms = now_ms_wallclock();
            entry.thread_id = get_thread_id_fast();
            entry.level = lvl;
            entry.size = msg_len;
            if (msg_len)
                ::memcpy(entry.msg, msg_data, msg_len);
            if (msg_len < sizeof(entry.msg))
                entry.msg[msg_len] = '\0';

            Logger& log = instance();

            if (log.queue_.try_enqueue(entry))
            {
                try_drain_overflow(overflow, log.queue_);
                return;
            }

            if (overflow.try_push(entry))
            {
                return;
            }

            while (!log.queue_.try_enqueue(entry))
            {
                cpu_relax();
            }

            try_drain_overflow(overflow, log.queue_);
        }

        template <typename... Args>
        static inline void pushf(Level lvl, std::string_view fmt, Args&&... args) noexcept
        {
            std::string msg;
            msg.reserve(256);
            fmt_build(msg, fmt, std::forward<Args>(args)...);

            uint16_t len = (uint16_t)std::min(msg.size(), sizeof(LogEntry::msg) - 1);
            enqueue_with_overflow(lvl, msg.data(), len);
        }

        static inline void push(Level lvl, const char* fmt, ...) noexcept
        {
            char local_buf[256];

            va_list ap;
            va_start(ap, fmt);
            int written = ::vsnprintf(local_buf, sizeof(local_buf), fmt, ap);
            va_end(ap);

            uint16_t len;
            if (written <= 0)
                len = 0;
            else if ((std::size_t)written >= sizeof(local_buf))
                len = (uint16_t)(sizeof(local_buf) - 1);
            else
                len = (uint16_t)written;

            enqueue_with_overflow(lvl, local_buf, len);
        }

        void flush_once_batch() noexcept;

        inline uint64_t flush_interval_ns() const noexcept { return flush_interval_ns_; }

    private:
        Logger(int fds[LEVEL_COUNT],
               bool color_enabled[LEVEL_COUNT],
               std::size_t queue_capacity_pow2,
               std::size_t batch_size,
               uint64_t flush_interval_ns) noexcept;

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        static inline uint64_t now_ms_wallclock() noexcept
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()
            ).count();
        }

        static inline uint32_t get_thread_id_fast() noexcept
        {
            static thread_local uint32_t tls_thread_id_cache = 0;

            if (tls_thread_id_cache != 0)
                return tls_thread_id_cache;

            uint32_t rt = uvent::system::this_thread::detail::t_id;

            bool valid =
                (rt != 0u) &&
                (rt != 0xFFFFFFFFu);

            if (!valid)
            {
                rt = (uint32_t)((reinterpret_cast<uintptr_t>(&tls_thread_id_cache)) & 0xFFFFu);
                if (rt == 0u) rt = 1u;
            }

            tls_thread_id_cache = rt;
            return rt;
        }


        static inline std::size_t format_prefix_plain(const LogEntry& e,
                                                      char* out,
                                                      std::size_t cap) noexcept
        {
            if (cap == 0) return 0;

            uint64_t ms_total = e.ts_ms;
            time_t sec = (time_t)(ms_total / 1000);
            uint32_t msec = (uint32_t)(ms_total % 1000);

            struct tm tmbuf;
            struct tm* tm_ptr = ::localtime_r(&sec, &tmbuf);

            auto u32_to_dec3 = [](uint32_t v, char* buf)
            {
                buf[0] = char('0' + (v / 100) % 10);
                buf[1] = char('0' + (v / 10) % 10);
                buf[2] = char('0' + (v % 10));
            };
            auto u32_to_dec2 = [](uint32_t v, char* buf)
            {
                buf[0] = char('0' + (v / 10) % 10);
                buf[1] = char('0' + (v % 10));
            };

            char tsbuf[32];
            int year = tm_ptr ? (tm_ptr->tm_year + 1900) : 0;
            int mon = tm_ptr ? (tm_ptr->tm_mon + 1) : 0;
            int day = tm_ptr ? tm_ptr->tm_mday : 0;
            int hr = tm_ptr ? tm_ptr->tm_hour : 0;
            int min = tm_ptr ? tm_ptr->tm_min : 0;
            int sec2 = tm_ptr ? tm_ptr->tm_sec : 0;

            tsbuf[0] = char('0' + (year / 1000) % 10);
            tsbuf[1] = char('0' + (year / 100) % 10);
            tsbuf[2] = char('0' + (year / 10) % 10);
            tsbuf[3] = char('0' + (year % 10));
            tsbuf[4] = '-';
            u32_to_dec2((uint32_t)mon, &tsbuf[5]);
            tsbuf[7] = '-';
            u32_to_dec2((uint32_t)day, &tsbuf[8]);
            tsbuf[10] = ' ';
            u32_to_dec2((uint32_t)hr, &tsbuf[11]);
            tsbuf[13] = ':';
            u32_to_dec2((uint32_t)min, &tsbuf[14]);
            tsbuf[16] = ':';
            u32_to_dec2((uint32_t)sec2, &tsbuf[17]);
            tsbuf[19] = '.';
            u32_to_dec3(msec, &tsbuf[20]);
            constexpr size_t tslen = 23;

            auto u64_to_buf = [](uint64_t v, char* buf, std::size_t max) noexcept -> std::size_t
            {
                char tmp[32];
                std::size_t n = 0;
                if (v == 0)
                {
                    if (max > 0) buf[0] = '0';
                    return 1;
                }
                while (v != 0 && n < sizeof(tmp))
                {
                    uint64_t q = v / 10;
                    uint64_t d = v - q * 10;
                    tmp[n++] = char('0' + d);
                    v = q;
                }
                std::size_t w = 0;
                while (w < n && w < max)
                {
                    buf[w] = tmp[n - 1 - w];
                    ++w;
                }
                return w;
            };

            std::size_t off = 0;

            if (off < cap) out[off++] = '[';
            {
                std::size_t cp = tslen;
                if (cp > cap - off) cp = cap - off;
                if (cp)
                {
                    ::memcpy(out + off, tsbuf, cp);
                    off += cp;
                }
            }
            if (off < cap) out[off++] = ']';

            if (off < cap) out[off++] = '[';
            off += u64_to_buf(e.thread_id, out + off, (off < cap ? cap - off : 0));
            if (off < cap) out[off++] = ']';

            if (off < cap) out[off++] = '[';
            {
                const char* lvlc = level_name(e.level);
                if (lvlc && lvlc[0] && off < cap)
                    out[off++] = lvlc[0];
            }
            if (off < cap) out[off++] = ']';
            if (off < cap) out[off++] = ' ';

            if (off > cap) off = cap;
            return off;
        }

        static inline void color_codes_for(Level lvl,
                                           const char*& start,
                                           const char*& end,
                                           bool enabled) noexcept
        {
            static const AnsiColors c{};
            if (!enabled)
            {
                start = "";
                end = "";
                return;
            }

            switch (lvl)
            {
            case Level::Trace:
                start = c.trace_prefix;
                end = c.reset;
                break;
            case Level::Debug:
                start = c.debug_prefix;
                end = c.reset;
                break;
            case Level::Info:
                start = c.info_prefix;
                end = c.reset;
                break;
            case Level::Warn:
                start = c.warn_prefix;
                end = c.reset;
                break;
            default:
            case Level::Error:
                start = c.error_prefix;
                end = c.reset;
                break;
            }
        }

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
                char buf[64];
                int n = ::snprintf(buf, sizeof(buf), "%lld", (long long)v);
                if (n > 0) out.append(buf, (size_t)n);
            }
            else if constexpr (std::is_floating_point_v<T>)
            {
                char buf[64];
                int n = ::snprintf(buf, sizeof(buf), "%g", (double)v);
                if (n > 0) out.append(buf, (size_t)n);
            }
            else
            {
                char buf[64];
                int n = ::snprintf(buf, sizeof(buf), "%p", (const void*)&v);
                if (n > 0) out.append(buf, (size_t)n);
            }
        }

        static inline void fmt_build(std::string& out,
                                     std::string_view fmt) noexcept
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

        template <std::size_t N>
        struct ThreadOverflowRing
        {
            LogEntry buf[N];
            uint16_t head = 0;
            uint16_t tail = 0;

            inline bool try_push(const LogEntry& e) noexcept
            {
                uint16_t next = uint16_t((tail + 1) % N);
                if (next == head)
                {
                    return false;
                }
                buf[tail] = e;
                tail = next;
                return true;
            }

            inline bool try_pop(LogEntry& out) noexcept
            {
                if (head == tail)
                {
                    return false;
                }
                out = buf[head];
                head = uint16_t((head + 1) % N);
                return true;
            }

            inline void rollback_last_pop() noexcept
            {
                head = uint16_t((head + 65535u) % N);
            }

            inline void overwrite_last_rollback(const LogEntry& e) noexcept
            {
                buf[head] = e;
            }
        };

        template <std::size_t N>
        static inline void try_drain_overflow(ThreadOverflowRing<N>& overflow,
                                              queue::concurrent::MPMCQueue<LogEntry>& q) noexcept
        {
            LogEntry tmp;
            while (overflow.try_pop(tmp))
            {
                if (!q.try_enqueue(tmp))
                {
                    overflow.rollback_last_pop();
                    overflow.overwrite_last_rollback(tmp);
                    break;
                }
            }
        }

    private:
        static inline Logger* global_ = nullptr;

        int fds_[LEVEL_COUNT];
        bool color_enabled_[LEVEL_COUNT];

        queue::concurrent::MPMCQueue<LogEntry> queue_;
        std::size_t batch_size_;
        uint64_t flush_interval_ns_;
        std::atomic<bool> shutting_down_;
    };
} // namespace usub::ulog
