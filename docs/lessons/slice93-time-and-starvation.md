# Slice 93 explained: a fake clock can starve real guest work

The device thread has a perfectly real wait in the game's code. Why did it
never wait in our model? A counting semaphore only blocks when its counter
is zero. With 254 credits available, the next wait consumes one and carries
on immediately. Higher-priority runnable work excludes lower-priority
workers under the ordinary scheduling rule; no special scheduler failure
is required.

Our old clock assigns one millisecond to **every handled service**. That
includes quick GetThreadId calls, handler work and the model's own return
trampolines. A frame is generated every 16 2/3 services. The polling round
uses roughly 54 services: the model generates about three frame signals
while it consumes one. Measured between 5k and 10k: 300 signals, 92 waits.
The counter climbs, never empties, and the device thread cannot yield via
its intended wait. These are model-time ratios, not hardware performance
measurements.

We changed only the service quantum to roughly 100 us for an experiment.
The wait reached zero, thread 4 blocked and thread 5 got the CPU; all twelve
workers executed. That is a useful causal test, but **100 us is not a
discovered truth**. It changes early boot ordering too; later the main
thread runs at priority 0 and another bottleneck appears. Choosing a magic
number because boot goes further would hide the model error rather than
explain it. The experiment was removed from production.

Two important evidence lessons:

1. A saved PC is not execution history. Threads 9/13 really did run and
   sleep, despite the earlier blanket statement that all workers never ran.
   Direct dispatch logging is stronger than inferring history from a stop.
2. A syscall trace entry is not automatically an idle event. Service 0x100
   at 0x1604 is our deferred-return trampoline. Counting those lines as
   idle made a plausible but false story about frame generation.

Both execution engines agreed under the diagnostic clock. That proves
they share the same behavior, not that the shared behavior matches a PS2.
The next step is to separate guest work from artificial return overhead,
measure the reference and define a defensible time policy with regression
tests and a new compatibility identity if adopted.

See `docs/reverse-engineering/slice93-scheduler-and-device-semaphore.md`
for exact addresses, hashes, experiments and unresolved questions.
