#include "ulog/Logger.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <memory>

namespace usub::ulog {
    static int open_or_fallback(const char *path, int fallback_fd) {
        if (!path) return fallback_fd;
        int fd = ::open(path, O_CREAT | O_APPEND | O_WRONLY, 0644);
        if (fd < 0) return fallback_fd;
        return fd;
    }

    void Logger::rotate_files(const char *path, uint32_t max_files) noexcept {
        if (!path || max_files == 0) return;

        auto make_name = [&](uint32_t idx) {
            std::string s;
            s.reserve(std::strlen(path) + 16);
            s.append(path);
            s.push_back('.');
            char buf[32];
            int n = ::snprintf(buf, sizeof(buf), "%u", idx);
            if (n > 0) s.append(buf, (size_t) n);
            return s;
        };

        if (max_files == 1) {
            auto dst = make_name(1);
            ::unlink(dst.c_str());
            ::rename(path, dst.c_str());
            return;
        } {
            auto oldname = make_name(max_files - 1);
            ::unlink(oldname.c_str());
        }
        for (int i = (int) max_files - 2; i >= 1; --i) {
            auto src = make_name((uint32_t) i);
            auto dst = make_name((uint32_t) (i + 1));
            ::rename(src.c_str(), dst.c_str());
        } {
            auto dst = make_name(1);
            ::rename(path, dst.c_str());
        }
    }

    void Logger::maybe_rotate_sink(size_t idx, size_t incoming_bytes) noexcept {
        Sink &s = sinks_[idx];
        if (!s.path || max_file_size_bytes_ == 0) return;

        size_t next_size = s.bytes_written + incoming_bytes;
        if (next_size < max_file_size_bytes_) return;

        ::fsync(s.fd);
        if (s.fd != 1 && s.fd != 2) ::close(s.fd);

        rotate_files(s.path, max_files_);

        int new_fd = ::open(s.path, O_CREAT | O_APPEND | O_WRONLY, 0644);
        if (new_fd < 0) {
            new_fd = 1;
            s.path = nullptr;
        }

        s.fd = new_fd;
        s.bytes_written = 0;
        s.color_enabled = ::isatty(new_fd);
    }

    void Logger::init_internal(const ULogInit &cfg) noexcept {
        if (global_.load(std::memory_order_acquire)) return;

        int base_fd = -1; {
            int tmp = -1;
            if (cfg.info_path)
                tmp = ::open(cfg.info_path, O_CREAT | O_APPEND | O_WRONLY, 0644);
            if (tmp < 0) tmp = 1;
            base_fd = tmp;
        }

        std::vector<Sink> local_sinks_vec(LEVEL_COUNT);
        auto init_sink = [&](Level lvl, const char *path) {
            Sink s{};
            s.fd = open_or_fallback(path, base_fd);
            s.path = path;
            s.bytes_written = 0;
            s.color_enabled = cfg.enable_color_stdout && ::isatty(s.fd);
            local_sinks_vec[(size_t) lvl] = s;
        };

        init_sink(Level::Trace, cfg.trace_path);
        init_sink(Level::Debug, cfg.debug_path);
        init_sink(Level::Info, cfg.info_path);
        init_sink(Level::Warn, cfg.warn_path);
        init_sink(Level::Error, cfg.error_path);
        init_sink(Level::Critical, cfg.critical_path ? cfg.critical_path : cfg.error_path);
        init_sink(Level::Fatal, cfg.fatal_path ? cfg.fatal_path : cfg.error_path);

        std::size_t bs = cfg.batch_size ? std::min<std::size_t>(cfg.batch_size, 4096) : 1;

        Logger::Sink local_sinks_arr[LEVEL_COUNT];
        for (size_t i = 0; i < LEVEL_COUNT; ++i)
            local_sinks_arr[i] = local_sinks_vec[i];

        Logger *lg = new Logger(
            local_sinks_arr,
            cfg.queue_capacity,
            bs,
            cfg.flush_interval_ns,
            cfg.max_file_size_bytes,
            cfg.max_files,
            cfg.json_mode,
            cfg.track_metrics
        );

        std::atomic_thread_fence(std::memory_order_release);
        global_.store(lg, std::memory_order_release);
    }

