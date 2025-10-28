# ULog

**ULog** is a high-performance, zero-allocation asynchronous logger designed to integrate tightly with the `uvent`
runtime.

---

## Key Features

- Lock-free MPMC queue (multi-producer, multi-consumer)
- Per-thread overflow buffer — prevents log loss during bursts
- Coroutine-based flusher (`logger_flush_task()`) running inside `uvent`
- Fully non-blocking, zero heap allocations on the hot path
- spdlog-compatible API (`ulog::info("user {} connected", id)`)
- Colored output support for TTY
- Per-level log files (trace/debug/info/warn/error)
- Human-readable timestamps with millisecond precision
- Safe shutdown (`ulog::shutdown()`) drains the queue and closes files
- UTF-8 safe logging (no re-encoding)

ULog does **not** spawn extra threads.  
The flusher coroutine executes in your existing `uvent::Uvent` worker pool.

---

## Log Format

```

[2025-10-28 12:03:44.861][3][I] starting event loop...

```

Where:

- `2025-10-28 12:03:44.861` → local timestamp (ms resolution)
- `3` → thread ID (worker index or pseudo-id)
- `[I]` → log level (`T`=Trace, `D`=Debug, `I`=Info, `W`=Warn, `E`=Error)

If stdout is a TTY, color is automatically enabled:

| Level | Color  |
|-------|--------|
| Trace | Gray   |
| Debug | Cyan   |
| Info  | Green  |
| Warn  | Yellow |
| Error | Red    |

---

## Log Levels

| Function                | Alias | Description                      |
|-------------------------|-------|----------------------------------|
| `ulog::trace(fmt, ...)` | `T`   | Detailed trace output            |
| `ulog::debug(fmt, ...)` | `D`   | Development debug logs           |
| `ulog::info(fmt, ...)`  | `I`   | General information              |
| `ulog::warn(fmt, ...)`  | `W`   | Warnings, recoverable issues     |
| `ulog::error(fmt, ...)` | `E`   | Critical or unrecoverable errors |

Levels control both:

1. Which output file the entry is written to, and
2. The color/style of the prefix in TTY.

---

## Example: Initialization

```cpp
#include "ulog/ulog.h"

int main() {
    usub::ulog::ULogInit cfg{
        .trace_path          = "./trace.log",
        .debug_path          = "./debug.log",
        .info_path           = "./info.log",
        .warn_path           = "./warn.log",
        .error_path          = "./error.log",
        .flush_interval_ns   = 2'000'000ULL,  // 2 ms
        .queue_capacity_pow2 = 14,            // 2^14 = 16384 entries (cells)
        .batch_size          = 512,
        .enable_color_stdout = true
    };

    ulog::init(cfg);
    ulog::info("Logger initialized, queue_size={}, flush={}ns", 1 << cfg.queue_capacity_pow2, cfg.flush_interval_ns);
}
```

---

## Example: Logging

```cpp
ulog::trace("Initializing module '{}'", "auth");
ulog::debug("connected user_id={} ip={}", 42, "127.0.0.1");
ulog::info("processing order id={}", 1834);
ulog::warn("slow request latency_ms={}", 211.5);
ulog::error("failed to connect to database: {}", "timeout");
```

Output:

```
[2025-10-28 12:03:44.861][1][T] Initializing module 'auth'
[2025-10-28 12:03:44.862][1][D] connected user_id=42 ip=127.0.0.1
[2025-10-28 12:03:44.864][1][I] processing order id=1834
[2025-10-28 12:03:45.074][1][W] slow request latency_ms=211.5
[2025-10-28 12:03:45.080][1][E] failed to connect to database: timeout
```

---

## Flusher Coroutine

ULog uses a background coroutine to periodically flush batched logs to disk.

```cpp
usub::uvent::task::spawn_detached(ulog::logger_flush_task());
```

It runs inside your `uvent` workers and sleeps for `flush_interval_ns` between batches.

---

## Shutdown

Before exit:

```cpp
ulog::shutdown();
```

This ensures all pending messages are flushed and file descriptors closed.

---

## UTF-8 Support

All messages are written raw (no transcoding).
UTF-8 is fully supported, provided the terminal or log viewer supports it.

```cpp
ulog::info("ユーザー {} がログインしました", username);
ulog::warn("Ошибка соединения с сервером №{}", server_id);
```

---

## Performance Notes

| Feature           | Type                       | Notes                               |
|-------------------|----------------------------|-------------------------------------|
| Queue             | MPMC lock-free             | preallocated ring buffer            |
| Message buffer    | fixed (default 4096 bytes) | same as `SPDLOG_MAX_MESSAGE_LENGTH` |
| Flush             | coroutine                  | no dedicated thread                 |
| Allocation        | none on hot path           | full zero-alloc design              |
| Overflow handling | TLS fallback               | avoids data loss under burst load   |

---

## Compare with spdlog

| Aspect            | spdlog                                    | ULog                                 |
|-------------------|-------------------------------------------|--------------------------------------|
| Threading         | Dedicated worker thread                   | Coroutine on existing `uvent` worker |
| Queue type        | Blocking ring buffer                      | Lock-free MPMC                       |
| Allocation        | May allocate during formatting            | Zero heap on hot path                |
| API style         | `{}` formatting                           | same syntax                          |
| File separation   | Supported                                 | per-level paths supported            |
| Color             | via `spdlog::sinks::stdout_color_sink_mt` | built-in                             |
| UTF-8             | full                                      | full                                 |
| Overload handling | drop/block                                | non-blocking + TLS overflow buffer   |

---

That’s the complete English documentation skeleton.
You can now add extra Markdown pages like `docs/config.md` or `docs/api/logger.md` for deeper API details — I can
generate those next if you want (with field-by-field breakdown and code references).
