# Telemetry data flow walkthrough

The [diagram](data-flow.drawio.svg) describes the current implementation in
[`src/server.cpp`](../src/server.cpp), [`include/telemetry/queue.hpp`](../include/telemetry/queue.hpp),
[`gui/dashboard.cpp`](../gui/dashboard.cpp), and [`gui/dashboard.hpp`](../gui/dashboard.hpp).
It is a data-flow view with thread ownership and control annotations, rather than
a strict numbered logical DFD. The source and layout were prepared with AI assistance.

## Follow one reading

1. The Python simulator or the Qt demo writes `device,sequence,value\n` to a real
   loopback TCP socket. These are simulated sensors; the application currently
   binds to `127.0.0.1` and has no physical sensor adapter.
2. The receiver thread uses nonblocking sockets and `select()` to serve up to 32
   clients. Each client has its own `LineFramer`, because TCP does not preserve
   message boundaries. A frame may span reads, and one read may contain several frames.
3. `parse_reading()` validates each completed line. The receiver timestamps valid
   readings and calls `try_push()`. Malformed lines are counted and skipped;
   an oversized frame closes the affected client. The frame limit is 256 bytes
   before the newline, including a CR when present.
4. The bounded FIFO protects its deque, closed flag and high-water mark with a
   mutex. The default capacity is 256. Successful insertion wakes the consumer
   with `notify_one()`. A full or closed queue rejects the new reading; the
   receiver increments `dropped`. This avoids waiting for free queue capacity,
   but insertion still acquires a mutex and can briefly wait for that lock.
5. The processor calls `pop()`, which waits on a condition variable until an item
   is present or the queue closes. It releases the queue lock before processing,
   writes and flushes the CSV row, increments `processed`, then invokes
   `observer.reading()`. A file-write failure ends normal processing and is
   reported as an error; draining is not guaranteed on this error path.
6. The reading callback locks `DashboardState::mutex` and updates the latest
   reading for that device. The display keeps at most 128 device entries;
   readings from additional devices are still written to CSV.
7. A `QTimer` fires every 100 ms on the GUI thread. `Dashboard::refresh()` locks
   the shared state, copies the readings and status, releases the lock, then
   updates the table and counters. Several readings from one device between
   refreshes may appear as one latest-value update. The CSV retains each
   successfully processed reading.

## Status, ownership and stopping

The Qt build has four execution contexts: the GUI thread, a `QThread` running
`ServerWorker::run()` / `run_server()`, the receiver `std::thread`, and the
processor `std::thread`. The CLI runs the same server lifecycle on its main
thread without the Qt display.

The receiver owns client sockets and framing buffers. The processor writes the
capture rows. Atomic counters collect connection, error, drop and processing
counts. The lifecycle thread calls `observer.status()` about every 200 ms and
once at the end; it also calls `observer.listening()` when ready. These callbacks
copy status and port into `DashboardState` under its mutex. Counts are sampled
individually, so a status snapshot is not a transactional view of all activity.

For normal shutdown, the GUI sets the observer's atomic `stop_requested` flag.
The CLI can stop on a signal or configured timeout. The lifecycle sets its
internal `stop` flag, joins the receiver (which closes its clients), closes the
queue and wakes the consumer, then joins the processor after queued readings
have drained. Closing an active dashboard requests this same stop and waits
for completion before closing the window.

## Questions to practice explaining

- **Why a bounded queue?** It caps buffered readings and makes overload explicit.
  Drop-newest favors keeping already queued data, at the cost of losing newer
  readings when overloaded. There is no application-level acknowledgement or retry.
- **Why a condition variable?** The consumer sleeps while the queue is empty.
  The predicate handles spurious wakeups and also lets `close()` wake an idle consumer.
- **Why use a GUI timer?** Widgets stay on the GUI thread, and the display reads
  a bounded latest-value snapshot rather than scheduling a widget update for
  every incoming reading. The snapshot mutex can still briefly delay either thread.
- **What if processing falls behind?** The FIFO fills, new readings are dropped,
  and `dropped` rises. A successful socket send alone does not prove CSV capture.
- **What if the GUI falls behind?** It can skip intermediate displayed values
  while CSV processing continues. The state stores latest readings, not a GUI event queue.
- **Does flush guarantee durability?** No. Flushing the C++ output stream is not
  a crash-proof disk commit.

## Edit in draw.io

Open [app.diagrams.net](https://app.diagrams.net/), choose **Open Existing Diagram**
or **File → Open from → Device**, then select `data-flow.drawio`. Each colored
column is a real swimlane with its related shapes inside it. Edit labels, move
groups, and reroute connectors as needed.

Export with **Export as → SVG**, leave **Include a copy of my diagram** checked,
use a light appearance with an opaque background, and save as
`data-flow.drawio.svg`. Keep the source and exported SVG together. The embedded
diagram also makes the SVG reopenable in draw.io.

For hands-on practice, spend 15 minutes tracing one reading against the code,
20 minutes moving or rebuilding the swimlanes, 15 minutes explaining the queue
and shutdown arrows aloud, and 10 minutes exporting and checking the README.
Only describe draw.io experience you have actually practiced.
