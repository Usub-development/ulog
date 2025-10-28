#include "ulog/Logger.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstdio>

namespace usub::ulog
{
    static int open_or_fallback(const char* path, int fallback_fd)
    {
        if (!path) return fallback_fd;
        int fd = ::open(path, O_CREAT | O_APPEND | O_WRONLY, 0644);
        if (fd < 0) return fallback_fd;
        return fd;
    }

    void Logger::rotate_files(const char* path, uint32_t max_files) noexcept
    {
        if (!path) return;
        if (max_files == 0) return;
        if (max_files == 1)
        {
            char dst[512];
            ::snprintf(dst, sizeof(dst), "%s.%u", path, 1u);
            ::unlink(dst);
            ::rename(path, dst);
            return;
        }

        {
            char oldname[512];
            ::snprintf(oldname, sizeof(oldname), "%s.%u", path, max_files - 1);
            ::unlink(oldname);
        }

        for (int i = (int)max_files - 2; i >= 1; --i)
        {
            char src[512];
            char dst[512];
            ::snprintf(src, sizeof(src), "%s.%d", path, i);
            ::snprintf(dst, sizeof(dst), "%s.%d", path, i + 1);
            ::rename(src, dst);
        }

        {
            char dst[512];
            ::snprintf(dst, sizeof(dst), "%s.%u", path, 1u);
            ::rename(path, dst);
        }
    }

    void Logger::maybe_rotate_sink(size_t idx, size_t incoming_bytes) noexcept
    {
        Sink& s = sinks_[idx];

        if (!s.path) return;
        if (max_file_size_bytes_ == 0) return;

        size_t next_size = s.bytes_written + incoming_bytes;
        if (next_size < max_file_size_bytes_) return;

        ::fsync(s.fd);
        if (s.fd != 1 && s.fd != 2)
        {
            ::close(s.fd);
        }

        rotate_files(s.path, max_files_);

        int new_fd = ::open(s.path, O_CREAT | O_APPEND | O_WRONLY, 0644);
        if (new_fd < 0)
        {
            new_fd = 1;
            s.path = nullptr;
        }

        s.fd = new_fd;
        s.bytes_written = 0;
        s.color_enabled = ::isatty(new_fd);
    }

    void Logger::init_internal(const ULogInit& cfg) noexcept
    {
        if (global_ != nullptr)
            return;

        int base_fd = -1;
        {
            int tmp = -1;
            if (cfg.info_path)
            {
                tmp = ::open(cfg.info_path, O_CREAT | O_APPEND | O_WRONLY, 0644);
            }
            if (tmp < 0)
                tmp = 1;
            base_fd = tmp;
        }

        Sink local_sinks[LEVEL_COUNT];

        auto init_sink = [&](Level lvl, const char* path)
        {
            Sink s{};
            s.fd = open_or_fallback(path, base_fd);
            s.path = path;
            s.bytes_written = 0;
            bool allow_color = cfg.enable_color_stdout && ::isatty(s.fd);
            s.color_enabled = allow_color;
            local_sinks[(size_t)lvl] = s;
        };

        init_sink(Level::Trace, cfg.trace_path);
        init_sink(Level::Debug, cfg.debug_path);
        init_sink(Level::Info, cfg.info_path);
        init_sink(Level::Warn, cfg.warn_path);
        init_sink(Level::Error, cfg.error_path);

        std::size_t bs = cfg.batch_size;
        if (bs == 0) bs = 1;
        if (bs > 4096) bs = 4096;

        global_ = new Logger(
            local_sinks,
            cfg.queue_capacity_pow2,
            bs,
            cfg.flush_interval_ns,
            cfg.max_file_size_bytes,
            cfg.max_files,
            cfg.json_mode,
            cfg.track_metrics
        );
    }

    void Logger::shutdown_internal() noexcept
    {
        Logger* g = global_;
        if (!g) return;

        g->shutting_down_.store(true, std::memory_order_release);

        for (;;)
        {
            g->flush_once_batch();

            if (g->queue_.empty())
                break;

            cpu_relax();
        }

        int seen[LEVEL_COUNT];
        size_t seen_n = 0;

        for (size_t i = 0; i < LEVEL_COUNT; ++i)
        {
            int fd = g->sinks_[i].fd;
            bool already = false;
            for (size_t j = 0; j < seen_n; ++j)
            {
                if (seen[j] == fd)
                {
                    already = true;
                    break;
                }
            }
            if (!already)
            {
                ::fsync(fd);
                if (fd != 1 && fd != 2)
                    ::close(fd);
                seen[seen_n++] = fd;
            }
        }
    }