    void Logger::shutdown_internal() noexcept {
        Logger *g = global_.load(std::memory_order_acquire);
        if (!g) return;

        g->shutting_down_.store(true, std::memory_order_release);

        for (;;) {
            g->flush_once_batch();

            bool empty_mpmc = g->queue_.empty();
            bool empty_fallback; {
                std::lock_guard<std::mutex> lk(g->fallback_mutex_);
                empty_fallback = g->fallback_queue_.empty();
            }

            if (empty_mpmc && empty_fallback)
                break;

            cpu_relax();
        }

        int seen[LEVEL_COUNT];
        size_t seen_n = 0;

        for (size_t i = 0; i < LEVEL_COUNT; ++i) {
            int fd = g->sinks_[i].fd;
            bool already = false;
            for (size_t j = 0; j < seen_n; ++j) {
                if (seen[j] == fd) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                ::fsync(fd);
                if (fd != 1 && fd != 2) ::close(fd);
                seen[seen_n++] = fd;
            }
        }

        global_.store(nullptr, std::memory_order_release);
        delete g;
    }

    Logger::Logger(Sink sinks_init[LEVEL_COUNT],
                   std::size_t queue_capacity,
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
          , queue_(queue_capacity)
          , fallback_queue_(queue_capacity) {
        for (size_t i = 0; i < LEVEL_COUNT; ++i)
            sinks_[i] = sinks_init[i];

        metric_overflows_.store(0, std::memory_order_relaxed);
    }

    std::string Logger::build_timestamp_string(uint64_t ts_ms) {
        time_t sec = (time_t) (ts_ms / 1000);
        uint32_t msec = (uint32_t) (ts_ms % 1000);

        struct tm tmbuf;
        struct tm *tm_ptr = ::localtime_r(&sec, &tmbuf);

        int year = tm_ptr ? (tm_ptr->tm_year + 1900) : 0;
        int mon = tm_ptr ? (tm_ptr->tm_mon + 1) : 0;
        int day = tm_ptr ? tm_ptr->tm_mday : 0;
        int hr = tm_ptr ? tm_ptr->tm_hour : 0;
        int min = tm_ptr ? tm_ptr->tm_min : 0;
        int sec2 = tm_ptr ? tm_ptr->tm_sec : 0;

        auto two = [](int v) {
            std::string s;
            s.resize(2);
            s[0] = char('0' + (v / 10) % 10);
            s[1] = char('0' + (v % 10));
            return s;
        };
        auto thr = [](int v) {
            std::string s;
            s.resize(3);
            s[0] = char('0' + (v / 100) % 10);
            s[1] = char('0' + (v / 10) % 10);
            s[2] = char('0' + (v % 10));
            return s;
        };

        std::string out;
        out.reserve(23);
        out.append({
            char('0' + (year / 1000) % 10),
            char('0' + (year / 100) % 10),
            char('0' + (year / 10) % 10),
            char('0' + (year % 10))
        });
        out.push_back('-');
        out += two(mon);
        out.push_back('-');
        out += two(day);
        out.push_back(' ');
        out += two(hr);
        out.push_back(':');
        out += two(min);
        out.push_back(':');
        out += two(sec2);
        out.push_back('.');
        out += thr((int) msec);
        return out;
    }

    std::string Logger::format_prefix_plain(const LogEntry &e) {
        std::string ts = build_timestamp_string(e.ts_ms);
        std::string out;

        auto u64_to_str = [](uint64_t v) {
            char buf[32];
            int n = ::snprintf(buf, sizeof(buf), "%llu", (unsigned long long) v);
            return std::string(buf, n > 0 ? (size_t) n : 0);
        };

        out.reserve(1 + ts.size() + 1 + 1 + 10 + 1 + 3 + 2);
        out.push_back('[');
        out += ts;
        out.push_back(']');
        out.push_back('[');
        out += u64_to_str(e.thread_id);
        out.push_back(']');
        out.push_back('[');
        out.push_back(level_name(e.level)[0]);
        out.push_back(']');
        out.push_back(' ');
        return out;
    }

    void Logger::color_codes_for(Level lvl,
                                 const char *&start,
                                 const char *&end,
                                 bool enabled) noexcept {
        static const AnsiColors c{};
        if (!enabled) {
            start = "";
            end = "";
            return;
        }
        switch (lvl) {
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
            case Level::Error:
                start = c.error_prefix;
                end = c.reset;
                break;
            case Level::Critical:
                start = c.critical_prefix;
                end = c.reset;
                break;
            case Level::Fatal:
                start = c.fatal_prefix;
                end = c.reset;
                break;
            default:
                start = "";
                end = "";
                break;
        }
    }

    void Logger::flush_once_batch() noexcept {
        const std::size_t limit = batch_size_;

        std::unique_ptr<LogEntry[]> tmp(new LogEntry[limit]);

        std::size_t n = queue_.try_dequeue_bulk(tmp.get(), limit);

        if (n < limit) {
            std::lock_guard<std::mutex> lk(fallback_mutex_);
            if (!fallback_queue_.empty()) {
                n += fallback_queue_.dequeue_bulk(tmp.get() + n, limit - n);
            }
        }

        if (n == 0) return;

        std::vector<std::string> bufs(LEVEL_COUNT);

        auto text_mode_emit = [&](std::string &lb,
                                  const LogEntry &e,
                                  bool color_enabled) {
            const char *c_begin;
            const char *c_end;
            color_codes_for(e.level, c_begin, c_end, color_enabled);

            lb.append(c_begin);
            lb += format_prefix_plain(e);
            lb.append(e.msg);
            lb.push_back('\n');
            lb.append(c_end);
        };

        auto json_mode_emit = [&](std::string &lb, const LogEntry &e) {
            std::string ts = build_timestamp_string(e.ts_ms);
            std::string line;
            line.reserve(64 + ts.size() + e.msg.size());

            line.append("{\"time\":\"");
            line.append(ts);
            line.append("\",\"thread\":"); {
                char tidbuf[32];
                int tidn = ::snprintf(tidbuf, sizeof(tidbuf), "%u", e.thread_id);
                if (tidn > 0) line.append(tidbuf, (size_t) tidn);
            }
            line.append(",\"level\":\"");
            line.push_back(level_name(e.level)[0]);
            line.append("\",\"msg\":\"");

            for (unsigned char c: e.msg) {
                if (c == '"' || c == '\\') {
                    line.push_back('\\');
                    line.push_back((char) c);
                } else if (c == '\n') line.append("\\n");
                else if (c == '\r') line.append("\\r");
                else if (c == '\t') line.append("\\t");
                else line.push_back((char) c);
            }
            line.append("\"}\n");

            lb.append(line);
        };

        for (std::size_t i = 0; i < n; ++i) {
            const LogEntry &e = tmp[i];
            size_t idx = (size_t) e.level;
            if (!json_mode_)
                text_mode_emit(bufs[idx], e, sinks_[idx].color_enabled);
            else
                json_mode_emit(bufs[idx], e);
        }

        for (size_t i = 0; i < LEVEL_COUNT; ++i) {
            std::string &lb = bufs[i];
            if (lb.empty()) continue;

            maybe_rotate_sink(i, lb.size());

            Sink &s = sinks_[i];
            ssize_t wr = ::write(s.fd, lb.data(), lb.size());
            if (wr > 0) s.bytes_written += (size_t) wr;
        }
    }
} // namespace usub::ulog
