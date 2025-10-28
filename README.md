# 🪶 ULog — Ultra-Low Overhead Async Logger for Uvent

**ULog** is a zero-allocation, coroutine-native asynchronous logger for the [
`uvent`](https://github.com/Usub-development/uvent) runtime.

It delivers **spdlog-level performance** without using extra threads, locks, or external dependencies.

---

## 🚀 Features

| Feature                       | Description                                                   |
|-------------------------------|---------------------------------------------------------------|
| ⚡️ **Zero-overhead hot path** | Log calls (`ulog::info`, etc.) never block or allocate memory |
| 🧩 **MPMC lock-free queue**   | Multi-producer, single-consumer batching                      |
| 🧠 **Coroutine flusher**      | Runs entirely inside `uvent` — no thread pools                |
| 📦 **Per-level sinks**        | Separate files for TRACE/DEBUG/INFO/WARN/ERROR                |
| 🎨 **Color output**           | ANSI colors for TTY when `enable_color_stdout=true`           |
| 🪶 **JSON mode**              | Structured output, one JSON object per line                   |
| 🔁 **File rotation**          | Automatic size-based rotation with retention limit            |
| 🧷 **Guaranteed delivery**    | Per-thread overflow buffer prevents log loss                  |
| 📊 **Metrics**                | Tracks queue saturation and overflow usage                    |
| 🌐 **UTF-8 safe**             | Truncates only on valid code-point boundaries                 |
| 🧱 **Zero dependencies**      | Pure C++23, no `fmt`, no `spdlog`, no `libstdc++` extensions  |

---

## 🧩 Integration with Uvent

ULog runs *inside your existing* event loop — no threads, no sleeps, no `std::async`.

```cpp
#include "ulog/ulog.h"
#include "ulog/LoggerFlushTask.h"
#include "uvent/Uvent.h"

using namespace usub;

int main() {
    // 1. Configure logger
    ulog::ULogInit cfg{
        .trace_path          = "./trace.log",
        .debug_path          = "./debug.log",
        .info_path           = "./info.log",
        .warn_path           = "./warn.log",
        .error_path          = "./error.log",
        .flush_interval_ns   = 2'000'000ULL,  // 2ms
        .queue_capacity_pow2 = 14,            // 2^14 = 16384 entries
        .batch_size          = 512,
        .enable_color_stdout = true,
        .max_file_size_bytes = 10 * 1024 * 1024, // rotate at 10 MB
        .max_files           = 3,
        .json_mode           = false,
        .track_metrics       = true
    };

    ulog::init(cfg);

    // 2. Run Uvent event loop
    Uvent loop(4);
    loop.run();

    // 3. Shutdown cleanly
    ulog::shutdown();
}
````

---

## 🪵 Logging API

ULog provides a familiar interface matching **spdlog/fmt-style formatting**:

```cpp
ulog::trace("user {} connected from {}", user_id, ip);
ulog::debug("cache hit key={}", key);
ulog::info("started module '{}'", module_name);
ulog::warn("slow request latency_ms={}", latency_ms);
ulog::error("db connect failed: {}", err_msg);
```

Each call:

1. Builds a formatted UTF-8 string on stack
2. Pushes it into a lock-free global queue
3. Returns immediately (no disk access)

Actual writes and rotation happen later inside the coroutine flusher.

---

## 🧾 Output examples

### Text mode

```
[2025-10-28 12:03:44.861][3][I] starting event loop...
```

### JSON mode

```json
{"time":"2025-10-28 12:03:44.861","thread":3,"level":"I","msg":"starting event loop..."}
```

---

## 🔁 File rotation

Each level has its own sink.
If the current file exceeds `max_file_size_bytes`, ULog:

1. `fsync()`s and closes it
2. Renames `file.log` → `file.log.1`, shifts older files up to `.max_files`
3. Opens a fresh `file.log`
4. Writes the next batch into the new file

Rotation is **atomic per batch** — a log batch is never split across files.

---

## 📊 Metrics

Enable metrics with:

```cpp
.track_metrics = true
```

Then inspect them at runtime:

```cpp
auto st = ulog::stats();
ulog::info("ulog: overflow_pushes={}, backpressure_spins={}",
           st.overflow_pushes,
           st.backpressure_spins);
```

* `overflow_pushes`: how many times the main queue was full
* `backpressure_spins`: how often producers had to spin because even overflow buffers were full

---

## ⚙️ Configuration summary

```cpp
struct ULogInit {
    const char* trace_path;
    const char* debug_path;
    const char* info_path;
    const char* warn_path;
    const char* error_path;
    uint64_t    flush_interval_ns;
    std::size_t queue_capacity_pow2;
    std::size_t batch_size;
    bool        enable_color_stdout;
    std::size_t max_file_size_bytes;
    uint32_t    max_files;
    bool        json_mode;
    bool        track_metrics;
};
```

All fields have defaults:

* all levels → stdout
* rotation disabled
* color enabled if tty
* queue = 2¹⁴ entries
* flush interval = 2ms
* batch size = 512

---

## 🧠 Internals

* MPMC queue between producers and the flusher
* Per-thread overflow ring (TLS)
* One coroutine handles all I/O and rotation
* No allocation after startup
* No mutexes
* UTF-8 validated truncation

### Thread ID

* If running inside `uvent`: prints worker index
* Otherwise: uses a stable pseudo-ID derived from TLS

---

## 🔍 Comparison: spdlog vs ULog

| Feature                | spdlog::async_logger | ulog                    |
|------------------------|----------------------|-------------------------|
| Thread pool            | ✅ yes                | ❌ none (uses coroutine) |
| Lock-free queue        | ✅                    | ✅                       |
| Overflow handling      | drop / block         | no-loss spill buffer    |
| Rotation               | ✅                    | ✅                       |
| JSON logs              | via sink             | built-in                |
| Compile-time filtering | ✅                    | ❌                       |
| ANSI color             | ✅                    | ✅                       |
| Metrics                | ❌                    | ✅                       |
| UTF-8 safe truncation  | ⚙️ partial           | ✅ strict                |
| Zero dependencies      | ❌ (`fmt`, `spdlog`)  | ✅ pure C++23            |

---

## 📦 Repository structure

```
include/
 └── ulog/
      ├── Logger.h
      ├── LoggerFlushTask.h
      └── ulog.h
docs/
 ├── index.md
 ├── getting-started.md
 ├── config.md
 └── api/
      ├── logger.md
      ├── logger-flush-task.md
      └── internals.md
```

---

## 🧰 Build

```bash
mkdir build && cd build
cmake ..
make -j
```

Requires **C++23** and **uvent** available as a submodule or system include.

---

## 🧩 License

MIT License — same as [`uvent`](https://github.com/Usub-development/uvent) and [
`upq`](https://github.com/Usub-development/upq).