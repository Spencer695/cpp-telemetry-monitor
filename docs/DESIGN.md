# Design decisions

## Thread ownership

One receiver thread multiplexes up to 32 nonblocking TCP clients with `select`. One processor thread consumes validated readings and writes the CSV. The main thread reports counters and requests shutdown. This avoids creating an unbounded thread for each client and gives each socket one owner.

`select` was chosen for a small, inspectable cross-platform implementation using POSIX sockets and Winsock. This is not a claim of high-scale server performance. The Linux implementation rejects file descriptors outside `FD_SETSIZE`; the Windows client cap is below its default descriptor-set capacity.

## TCP framing

TCP is an ordered byte stream, not a sequence of application messages. Each connection retains its own unfinished line. A receive can complete an earlier line, contain several lines, or end midway through another. A 256-byte limit bounds per-client unfinished storage. Completed lines before an oversized line are still validated; the oversized connection then closes.

The protocol is deliberately narrow: three comma-separated ASCII fields with no CSV quoting. Validation happens before enqueueing. Restricting device IDs also prevents commas/newlines from corrupting CSV output. Floating-point NaN, infinity, range overflow and trailing junk are rejected.

## Queue and overload

The queue has a fixed capacity and a mutex protecting its deque, closed flag and high-water mark. `pop` waits with a predicate so notifications and spurious wakeups cannot make an empty read valid. Closing wakes waiters, rejects new writes and allows existing entries to drain.

The receiver does not block on a full queue: it drops the newest validated reading and increments `dropped`. This keeps other clients responsive, at the cost of loss during overload. There is no wire acknowledgement. After a successful shutdown, each valid complete reading handed to the queue is accounted for by `processed + dropped`; malformed and unfinished messages have separate counters.

## Shutdown and failures

On Ctrl+C, SIGTERM (Linux), Ctrl+Break (Windows), or runtime expiry, the main thread sets the atomic stop flag and joins the receiver. Socket readiness waits are bounded to 100 ms. The receiver closes its clients and counts unfinished frames. Main then closes the queue and joins the processor after accepted work drains. Artificial processing delay can intentionally extend drain time.

An exception in either worker is captured for main to report as a nonzero exit. A processing failure also closes the queue and requests stop. CSV stream errors are enabled instead of silently discarding failed writes. Existing readable capture files are refused; the initial existence check is intended for ordinary single-process demos, not adversarial concurrent writers to the same path.

## Qt dashboard

The optional Qt Widgets executable embeds the server library. A `QThread` owns the blocking server lifecycle; the backend still uses its original receiving and processing threads. A shared `ServerObserver` exposes an atomic stop request and callbacks for listening port, logged readings and status snapshots.

Callbacks never touch widgets. They write to a mutex-protected dashboard cache. The GUI's 100 ms timer copies a bounded snapshot and updates labels/table cells on the GUI thread. Latest values replace earlier values for a device, rather than enqueueing an unbounded GUI event for every reading. The cache is capped at 128 device IDs; the CSV still records every successfully processed reading.

Start creates a fresh worker and capture. Stop sets the atomic request and leaves the UI event loop running while the backend drains. Close requests the same stop and defers the actual close until the thread finishes. Errors return through Qt signals and restore editable controls. The built-in demo uses asynchronous `QTcpSocket` writes from a GUI timer; it transmits synthetic data through the actual TCP server.

GUI tests exercise both behavior and a timer heartbeat during data ingestion. References: [Qt thread ownership](https://doc.qt.io/qt-6/threads-qobject.html), [Qt Widgets](https://doc.qt.io/qt-6/qtwidgets-index.html) and [QTcpSocket](https://doc.qt.io/qt-6/qtcpsocket.html).

## Tradeoffs and limits

- Per-record flushing favors a readable live demo over throughput. No throughput numbers are claimed.
- Atomic counters can show slightly different instants during live status output; final output occurs after both joins.
- Timestamp is the server's wall-clock receive time, not a device timestamp. It can move if the system clock changes.
- Sequence values are logged but not deduplicated. Ordering between clients depends on readiness scheduling.
- A ten-second idle timeout and 32-client cap bound idle resource use.
- The simulator is synthetic. Hardware integration requires a real device adapter and a separately tested interface.

References: [Linux TCP documentation](https://man7.org/linux/man-pages/man7/tcp.7.html), [Linux select documentation](https://man7.org/linux/man-pages/man2/select.2.html), and the [C++ working draft condition-variable requirements](https://eel.is/c++draft/thread.condition.condvar).
