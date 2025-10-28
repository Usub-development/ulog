# Getting Started

This guide walks you through integrating **ULog** into a `uvent`-based service.

---

## 1. Include ULog

Add ULog to your project and include:

```cpp
#include "ulog/ulog.h"
#include "ulog/LoggerFlushTask.h"
```

ULog has no external runtime dependencies beyond `uvent`.

---

## 2. Initialize the logger

ULog is configured using `ULogInit`. You can either rely on defaults, or pass your own config.

### Minimal init (everything to stdout)

```cpp
int main() {
    ulog::init(); // uses default ULogInit
    // ...
}
```

This will:

* send all levels to `stdout`
* enable color if `stdout` is a TTY
* no JSON mode
* no rotation
* no metrics
* flush every 2ms
* queue size ~16k entries

### Custom init

```cpp
int main() {
    usub::ulog::ULogInit cfg{
        .trace_path          = "./trace.log",
        .debug_path          = "./debug.log",
        .info_path           = "./info.log",
        .warn_path           = "./warn.log",
        .error_path          = "./error.log",

        .flush_interval_ns   = 2'000'000ULL, // 2ms
        .queue_capacity_pow2 = 14,           // 2^14 = 16384
        .batch_size          = 512,

        .enable_color_stdout = true,

        .max_file_size_bytes = 10 * 1024 * 1024, // rotate at 10MB
        .max_files           = 3,                // keep file.log.1..file.log.3

        .json_mode           = false,            // human-readable
        .track_metrics       = true              // enable contention stats
    };

    ulog::init(cfg);
    // ...
}
```

If any `*_path` is `nullptr`, that level logs to `stdout`.

---

## 3. Start the flusher coroutine

ULog does not spawn its own thread.
Instead, it runs a flush coroutine inside your existing `uvent` worker pool:

```cpp
usub::uvent::system::co_spawn(ulog::logger_flush_task());
```

This coroutine:

* periodically flushes queued log entries
* performs file rotation if needed
* writes batched logs to sinks

You only start **one** `logger_flush_task()`.

---

## 4. Log from anywhere

```cpp
ulog::trace("trace step={} phase='{}'", step, phase);
ulog::debug("user connected id={} ip={}", user_id, ip);
ulog::info("started module '{}'", module_name);
ulog::warn("slow request latency_ms={}", latency_ms);
ulog::error("db connect failed: {}", err_msg);
```

All of these:

* format using `{}` placeholders
* enqueue the result in a lock-free MPMC ring buffer
* never block on disk I/O

---

## 5. Shutdown

Call this once at process shutdown:

```cpp
ulog::shutdown();
```

This:

* drains all pending logs from the queue
* flushes them
* rotates/flushes/closes all file descriptors cleanly

---

## 6. Metrics

If you enable `track_metrics = true` in config, you can inspect logger pressure:

```cpp
auto st = ulog::stats();
ulog::info("ulog stats: overflow_pushes={} backpressure_spins={}",
           st.overflow_pushes,
           st.backpressure_spins);
```

`overflow_pushes` means the global queue was full and a thread had to stash logs in its per-thread spill buffer.
`backpressure_spins` means even the spill buffer filled and we had to spin-push synchronously.