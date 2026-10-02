# M30, thirty-ninth slice — the pump's state machine and the missing state 2

Date: 2026-10-02. Inputs: the pinned CORE and the slice-38 trace. Follow-up
to the thirty-eighth slice (the completion never runs). **No model behavior
changed**: this slice maps the state machine around the open and names the
step the model never reaches.

## The pump is a state machine

**0x004AF268** drives the stream's state (its +0x80):

| state | step | notes |
| --- | --- | --- |
| 0 | 0x004AD368 | then `*(stream+0x84) = 1` and a vtable+0x40 call |
| 1 | the check **0x004AF520** then **0x004AD438** | 0x004AD438 signals the condition (0x00574EE8 on the handler+0x64) and sets +0x80 = 1 |
| 2 | the check then **0x004AD5E0** | |

The check **0x004AF520** is a once-only gate: it reads the stream's +0x98,
sets it to 1 and returns true only the first time — so each stage runs
exactly once.

## The completion the result needs

The function that sets the **stream's +0x94** (the result the formatter
reads) is **0x004AD890 → 0x004AD808** — `*(stream+0x94) = the vtable+0x20
method's result` (wrapped by 0x005750C0). The vtable+0x20 entry of the
handler class (vtable 0x688C58 +0x24) is **0x004AD868**, and its only
direct caller is 0x004AD89C (inside the family); no code calls it directly
otherwise — it is a *virtual method of the handler*, invoked by the
pipeline when the open finishes.

## Where the model stops

The slice-38 trace shows the open reaching **state 1** (the step 0x004AD690
inside 0x004AD648 sets +0x80 = 1 and appends the stream to the handler's
+0x4C list) and then the formatter's cleanup; the **state never reaches 2**,
so the state-2 step (0x004AD5E0) and the completion (0x004AD868 →
0x004AD808 → the stream's +0x94) never run. The **+0x4C list** is the
stage the worker pipeline drains to advance to state 2.

## What the next slice must find

Which code drains the handler's **+0x4C list** and advances the state to 2
— the candidates are the handler's own worker thread (created by the engine
at boot) and the pump's callers — and why the model's run stops before it:
whether the worker thread is never scheduled (the cooperative scheduler's
ordering) or waits on a condition the model does not signal.

## Verification

- No model behavior changed; the tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
