# Learn and explain the monitor

This project was built with AI assistance. Use it as a working starting point: run it, trace the code, and make your own changes before describing personal proficiency with its design.

## 1. Follow a single reading

Run the server and send one reading using `client/simulator.py --count 1`. Locate the record in the CSV. Trace it through `recv`, `LineFramer::feed`, `parse_reading`, `BoundedQueue::try_push`, `pop`, and the processor's log write.

Explain why successful `sendall` only confirms that the client handed bytes to its TCP stack, and why the application still needs a CSV/counter check.

## 2. Understand message boundaries

Run the simulator with `--fragment`. Read the split/coalesced framing test. Explain why one `recv` is not guaranteed to equal one reading, and why each client needs its own buffer.

Exercise: add a test where the CR and LF arrive in different chunks. Confirm that a complete record is still accepted.

## 3. Explain synchronization

Identify the data owned solely by the receiver, solely by the processor, and shared by threads. Explain the queue's mutex and the predicate in its condition-variable wait.

Exercise: change the queue capacity from 256 to 2, enable a 20 ms processing delay, and send 200 records with no interval. Compare `processed`, `dropped` and `queue_high_water`. Describe why the chosen overload policy loses new records instead of freezing the receiver.

## 4. Trace shutdown

Stop the server while a client is connected. Follow the sequence: stop receiving, join receiver, close queue, drain processor, join processor, report final counters.

Explain why closing the queue before the receiver finishes would reject readings that the receiver might still attempt to enqueue.

## 5. Make a measurable improvement

Choose one: add duplicate-sequence detection, record queue residence time with a monotonic clock, or build a Qt dashboard. Write a test for the behavior before changing the code. Record actual observations and limitations in your README.

Keep the Python input described as simulated until a real hardware connection is implemented and tested. Keep your team's Thumby work separate unless the team agrees on an integration.

## Interview prompts

- Why is a bounded queue useful, and what alternative overload policies could you choose?
- What does a mutex protect here, and what do the atomics protect?
- How are malformed, oversized and unfinished messages handled differently?
- Which guarantees come from TCP, and which delivery guarantees are missing at the application level?
- How would you add an actual device without changing the server's wire protocol?
