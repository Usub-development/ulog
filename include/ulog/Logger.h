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
#include <iostream>

#include <optional>
#include <variant>
#include <array>
#include <tuple>
#include <iterator>
#include <type_traits>
#include <utility>

#include "uvent/utils/intrinsincs/optimizations.h"
#include "uvent/base/Predefines.h"
#include "uvent/utils/datastructures/DataStructuresMetadata.h"
#include "uvent/system/SystemContext.h"
#include "uvent/utils/datastructures/queue/ConcurrentQueues.h"
#include "uvent/utils/datastructures/queue/FastQueue.h"

#include "ureflect/ureflect_auto.h"

namespace usub::ulog {
    enum class Level : uint8_t {
        Trace,
        Debug,
        Info,
        Warn,
        Error,
        Critical,
        Fatal
    };

    static inline constexpr size_t LEVEL_COUNT = 7;

    inline constexpr const char *level_name(Level lvl) noexcept {
        switch (lvl) {
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

    struct AnsiColors {
        const char *trace_prefix = "\x1b[90m";
        const char *debug_prefix = "\x1b[36m";
        const char *info_prefix = "\x1b[32m";
        const char *warn_prefix = "\x1b[33m";
        const char *error_prefix = "\x1b[31m";
        const char *critical_prefix = "\x1b[91m";
        const char *fatal_prefix = "\x1b[95m";
        const char *reset = "\x1b[0m";
    };

    struct LogEntry {
        uint64_t ts_ms{};
        uint32_t thread_id{};
        Level level{};
        std::string msg;
    };

    struct ULogInit {
        const char *trace_path = nullptr;
        const char *debug_path = nullptr;
        const char *info_path = nullptr;
        const char *warn_path = nullptr;
        const char *error_path = nullptr;
        const char *critical_path = nullptr;
        const char *fatal_path = nullptr;

        uint64_t flush_interval_ns = 2'000'000ULL;
        std::size_t queue_capacity = 16384;
        std::size_t batch_size = 512;
        bool enable_color_stdout = true;
        std::size_t max_file_size_bytes = 0;
        uint32_t max_files = 3;
        bool json_mode = false;
        bool track_metrics = false;
    };

    class Logger {
    public:
        ~Logger() {
            std::cout << "dtor Logger" << std::endl;
        }

        static void init_internal(const ULogInit &cfg) noexcept;

        static void shutdown_internal() noexcept;

        static inline Logger &instance() noexcept {
            return *global_.load(std::memory_order_acquire);
        }

        static inline Logger *try_instance() noexcept {
            return global_.load(std::memory_order_acquire);
        }

        static constexpr std::size_t kMaxLogLineBytes = 64 * 1024;

        static inline void enqueue_with_overflow(Level lvl, const char *msg_data, uint32_t msg_len) noexcept {
            Logger *lg = try_instance();
            if (!lg) return;
            if (lg->is_shutting_down()) return;

            LogEntry entry{};
            entry.ts_ms = now_ms_wallclock();
            entry.thread_id = get_thread_id_fast();
            entry.level = lvl;

            const uint32_t safe_len = utf8_safe_size(msg_data, msg_len, kMaxLogLineBytes);
            entry.msg.assign(msg_data, msg_data + safe_len);

            if (lg->queue_.try_enqueue(entry)) {
                if (!lg->flusher_running())
                    lg->flush_once_batch();
                return;
            }

            if (lg->track_metrics_)
                lg->metric_overflows_.fetch_add(1, std::memory_order_relaxed); {
                std::lock_guard<std::mutex> lk(lg->fallback_mutex_);
                lg->fallback_queue_.enqueue(std::move(entry));
            }

            if (!lg->flusher_running())
                lg->flush_once_batch();
        }

        template<typename... Args>
        static inline void pushf(Level lvl, std::string_view fmt, Args &&... args) noexcept {
            std::string msg;
            msg.reserve(512);
            fmt_build(msg, fmt, std::forward<Args>(args)...);
            const uint32_t len = utf8_safe_size(msg.data(), msg.size(), kMaxLogLineBytes);
            enqueue_with_overflow(lvl, msg.data(), len);
        }

        static inline void push(Level lvl, const char *fmt, ...) noexcept {
            std::vector<char> buf(256);
            va_list ap;
            va_start(ap, fmt);
            for (;;) {
                va_list ap2;
                va_copy(ap2, ap);
                int needed = ::vsnprintf(buf.data(), buf.size(), fmt, ap2);
                va_end(ap2);
                if (needed < 0) {
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

        inline uint64_t flush_interval_ns() const noexcept {
            return flush_interval_ns_;
        }

        inline uint64_t get_overflow_events() const noexcept {
            return metric_overflows_.load(std::memory_order_relaxed);
        }

        inline bool is_shutting_down() const noexcept {
            return shutting_down_.load(std::memory_order_acquire);
        }

        inline void mark_flusher_started() noexcept {
            flusher_started_.store(true, std::memory_order_release);
        }

        inline bool flusher_running() const noexcept {
            return flusher_started_.load(std::memory_order_acquire);
        }

    private:
        struct Sink {
            int fd{};
            const char *path{};
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

        Logger(const Logger &) = delete;

        Logger &operator=(const Logger &) = delete;

        static inline uint64_t now_ms_wallclock() noexcept {
            using namespace std::chrono;
            return duration_cast<milliseconds>(
                        system_clock::now().time_since_epoch())
                    .count();
        }

        static inline uint32_t get_thread_id_fast() noexcept {
            static thread_local uint32_t tls_thread_id_cache = 0;
            if (tls_thread_id_cache != 0) return tls_thread_id_cache;
            uint32_t rt = uvent::system::this_thread::detail::t_id;
            bool valid = (rt != 0u) && (rt != 0xFFFFFFFFu);
            if (!valid) {
                rt = (uint32_t) ((reinterpret_cast<uintptr_t>(&tls_thread_id_cache)) & 0xFFFFu);
                if (rt == 0u) rt = 1u;
            }
            tls_thread_id_cache = rt;
            return rt;
        }

        static std::string build_timestamp_string(uint64_t ts_ms);

        static std::string format_prefix_plain(const LogEntry &e);

        static void color_codes_for(Level lvl, const char *&start, const char *&end, bool enabled) noexcept;

        template<class T>
        using rmcvref_t = std::remove_cv_t<std::remove_reference_t<T> >;

        template<class T>
        struct is_optional : std::false_type {
        };

        template<class U>
        struct is_optional<std::optional<U> > : std::true_type {
        };

        template<class T>
        static inline constexpr bool is_optional_v = is_optional<rmcvref_t<T> >::value;

        template<class T>
        struct is_variant : std::false_type {
        };

        template<class... U>
        struct is_variant<std::variant<U...> > : std::true_type {
        };

        template<class T>
        static inline constexpr bool is_variant_v = is_variant<rmcvref_t<T> >::value;

        template<class T>
        struct is_std_array : std::false_type {
        };

        template<class U, std::size_t N>
        struct is_std_array<std::array<U, N> > : std::true_type {
        };

        template<class T>
        static inline constexpr bool is_std_array_v = is_std_array<rmcvref_t<T> >::value;

        template<class T>
        struct is_pair : std::false_type {
        };

        template<class A, class B>
        struct is_pair<std::pair<A, B> > : std::true_type {
        };

        template<class T>
        static inline constexpr bool is_pair_v = is_pair<rmcvref_t<T> >::value;

        template<class T>
        static inline constexpr bool is_cstr_v =
                std::is_same_v<rmcvref_t<T>, const char *> || std::is_same_v<rmcvref_t<T>, char *>;

        template<class T>
        static inline constexpr bool is_sv_convertible_v =
                std::is_convertible_v<T, std::string_view>;

        template<class T>
        static inline constexpr bool is_range_v =
                requires(const rmcvref_t<T> &x) { std::begin(x); std::end(x); } &&
                !is_sv_convertible_v<T> &&
                !is_optional_v<T> &&
                !is_variant_v<T> &&
                !is_std_array_v<T>;

        template<class T, class = void>
        struct ureflect_member_count : std::integral_constant<std::size_t, 0> {
        };

        template<class T>
        struct ureflect_member_count<T, std::void_t<decltype(ureflect::count_members<T>)> >
                : std::integral_constant<std::size_t, (std::size_t) ureflect::count_members<T>> {
        };

        template<class T>
        static inline constexpr std::size_t ureflect_member_count_v = ureflect_member_count<T>::value;

        template<class T>
        static inline constexpr bool is_reflectable_aggregate_v =
                std::is_aggregate_v<rmcvref_t<T> > &&
                !std::is_array_v<rmcvref_t<T> > &&
                !is_std_array_v<T> &&
                !is_range_v<T> &&
                !is_optional_v<T> &&
                !is_variant_v<T> &&
                (ureflect_member_count_v<rmcvref_t<T> > > 0);

        static inline void append_cstr(std::string &out, const char *s) noexcept {
            if (!s) {
                out.append("null");
                return;
            }
            out.append(s);
        }

        template<class T>
        static inline void append_int(std::string &out, T v) noexcept {
            char b[64];
            if constexpr (std::is_signed_v<T>) {
                int n = ::snprintf(b, sizeof(b), "%lld", (long long) v);
                if (n > 0) out.append(b, (size_t) n);
            } else {
                int n = ::snprintf(b, sizeof(b), "%llu", (unsigned long long) v);
                if (n > 0) out.append(b, (size_t) n);
            }
        }

        template<std::size_t I = 0, class Tup>
        static inline bool append_arg_at(std::string &out, Tup &&t, std::size_t idx) noexcept {
            constexpr std::size_t N = std::tuple_size_v<std::remove_reference_t<Tup> >;
            if constexpr (I >= N) {
                return false;
            } else {
                if (idx == I) {
                    append_one(out, std::get<I>(t));
                    return true;
                }
                return append_arg_at<I + 1>(out, std::forward<Tup>(t), idx);
            }
        }

        template<class T>
        static inline void append_value(std::string &out, const T &v, int depth) noexcept {
            if (depth > 16) {
                out.append("...");
                return;
            }

            using U = rmcvref_t<T>;

            if constexpr (std::is_same_v<U, std::nullptr_t>) {
                out.append("null");
            } else if constexpr (std::is_same_v<U, std::nullopt_t>) {
                out.append("null");
            } else if constexpr (is_cstr_v<T>) {
                append_cstr(out, (const char *) v);
            } else if constexpr (std::is_same_v<U, std::string>) {
                out.append(v);
            } else if constexpr (is_sv_convertible_v<T>) {
                std::string_view sv(v);
                out.append(sv.data(), sv.size());
            } else if constexpr (std::is_same_v<U, bool>) {
                out.append(v ? "true" : "false");
            } else if constexpr (std::is_enum_v<U>) {
                using E = std::underlying_type_t<U>;
                append_int(out, (E) v);
            } else if constexpr (std::is_integral_v<U>) {
                append_int(out, v);
            } else if constexpr (std::is_floating_point_v<U>) {
                char b[64];
                int n = ::snprintf(b, sizeof(b), "%g", (double) v);
                if (n > 0) out.append(b, (size_t) n);
            } else if constexpr (is_optional_v<T>) {
                if (!v) out.append("null");
                else append_value(out, *v, depth + 1);
            } else if constexpr (is_variant_v<T>) {
                std::visit([&](const auto &x) { append_value(out, x, depth + 1); }, v);
            } else if constexpr (std::is_array_v<U>) {
                out.push_back('[');
                bool first = true;
                for (const auto &x: v) {
                    if (!first) out.append(", ");
                    first = false;
                    append_value(out, x, depth + 1);
                }
                out.push_back(']');
            } else if constexpr (is_std_array_v<T>) {
                out.push_back('[');
                bool first = true;
                for (const auto &x: v) {
                    if (!first) out.append(", ");
                    first = false;
                    append_value(out, x, depth + 1);
                }
                out.push_back(']');
            } else if constexpr (is_pair_v<T>) {
                out.push_back('{');
                append_value(out, v.first, depth + 1);
                out.append(", ");
                append_value(out, v.second, depth + 1);
                out.push_back('}');
            } else if constexpr (is_range_v<T>) {
                out.push_back('[');
                bool first = true;
                for (const auto &x: v) {
                    if (!first) out.append(", ");
                    first = false;
                    append_value(out, x, depth + 1);
                }
                out.push_back(']');
            } else if constexpr (is_reflectable_aggregate_v<T>) {
                out.push_back('{');
                bool first = true;

                auto &nc = const_cast<U &>(v);
                ureflect::for_each_field(nc, [&](std::string_view name, auto &field) {
                    if (!first) out.append(", ");
                    first = false;
                    out.append(name.data(), name.size());
                    out.push_back('=');
                    append_value(out, field, depth + 1);
                });

                out.push_back('}');
            } else {
                char b[64];
                int n = ::snprintf(b, sizeof(b), "%p", (const void *) &v);
                if (n > 0) out.append(b, (size_t) n);
            }
        }

        template<typename T>
        static inline void append_one(std::string &out, const T &v) noexcept {
            append_value(out, v, 0);
        }

        static inline void fmt_build(std::string &out, std::string_view fmt) noexcept {
            out.append(fmt);
        }

        template<typename... Args>
        static inline void fmt_build(std::string &out, std::string_view fmt, Args &&... args) noexcept {
            auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
            std::size_t next_seq = 0;

            const char *p = fmt.data();
            const char *e = p + fmt.size();

            auto append_literal = [&](const char *b, const char *x) {
                if (x > b) out.append(b, (size_t) (x - b));
            };

            const char *lit = p;
            while (p < e) {
                if (*p != '{') {
                    ++p;
                    continue;
                }

                if (p + 1 < e && p[1] == '{') {
                    append_literal(lit, p);
                    out.push_back('{');
                    p += 2;
                    lit = p;
                    continue;
                }

                const char *brace = p;
                ++p;

                std::size_t idx = 0;
                bool has_idx = false;

                while (p < e && *p >= '0' && *p <= '9') {
                    has_idx = true;
                    idx = idx * 10 + (std::size_t) (*p - '0');
                    ++p;
                }

                if (p >= e || *p != '}') {
                    p = brace + 1;
                    continue;
                }
                ++p;

                append_literal(lit, brace);

                std::size_t want = has_idx ? idx : next_seq++;
                if (!append_arg_at(out, tup, want)) {
                    out.push_back('{');
                    if (has_idx) {
                        char b[32];
                        int n = ::snprintf(b, sizeof(b), "%zu", idx);
                        if (n > 0) out.append(b, (size_t) n);
                    }
                    out.push_back('}');
                }

                lit = p;
            }

            append_literal(lit, e);
        }

        static inline uint32_t utf8_safe_size(const char *data, size_t len, size_t max_bytes) {
            size_t i = std::min(len, max_bytes);
            if (i == 0) return 0;
            i--;
            while (i > 0 && (static_cast<unsigned char>(data[i]) & 0xC0) == 0x80) --i;
            return static_cast<uint32_t>(i + 1);
        }

        void maybe_rotate_sink(size_t idx, size_t incoming_bytes) noexcept;

        static void rotate_files(const char *path, uint32_t max_files) noexcept;

    private:
        static inline std::atomic<Logger *> global_{nullptr};

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
