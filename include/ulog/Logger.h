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
#include <algorithm>

#include "uvent/utils/intrinsincs/optimizations.h"
#include "uvent/base/Predefines.h"
#include "uvent/utils/datastructures/DataStructuresMetadata.h"
#include "uvent/system/SystemContext.h"
#include "uvent/utils/datastructures/queue/ConcurrentQueues.h"

namespace usub::ulog
{
    enum class Level : uint8_t { Trace, Debug, Info, Warn, Error };

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
        const char* trace_prefix = "\x1b[90m";
        const char* debug_prefix = "\x1b[36m";
        const char* info_prefix = "\x1b[32m";
        const char* warn_prefix = "\x1b[33m";
        const char* error_prefix = "\x1b[31m";
        const char* reset = "\x1b[0m";
    };

    struct LogEntry
    {
        uint64_t ts_ms;
        uint32_t thread_id;
        Level level;
        uint16_t size;
        char msg[4096];
    };

    struct ULogInit
    {
        const char* trace_path = nullptr;
        const char* debug_path = nullptr;
        const char* info_path = nullptr;
        const char* warn_path = nullptr;
        const char* error_path = nullptr;
        uint64_t flush_interval_ns = 2'000'000ULL;
        std::size_t queue_capacity_pow2 = 14;
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
        static void init_internal(const ULogInit& cfg) noexcept;
        static void shutdown_internal() noexcept;

        static inline Logger& instance() noexcept { return *global_.load(std::memory_order_acquire); }
        static inline Logger* try_instance() noexcept { return global_.load(std::memory_order_acquire); }

        static inline void enqueue_with_overflow(Level lvl,
                                                 const char* msg_data,
                                                 uint16_t msg_len) noexcept
        {
            Logger* lg = try_instance();
            if (!lg) return;
            if (lg->is_shutting_down()) return;

            thread_local ThreadOverflowRing<64> overflow;

            LogEntry entry{};
            entry.ts_ms = now_ms_wallclock();
            entry.thread_id = get_thread_id_fast();
            entry.level = lvl;

            uint16_t safe_len = msg_len;
            if (safe_len >= sizeof(entry.msg)) safe_len = sizeof(entry.msg) - 1;
            if (safe_len) ::memcpy(entry.msg, msg_data, safe_len);
            entry.msg[safe_len] = '\0';
            entry.size = safe_len;

            Logger& log = *lg;
            auto overflow_non_empty = [](const auto& r) noexcept { return r.head != r.tail; };

            if (log.queue_.try_enqueue(entry))
            {
                if (overflow_non_empty(overflow) && !log.is_shutting_down())
                    try_drain_overflow(overflow, log.queue_);

                if (!log.flusher_running()) log.flush_once_batch();
                return;
            }

            if (overflow.try_push(entry))
            {
                if (log.track_metrics_)
                    log.metric_overflow_pushes_.fetch_add(1, std::memory_order_relaxed);
                if (!log.flusher_running()) log.flush_once_batch();
                return;
            }

            if (log.track_metrics_)
                log.metric_backpressure_spins_.fetch_add(1, std::memory_order_relaxed);

            while (!log.queue_.try_enqueue(entry))
                cpu_relax();

            if (overflow_non_empty(overflow) && !log.is_shutting_down())
                try_drain_overflow(overflow, log.queue_);

            if (!log.flusher_running()) log.flush_once_batch();
        }

        template <typename... Args>
        static inline void pushf(Level lvl, std::string_view fmt, Args&&... args) noexcept
        {
            std::string msg;
            msg.reserve(512);
            fmt_build(msg, fmt, std::forward<Args>(args)...);

            uint16_t len = utf8_safe_size(msg.data(), msg.size(), sizeof(LogEntry::msg) - 1);
            enqueue_with_overflow(lvl, msg.data(), len);
        }

        static inline void push(Level lvl, const char* fmt, ...) noexcept
        {
            char local_buf[4096];

            va_list ap;
            va_start(ap, fmt);
            int written = ::vsnprintf(local_buf, sizeof(local_buf), fmt, ap);
            va_end(ap);

            uint16_t len;
            if (written <= 0) len = 0;
            else if ((std::size_t)written >= sizeof(local_buf)) len = (uint16_t)(sizeof(local_buf) - 1);
            else len = (uint16_t)written;

            len = utf8_safe_size(local_buf, len, sizeof(LogEntry::msg) - 1);
            enqueue_with_overflow(lvl, local_buf, len);
        }

        void flush_once_batch() noexcept;

        inline uint64_t flush_interval_ns() const noexcept { return flush_interval_ns_; }

        inline uint64_t get_overflow_pushes() const noexcept
        {
            return metric_overflow_pushes_.load(std::memory_order_relaxed);
        }

        inline uint64_t get_backpressure_spins() const noexcept
        {
            return metric_backpressure_spins_.load(std::memory_order_relaxed);
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
            int fd;
            const char* path;
            size_t bytes_written;
            bool color_enabled;
        };

        Logger(Sink sinks_init[LEVEL_COUNT],
               std::size_t queue_capacity_pow2,
               std::size_t batch_size,
               uint64_t flush_interval_ns,
               std::size_t max_file_size_bytes,
               uint32_t max_files,
               bool json_mode,
               bool track_metrics) noexcept
            : batch_size_(batch_size)
              , flush_interval_ns_(flush_interval_ns)
              , max_file_size_bytes_(max_file_size_bytes)
              , max_files_(max_files)
              , json_mode_(json_mode)
              , track_metrics_(track_metrics)
              , shutting_down_(false)
              , queue_(std::size_t{1} << queue_capacity_pow2)
        {
            for (size_t i = 0; i < LEVEL_COUNT; ++i) sinks_[i] = sinks_init[i];
            metric_overflow_pushes_.store(0, std::memory_order_relaxed);
            metric_backpressure_spins_.store(0, std::memory_order_relaxed);
        }

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        static inline uint64_t now_ms_wallclock() noexcept
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
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

        static inline size_t build_timestamp_string(uint64_t ts_ms, char* out, size_t cap) noexcept
        {
            if (cap < 24) return 0;

            time_t sec = (time_t)(ts_ms / 1000);
            uint32_t msec = (uint32_t)(ts_ms % 1000);

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

            int year = tm_ptr ? (tm_ptr->tm_year + 1900) : 0;
            int mon = tm_ptr ? (tm_ptr->tm_mon + 1) : 0;
            int day = tm_ptr ? tm_ptr->tm_mday : 0;
            int hr = tm_ptr ? tm_ptr->tm_hour : 0;
            int min = tm_ptr ? tm_ptr->tm_min : 0;
            int sec2 = tm_ptr ? tm_ptr->tm_sec : 0;

            out[0] = char('0' + (year / 1000) % 10);
            out[1] = char('0' + (year / 100) % 10);
            out[2] = char('0' + (year / 10) % 10);
            out[3] = char('0' + (year % 10));
            out[4] = '-';
            u32_to_dec2((uint32_t)mon, &out[5]);
            out[7] = '-';
            u32_to_dec2((uint32_t)day, &out[8]);
            out[10] = ' ';
            u32_to_dec2((uint32_t)hr, &out[11]);
            out[13] = ':';
            u32_to_dec2((uint32_t)min, &out[14]);
            out[16] = ':';
            u32_to_dec2((uint32_t)sec2, &out[17]);
            out[19] = '.';
            u32_to_dec3(msec, &out[20]);

            return 23;
        }

        static inline std::size_t format_prefix_plain(const LogEntry& e, char* out, std::size_t cap) noexcept
        {
            if (cap == 0) return 0;

            char tsbuf[32];
            size_t tslen = build_timestamp_string(e.ts_ms, tsbuf, sizeof(tsbuf));

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
                    tmp[n++] = char('0' + (v - q * 10));
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
                std::size_t cp = std::min(tslen, cap - off);
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
                if (lvlc && lvlc[0] && off < cap) out[off++] = lvlc[0];
            }
            if (off < cap) out[off++] = ']';
            if (off < cap) out[off++] = ' ';

            if (off > cap) off = cap;
            return off;
        }

        static inline void color_codes_for(Level lvl, const char*& start, const char*& end, bool enabled) noexcept
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
            case Level::Trace: start = c.trace_prefix;
                end = c.reset;
                break;
            case Level::Debug: start = c.debug_prefix;
                end = c.reset;
                break;
            case Level::Info: start = c.info_prefix;
                end = c.reset;
                break;
            case Level::Warn: start = c.warn_prefix;
                end = c.reset;
                break;
            default:
            case Level::Error: start = c.error_prefix;
                end = c.reset;
                break;
            }
        }

        template <typename T>
        static inline void append_one(std::string& out, const T& v) noexcept
        {
            if constexpr (std::is_same_v<T, std::string>) out.append(v);
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

        static inline void fmt_build(std::string& out, std::string_view fmt) noexcept { out.append(fmt); }

        template <typename Arg, typename... Rest>
        static inline void fmt_build(std::string& out, std::string_view fmt, Arg&& a, Rest&&... rest) noexcept
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

        static inline uint16_t utf8_safe_size(const char* data, size_t len, size_t max_bytes)
        {
            size_t i = std::min(len, max_bytes);
            if (i == 0) return 0;
            i--;
            while (i > 0 && (static_cast<unsigned char>(data[i]) & 0xC0) == 0x80) --i;
            return static_cast<uint16_t>(i + 1);
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
                if (next == head) return false;
                buf[tail] = e;
                tail = next;
                return true;
            }

            inline bool try_pop(LogEntry& out) noexcept
            {
                if (head == tail) return false;
                out = buf[head];
                head = uint16_t((head + 1) % N);
                return true;
            }

            inline void rollback_last_pop() noexcept { head = static_cast<uint16_t>((head + N - 1) % N); }
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
                    break;
                }
            }
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
        std::atomic<uint64_t> metric_overflow_pushes_{0};
        std::atomic<uint64_t> metric_backpressure_spins_{0};
        queue::concurrent::MPMCQueue<LogEntry> queue_;
    };
} // namespace usub::ulog

#endif // LOGGER_H