#include "ulog/Logger.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>

namespace usub::ulog
{
    static int open_or_fallback(const char* path, int fallback_fd)
    {
        if (!path) return fallback_fd;
        int fd = ::open(path, O_CREAT | O_APPEND | O_WRONLY, 0644);
        if (fd < 0) return fallback_fd;
        return fd;
    }

    Logger::Logger(int fds[LEVEL_COUNT],
                   bool color_enabled[LEVEL_COUNT],
                   std::size_t queue_capacity_pow2,
                   std::size_t batch_size,
                   uint64_t flush_interval_ns) noexcept
        : queue_(queue_capacity_pow2)
        , batch_size_(batch_size)
        , flush_interval_ns_(flush_interval_ns)
        , shutting_down_(false)
    {
        for (size_t i = 0; i < LEVEL_COUNT; ++i)
        {
            fds_[i] = fds[i];
            color_enabled_[i] = color_enabled[i];
        }
    }

    void Logger::init_internal(const ULogInit& cfg) noexcept
    {
        if (global_ != nullptr)
            return;

        // pick fallback (info_path if possible, else stdout)
        int base_fd = -1;
        {
            int tmp = -1;
            if (cfg.info_path)
            {
                tmp = ::open(cfg.info_path, O_CREAT | O_APPEND | O_WRONLY, 0644);
            }
            if (tmp < 0)
                tmp = 1; // stdout fallback
            base_fd = tmp;
        }

        int fds_local[LEVEL_COUNT];
        fds_local[(size_t)Level::Trace] = open_or_fallback(cfg.trace_path, base_fd);
        fds_local[(size_t)Level::Debug] = open_or_fallback(cfg.debug_path, base_fd);
        fds_local[(size_t)Level::Info]  = open_or_fallback(cfg.info_path,  base_fd);
        fds_local[(size_t)Level::Warn]  = open_or_fallback(cfg.warn_path,  base_fd);
        fds_local[(size_t)Level::Error] = open_or_fallback(cfg.error_path, base_fd);

        bool color_flags[LEVEL_COUNT];
        for (size_t i = 0; i < LEVEL_COUNT; ++i)
        {
            bool can_color = cfg.enable_color_stdout && ::isatty(fds_local[i]);
            color_flags[i] = can_color;
        }

        std::size_t bs = cfg.batch_size;
        if (bs == 0) bs = 1;
        if (bs > 4096) bs = 4096;

        global_ = new Logger(
            fds_local,
            color_flags,
            cfg.queue_capacity_pow2,
            bs,
            cfg.flush_interval_ns
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

        // close each unique fd once
        int seen[LEVEL_COUNT];
        size_t seen_n = 0;

        for (size_t i = 0; i < LEVEL_COUNT; ++i)
        {
            int fd = g->fds_[i];
            bool already = false;
            for (size_t j = 0; j < seen_n; ++j)
                if (seen[j] == fd)
                {
                    already = true;
                    break;
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
            char  buf[64 * 1024];
            size_t off = 0;
        };

        static thread_local PerLevelBuf bufs[LEVEL_COUNT];

        for (size_t i = 0; i < LEVEL_COUNT; ++i)
            bufs[i].off = 0;

        for (std::size_t i = 0; i < n; ++i)
        {
            const LogEntry& e = tmp[i];
            const size_t idx = (size_t)e.level;
            PerLevelBuf& lb = bufs[idx];

            const char* c_begin;
            const char* c_end;
            color_codes_for(e.level, c_begin, c_end, color_enabled_[idx]);

            auto append_bytes = [&](const char* data, size_t len) {
                if (len == 0) return;
                if (lb.off + len >= sizeof(lb.buf))
                {
                    ::write(fds_[idx], lb.buf, lb.off);
                    lb.off = 0;
                }
                ::memcpy(lb.buf + lb.off, data, len);
                lb.off += len;
            };

            // color start
            append_bytes(c_begin, ::strlen(c_begin));

            // prefix
            char prefix[128];
            size_t pref_len = format_prefix_plain(e, prefix, sizeof(prefix));
            append_bytes(prefix, pref_len);

            // message
            size_t msg_len = e.size;
            if (msg_len > sizeof(e.msg)) msg_len = sizeof(e.msg);
            append_bytes(e.msg, msg_len);

            // newline
            const char nl = '\n';
            append_bytes(&nl, 1);

            // color reset
            append_bytes(c_end, ::strlen(c_end));
        }

        // flush per-level buffers
        for (size_t i = 0; i < LEVEL_COUNT; ++i)
        {
            if (bufs[i].off)
            {
                ::write(fds_[i], bufs[i].buf, bufs[i].off);
                bufs[i].off = 0;
            }
        }
    }

} // namespace usub::ulog
