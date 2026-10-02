# M12 branching programs evidence — generated control-flow suites

2026-10-01: BUILD/VERIFY passed. EXPLAIN lesson pending.

## Scope

The M11 generator gains a branching mode: structured control-flow programs
whose expected final state is computed by the same kind of independent Python
model, now extended with control flow and delay slots. The C++ interpreter must
reproduce every expectation — including the exact executed instruction count.

## Shapes (all terminate by construction)

| Shape | Layout | What it exercises |
| --- | --- | --- |
| Countdown loop | `addiu counter,zero,K`; loop: body; decrement; `bne counter,zero,loop`; delay | backward taken branches, a delay slot on every iteration, 64-bit comparisons |
| Conditional skip | setup register; branch over a 1-2 instruction body; delay; body; final | both branch outcomes, likely-branch nullification, link branches |
| Call/return | `j` over an inline function; function body; `jr ra` + delay; main body; `jal` + delay; final | jal link = pc+8 (r31 appears in the expectations and the return lands correctly), jr return after its delay slot, the jump-over-function layout |

Branch types cycle deterministically across the fixture, so all 14 implemented
branch operations appear; the generator refuses to emit a fixture that does
not cover them. Loop bodies and delay slots avoid writing the counter register
through rejection sampling, which keeps termination structural rather than
probabilistic.

## Format addition

Branching programs add one line pair to the M11 format:

```text
steps <n>            (exact executed instruction count, including delay slots)
expect_pc <hex8>     (the exit address the program reaches after n steps)
```

The C++ fixture runner executes exactly `n` steps, requires every step to be
`Executed`, and then compares registers, memory and the pc.

## Real evidence

```powershell
.\build\ee_synth_tests.exe tests/data/synth-branching.txt
# -> 30 synthetic programs passed
```

Both fixtures run under CTest (`ee_synth`, `ee_synth_branching`). The Python
suite regenerates the committed files byte-for-byte and hand-checks the
simulation on taken, not-taken and likely-nullified branches.

## Defects caught during development (recorded)

The first generation attempts failed loudly, each time at the right layer:

1. Plain effects in the Python model did not advance the pc — latent since the
   M11 refactor because straight-line generation never simulated. The fix wraps
   every plain effect in a `plain_step` that advances the pc, keeping transfer
   effects in charge of their own control flow.
2. Two link branches called a non-existent `write_gpr64` on the model; renamed
   to `write_reg64`.
3. The call shape computed its targets assuming it starts at word zero; with a
   preamble present, the `j` landed on the inline function's `jr ra` with
   ra = 0, and execution left the image at pc 0. Targets now derive from the
   builder's actual address, and the simulator reports the offending pc.

None reached a commit; each was reported by the simulator with the address
where the disagreement happened — the same context-carrying-error discipline
as the C++ side.

## Verification

- 12/12 CTest; Python suite 48 collected (42 run, 6 skip without the M3
  reference ELF).
- The branching fixture exercises all 14 branch operations plus jal/jr; both
  fixtures are committed and regenerate deterministically from their seeds.

## Limits

No other-execution yet: the branching fixture uses one code region and no
computed-pointer loads. The model and the interpreter share the same
*specification* even though their implementations are independent, so a
misunderstanding of the architecture itself would fool both — that is what
the later observation milestones (PCSX2 captures) are for. M13, compiling and
running one real GT4 function, is the next landmark.
