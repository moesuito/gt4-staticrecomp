# M30, thirty-eighth slice — the stack watch captures the open; the completion never runs

Date: 2026-10-02. Inputs: the pinned CORE and ISO. Follow-up to the
thirty-seventh slice (the archive buffers are identical). **No model
behavior changed**: this slice watches the formatter's context directly and
finds where the open's pipeline stops.

## The instrument

A temporary write watch followed a **narrow window around the guest sp**
(the driver publishes the current sp at every module entry and interpreter
step) and reported each *new* (pc, address) pair — deduplicated, because
the idle loops otherwise drown the log. The sound phase's writes then read
like a trace of the open.

## What the trace shows

The formatter's context (the "stream") lives at 0x1FFFDB0 on the sound
thread's stack, and the open runs through it in order:

- **0x004B176C** stores the handler object (0x617AB0) at stream+0x38;
- **0x004B179C** stores the path length at stream+0x48 area;
- **0x004AD330** sets the **stream's state (+0x80) to 0** and
  **0x0057CB00** appends the stream to the handler's pending list (+0x40);
- **0x0044D410** builds the formatter's result context (0x688FC0) at
  0x1FFFE80;
- **0x0044D7C0/0x0044D7C8** run the formatter's own bookkeeping
  (`*(stream+0x74) = 0`, `*(stream+0x4C) = sp+0xB0`);
- **0x004AD690** (the handler's next-stage step) sets the **stream's state
  (+0x80) to 1** and appends the stream to the +0x4C list;
- **0x004B30B8** writes the **path string** `'gt4sys.ins` into the buffer
  at 0x96DAF2 (the bytes `27 ac 67 74 34 73 79 73 2e 69 6e 73 73`);
- then the formatter's cleanup (0x004AF568, 0x0044D460, 0x004AF0A0) runs
  and the formatter reads the result.

**The stream's +0x94 (the result) is never written** — only its initial
clear appears in the trace. So the open advances through stage 1 and the
completion that would set the result never runs.

## The completion function

**0x004AD890 → 0x004AD808** is the function that sets the result:

```
0x004AD808: v1 = *(stream+0xA4)        ; the vtable
            a0 = stream; v0 = *(v1+0x20); jalr v0    ; a virtual method
            v0 = 0x5750C0(stream+0x98, result)
            *(stream+0x94) = v0        ; **the result the formatter reads**
            *(v0+0x3C) = stream
            *(v0+0x38) = 0x004BD018
```

The vtable-0x688C58 handler's entry at **+0x24 is 0x004AD868** (which calls
0x004AD890), so the completion is a *virtual method of the handler* — the
step the worker pipeline invokes when the open finishes. In the model it
never runs.

## What the next slice must find

Which code drains the handler's **+0x4C list** (the stage after the state-1
step) and calls the completion method (vtable+0x24 → 0x004AD868), and why
the model's run stops before it — the candidates are the handler's worker
thread (the cooperative scheduler's ordering) and the pump the formatter
itself uses. The trace also shows the path string being built correctly
(`'gt4sys.ins`), so the request itself is right.

## Verification

- No model behavior changed; the temporary instruments are removed and the
  tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
