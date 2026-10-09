# Slice 94 explained: separate contributions before choosing a correction

Slice 93 showed that a slower artificial clock changed worker scheduling.
It did not tell us which clock charges were wrong. Slice 94 separates four
kinds of work before execution: normal calls, interrupt-handler calls,
patched-call returns and interrupt returns. Returns pop their state, so
classifying afterward would mislabel the very event we need to measure.

Handler/return charges represent 23.26% of the baseline's 10k service-time
advance. We tried removing those charges while keeping normal calls at
1 ms. It reduced the surplus signals, but the semaphore still received
233 signals for only 97 waits in the steady window. The workers still did
not run. A contributing cause is not necessarily a sufficient explanation,
and removing it is not automatically a complete correction.

The clock also counts frequent GetThreadId queries as whole milliseconds.
That shortcut needs its own evidence; a fabricated duration that happens
to advance boot is not an answer. Shared-engine agreement checks engine
consistency, not console fidelity.

Meanwhile, a separate audit found a different contract defect: a handler
can wake a higher-priority thread, but the final return does not schedule
it immediately. That can be tested directly without inventing a new clock.
When correcting it, save the interrupted PC exactly: a syscall switch
normally advances by four, but an interrupt did not consume that next
instruction. The two problems must remain separate experiments.

Evidence: `docs/reverse-engineering/slice94-clock-charge-attribution.md`.
