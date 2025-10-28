# Logger API

`Logger` is the internal engine. You normally won't call it directly — you'll use the `ulog::*` free functions. But its behavior defines the guarantees.

---

## Levels

```cpp
enum class Level : uint8_t {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4
};
````

Public wrappers:

```cpp
ulog::trace(fmt, ...);
ulog::debug(fmt, ...);
ulog::info(fmt, ...);
ulog::warn(fmt, ...);
ulog::error(fmt, ...);
```

These all internally call `Logger::pushf(Level, fmt, ...)`.

---

## Message formatting

ULog uses `{}`-style formatting, similar to spdlog / fmtlib-style:

```cpp
ulog::info("user {} logged in from {}", user_id, ip);
ulog::warn("slow request {}, latency_ms={}", url, latency_ms);
ulog::error("db error code={} msg={}", code, err_msg);
```

Formatting is done without heap allocations on the hot path:

* We build the formatted string into a local `std::string`.
* We UTF-8 truncate it to fit the fixed buffer in `LogEntry::msg[4096]`.
* The `LogEntry` struct is then pushed into a lock-free MPMC ring buffer.

---

## No blocking on write()

Calls like `ulog::info(...)` do **not** call `write()`, `fsync()`, `open()`, or `rename()`.
They never touch disk directly.

Instead they:

1. Grab a timestamp
2. Capture thread id
3. Push `LogEntry` into a wait-free MPMC queue
4. Return

If the queue is temporarily full:

* We spill into a small per-thread ring buffer (overflow buffer).
* If even that is full, we spin very briefly to force the push.
  (We do not drop logs by default.)

If you enabled metrics (`track_metrics = true`), those pressure events are counted.

---

## Flush & rotation

Disk I/O is only done in the flush coroutine, not in producers.

`Logger::flush_once_batch()`:

1. Dequeues up to `batch_size` log entries from the MPMC queue in bulk.
2. Groups them by level into per-level staging buffers.
3. For each level:

    * Checks rotation threshold (`max_file_size_bytes`).
    * If rotation is needed:

        * `fsync()`s and closes the old file.
        * Renames `file.log` → `file.log.1`, shifts `.1` to `.2`, etc.
        * Opens a fresh new `file.log`.
    * Writes the entire batch to that file (or stdout).
4. Updates byte counters for rotation.

So rotation is **atomic at batch boundaries**:

* a given flush batch is either entirely pre-rotation or entirely post-rotation
* never half-in-one-file, half-in-another

---

## Output format

### Text mode

ULog writes lines like:

```text
[2025-10-28 12:03:44.861][3][I] message text here...
```

* Timestamp has ms resolution.
* `[3]` is the logical thread id:

    * For uvent workers: `this_thread::detail::t_id`
    * For non-uvent threads: a stable pseudo-id
* `[I]` is one-letter level

If the sink is a TTY and `enable_color_stdout = true`, the line will be wrapped in level-specific ANSI color.

### JSON mode

If `json_mode = true`, ULog writes one JSON object per line:

```json
{"time":"2025-10-28 12:03:44.861","thread":3,"level":"I","msg":"message text here..."}
```

* Newlines, quotes, backslashes in the message are escaped.
* No ANSI colors are emitted in JSON mode.
* This mode is friendly to log shippers / structured ingestion.

You choose mode globally at init.

---

## Stats

If `track_metrics = true`:

```cpp
struct Stats {
    uint64_t overflow_pushes;     // queue was full, used TLS overflow
    uint64_t backpressure_spins;  // queue+overflow full, producer had to spin-push
};

Stats ulog::stats();
```

You can log or export these stats for observability.