    void Logger::flush_once_batch() noexcept
    {
        const std::size_t limit = batch_size_;

        static thread_local LogEntry tmp[4096];
        std::size_t n = queue_.try_dequeue_bulk(tmp, limit);
        if (n == 0)
            return;

        struct PerLevelBuf
        {
            char buf[128 * 1024];
            size_t off = 0;
        };

        static thread_local PerLevelBuf bufs[LEVEL_COUNT];

        for (size_t i = 0; i < LEVEL_COUNT; ++i)
            bufs[i].off = 0;

        auto text_mode_emit = [&](PerLevelBuf& lb, const LogEntry& e, bool color_enabled)
        {
            const char* c_begin;
            const char* c_end;
            color_codes_for(e.level, c_begin, c_end, color_enabled);

            auto append_bytes = [&](const char* data, size_t len)
            {
                if (len == 0) return;
                if (lb.off + len >= sizeof(lb.buf))
                {
                    return;
                }
                ::memcpy(lb.buf + lb.off, data, len);
                lb.off += len;
            };

            append_bytes(c_begin, ::strlen(c_begin));

            char prefix[160];
            size_t pref_len = format_prefix_plain(e, prefix, sizeof(prefix));
            append_bytes(prefix, pref_len);

            size_t msg_len = e.size;
            if (msg_len > sizeof(e.msg)) msg_len = sizeof(e.msg);
            append_bytes(e.msg, msg_len);

            append_bytes("\n", 1);

            append_bytes(c_end, ::strlen(c_end));
        };

        auto json_mode_emit = [&](PerLevelBuf& lb, const LogEntry& e)
        {
            char tsbuf[32];
            size_t tslen = build_timestamp_string(e.ts_ms, tsbuf, sizeof(tsbuf));

            char line[8192];
            size_t w = 0;

            auto put = [&](const char* s, size_t len)
            {
                if (len == 0) return;
                size_t room = sizeof(line) - w;
                if (room == 0) return;
                if (len > room) len = room;
                ::memcpy(line + w, s, len);
                w += len;
            };
            auto put_ch = [&](char c)
            {
                if (w < sizeof(line)) line[w++] = c;
            };

            put("{\"time\":\"", 9);
            put(tsbuf, tslen);
            put("\",\"thread\":", 12);

            {
                char tidbuf[32];
                int tidn = ::snprintf(tidbuf, sizeof(tidbuf), "%u", e.thread_id);
                if (tidn > 0) put(tidbuf, (size_t)tidn);
            }

            put(",\"level\":\"", 11);
            {
                const char* lvlc = level_name(e.level);
                put(lvlc, 1);
            }
            put("\",\"msg\":\"", 10);

            size_t msg_len = e.size;
            if (msg_len > sizeof(e.msg)) msg_len = sizeof(e.msg);
            for (size_t k = 0; k < msg_len; ++k)
            {
                unsigned char c = (unsigned char)e.msg[k];
                if (c == '"' || c == '\\')
                {
                    put("\\", 1);
                    put((const char*)&c, 1);
                }
                else if (c == '\n')
                {
                    put("\\n", 2);
                }
                else if (c == '\r')
                {
                    put("\\r", 2);
                }
                else if (c == '\t')
                {
                    put("\\t", 2);
                }
                else
                {
                    put((const char*)&c, 1);
                }
            }

            put("\"}\n", 3);

            if (lb.off + w < sizeof(lb.buf))
            {
                ::memcpy(lb.buf + lb.off, line, w);
                lb.off += w;
            }
        };

        for (std::size_t i = 0; i < n; ++i)
        {
            const LogEntry& e = tmp[i];
            size_t idx = (size_t)e.level;

            if (!json_mode_)
            {
                text_mode_emit(bufs[idx], e, sinks_[idx].color_enabled);
            }
            else
            {
                json_mode_emit(bufs[idx], e);
            }
        }

        for (size_t i = 0; i < LEVEL_COUNT; ++i)
        {
            PerLevelBuf& lb = bufs[i];
            if (lb.off == 0)
                continue;

            maybe_rotate_sink(i, lb.off);

            Sink& s = sinks_[i];

            ssize_t wr = ::write(s.fd, lb.buf, lb.off);
            if (wr > 0)
            {
                s.bytes_written += (size_t)wr;
            }
        }
    }
} // namespace usub::ulog
