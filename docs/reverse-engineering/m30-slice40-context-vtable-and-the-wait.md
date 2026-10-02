# M30, fortieth slice — the context's virtual table and the wait for state 3

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the thirty-ninth
slice (the pump's state machine). **No model behavior changed**: this slice
maps the context class's methods and finds the wait the open depends on.

## The context class (vtable 0x00688ED0)

| entry | method | what it is |
| --- | --- | --- |
| +0x08 | 0x004AF0A0 | the destructor (releases the context) |
| +0x10 | **0x004AF1D8** | the *process* step: with the state at 0 and a handler bound (+0x38) it calls 0x004AD648 (the next-stage step) |
| +0x18 | **0x004AF268** | the *state machine*: state 0 → 0x004AD368, state 1 → the check + 0x004AD438, state 2 → the check + 0x004AD5E0 |
| +0x20 | **0x004AF3A0** | the *wait*: while the stream's state (+0x80) is not 3 it blocks (0x005767E0) |
| +0x30 | 0x004AF108 | |
| +0x38 | 0x004AF448 | a callback dispatcher |
| +0x40 | 0x004AF478 | a callback dispatcher |

## The formatter's flow

The formatter's format step **0x004AF3E8** calls the context's **+0x10**
(the process) and then its **+0x20** (the wait): the open is asynchronous —
the formatter **blocks until the stream's state reaches 3**, and only then
reads the result at +0x94. So the completion depends on whatever advances
the state beyond 1:

- the once-only check **0x004AF520** is called not only from the pump
  (0x004AF2F8/0x004AF320) but also from the handler's own steps
  **0x004AD9F4** and **0x004ADBD4** — the worker-side processing that
  advances the state toward 3.

## Where the model stops

The slice-38 trace shows the state reaching **1** (the process's next-stage
step 0x004AD648, which appends the stream to the handler's +0x4C list) and
never reaching 2 or 3 — so the wait would block, and the handler-side steps
(0x004AD9F4/0x004ADBD4) that advance it never run. Those functions are the
worker pipeline the next slice must find: which code calls them, on which
thread, and why the model's run stops before them.

## Verification

- No model behavior changed; the tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
