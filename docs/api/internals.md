# Internals

This page describes how ULog is built and why it behaves the way it does under load.

---

## High-level design

ULog goals:
- Extremely cheap `ulog::info(...)` calls
- No dynamic allocation in the hot path
- No mutexes around the logging fast path
- No extra thread just for logging
- Predictable rotation and flush timing

---

## Components

### 1. Producers
Any thread or coroutine that calls `ulog::info`, `ulog::error`, etc.

### 2. Global MPMC queue
A lock-free multi-producer / multi-consumer ring buffer that stores `LogEntry`.  
Capacity is `2^queue_capacity_pow2`.

### 3. Per-thread overflow ring
Each producer thread has a tiny ring buffer (TLS).  
If the global queue is briefly full, we spill into this overflow ring instead of losing logs immediately.

### 4. Flush coroutine
A single coroutine (`logger_flush_task()`):
- batch-dequeues logs from the global queue
- formats them
- rotates files if needed
- does the actual `write()` syscalls

There is exactly one flush coroutine.  
No dedicated logging thread.

---

## Flow

```text
[Producer Thread/Coroutine] --pushf()--> [MPMCQueue<LogEntry>]
                                      \
                                       \ (if full)
                                        -> [Thread Overflow Ring]

[logger_flush_task()]
  -> dequeue bulk
  -> group by level
  -> maybe rotate file
  -> write batch to sink
```

---

## Rotation model

Rotation is size-based and per-level:

* Each level (TRACE, DEBUG, INFO, WARN, ERROR) has its own "sink".
* A sink may be stdout or a file.
* If it's a file and `max_file_size_bytes` is set, the flush coroutine checks before every batch whether adding that batch would exceed the limit.
* If yes:

    1. `fsync()` and close current file
    2. rename `file.log` → `file.log.1`, shift `file.log.1` → `file.log.2`, etc.
    3. open a fresh new `file.log`
    4. write the batch there

Producer code never does any of that.

Because rotation happens at batch boundaries, ULog guarantees:

* A single logical log batch is never split across rotated files.
* Files won't be mid-rename while something else is writing.

---

## Output modes

### Text mode

Example:

```text
[2025-10-28 12:03:44.861][3][W] slow request latency_ms=211.5
```

* Millisecond-resolution local timestamp
* Thread ID
* Level marker `[W]`
* Original message
* Optional ANSI color (only if sink is a TTY and `enable_color_stdout=true`)

### JSON mode

Example:

```json
{"time":"2025-10-28 12:03:44.861","thread":3,"level":"W","msg":"slow request latency_ms=211.5"}
```

* One JSON object per line
* Escaped quotes / newlines / tabs
* No ANSI color
* Easier to ingest into Loki / ELK / Vector / etc.

Mode is chosen once at startup via `ULogInit.json_mode`.

---

## Thread IDs

ULog prints a "thread" field in each record.

* For `uvent` worker threads, we use the internal worker index (cheap, stable).
* For any non-uvent thread, we derive a stable pseudo-ID from TLS address bits.

No syscalls like `gettid()`. No string formatting of thread names. It's always an integer.

---

## Metrics

If `track_metrics = true`:

* `overflow_pushes`: incremented when the global MPMC queue was full and we had to stash the log in the per-thread overflow ring.
* `backpressure_spins`: incremented when both the queue and the overflow ring were full, and the producer had to spin to force-enqueue.

You can read these stats at runtime:

```cpp
auto st = ulog::stats();
ulog::info("ulog overflow_pushes={} backpressure_spins={}",
           st.overflow_pushes,
           st.backpressure_spins);
```

This lets you detect that logging is starting to choke before you start losing data or stalling latency.

---

## Summary

* Producers never block on disk I/O.
* Disk I/O (and log rotation) only happens in one coroutine, under your scheduler.
* Each flush is batched per log level to minimize syscalls.
* Rotation is atomic at batch granularity.
* JSON mode can be turned on globally for structured logs.
* Metrics are optional and cheap.

ULog is basically: *"spdlog async logger, but zero extra thread, and tuned for coroutine runtime."*