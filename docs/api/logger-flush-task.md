# logger_flush_task()

```cpp
uvent::task::Awaitable<void> logger_flush_task();
```

`logger_flush_task()` is the only piece of code that actually touches I/O.

You must launch exactly one instance of it, usually right after `ulog::init()`:

```cpp
ulog::init(cfg);
usub::uvent::system::co_spawn(ulog::logger_flush_task());

// ... then start uvent loop
usub::Uvent uvent(/*workers=*/4);
uvent.run();
```

---

## What it does

Inside its loop:

1. Calls `Logger::flush_once_batch()`

    * Pulls up to `batch_size` log records from the global MPMC queue.
    * Groups them by level.
    * Optionally rotates the destination file (if size limit exceeded).
    * Writes the batch to disk/stdout in as few `write()` syscalls as possible.

2. `co_await sleep_for(flush_interval_ns)`

    * Gives control back to the uvent scheduler.
    * No busy-waiting.

---

## Rotation happens *here*

Log rotation is done **only inside this coroutine**, right before writing each per-level batch:

* Producers never rotate.
* Producers never close FDs.
* Producers never rename files.

This means:

* No races between logging threads and I/O.
* No partial rename while someone else is writing.
* Either the full batch lands in the "old" file, or the entire batch lands in the "new" file after rotation.

---

## Shutdown

When you call `ulog::shutdown()`:

* The logger sets an internal shutdown flag.
* The flush logic drains the queue completely.
* After the queue is empty, all sinks are `fsync()`'d and closed (or flushed if stdout).
* After that point, logging calls are no longer expected to be used.

You should call `ulog::shutdown()` after the `uvent` loop exits.