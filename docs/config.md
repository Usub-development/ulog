# Configuration

ULog is configured with a single struct: `ULogInit`.

```cpp
struct ULogInit {
    const char* trace_path; // per-level sink paths
    const char* debug_path;
    const char* info_path;
    const char* warn_path;
    const char* error_path;

    uint64_t    flush_interval_ns;   // flush period for the background coroutine
    std::size_t queue_capacity_pow2; // queue capacity as 2^N
    std::size_t batch_size;          // max logs written per flush
    bool        enable_color_stdout; // allow ANSI color on TTY sinks

    // rotation
    std::size_t max_file_size_bytes; // 0 = disabled
    uint32_t    max_files;           // number of rotated backups to keep

    // structured mode
    bool        json_mode;           // emit JSON lines if true

    // metrics
    bool        track_metrics;       // expose overflow/backpressure stats
};
````

All fields have sane defaults in `ulog::init()` if you don't pass a config.

---

## Output routing

Each log level can go to its own file:

```cpp
.trace_path = "./trace.log",
.debug_path = "./debug.log",
.info_path  = "./info.log",
.warn_path  = "./warn.log",
.error_path = "./error.log",
```

If the path for a level is `nullptr`, that level goes to `stdout`.

So this:

```cpp
.trace_path = nullptr,
.debug_path = nullptr,
.info_path  = "./service.log",
.warn_path  = "./service.log",
.error_path = "./service.log",
```

means:

* TRACE and DEBUG go to stdout (with color if TTY),
* INFO/WARN/ERROR go to `service.log`.

---

## Rotation

ULog supports size-based log rotation per level.

Fields:

* `max_file_size_bytes`: when the sink for a level exceeds this size, rotation triggers.
* `max_files`: how many rotated versions to keep (`file.log.1`, `file.log.2`, ...).

How it works:

1. The logger flush coroutine checks the file size **before** each write batch.
2. If the next batch would exceed `max_file_size_bytes`:

    * It `fsync()`s and closes the current file.
    * It renames:

        * `file.log.(N-1)` → `file.log.N`
        * ...
        * `file.log.1`     → `file.log.2`
        * `file.log`       → `file.log.1`
    * Then it opens a fresh new `file.log`.
3. The whole pending batch then goes into the *new* file.

No producer thread ever rotates or touches file descriptors.
Rotation is exclusively done in the flush coroutine.

If `max_file_size_bytes == 0`, rotation is disabled.

If a level is logging to stdout (path is `nullptr`), rotation is also disabled for that level.

---

## JSON mode

If `json_mode == false` (default), each line looks like:

```text
[2025-10-28 12:03:44.861][3][I] starting event loop...
```

If `json_mode == true`, each line looks like:

```json
{"time":"2025-10-28 12:03:44.861","thread":3,"level":"I","msg":"starting event loop..."}
```

Notes:

* In JSON mode, color is not applied.
* Message text is JSON-escaped (`"`, `\n`, `\t`, etc.).
* One JSON object per line → log aggregators love this.

You can switch this at startup only. It's global.

---

## Queue sizing

`queue_capacity_pow2` controls the MPMC ring buffer size as a power of two.

Examples:

* `queue_capacity_pow2 = 12` → 2^12 = 4096 entries
* `queue_capacity_pow2 = 14` → 2^14 = 16384 entries
* `queue_capacity_pow2 = 16` → 65536 entries

Each entry holds:

* timestamp
* thread id
* level
* message (up to 4096 bytes, UTF-8 safe truncated)

Large queues = more burst tolerance, more memory.

---

## Metrics

If `track_metrics == true`, ULog records internal pressure data:

* how often it had to fall back to a per-thread overflow buffer
* how often that overflow buffer was also full and it had to spin-push

You can fetch them via:

```cpp
auto st = ulog::stats();
ulog::info("ulog stats: overflow_pushes={} backpressure_spins={}",
           st.overflow_pushes,
           st.backpressure_spins);
```

Metrics are purely in-memory counters and do not impact performance meaningfully.