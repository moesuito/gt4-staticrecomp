# Slice 95 explained: an interrupt return is not a syscall continuation

A handler may wake a thread more important than the one it interrupted.
We correctly postponed the switch while the handler was running, but forgot
to perform it at the final return when the interrupted thread was still
RUN. The ready thread then waited for an unrelated later scheduling point.
New tests reproduced that omission before the code was corrected.

The subtle part is preserving the interrupted instruction. An ordinary
blocking syscall resumes **after** the syscall, so dispatch normally saves
PC +4. An interrupt instead resumes the instruction it interrupted. Simply
restoring that context and calling the syscall switch helper would skip
the instruction. The correction saves the context directly and makes the
interrupted thread READY before dispatch, avoiding the RUN syscall-save
path. Tests compare every saved register and actually resume that worker.

The entire handler chain must finish first. Equal-priority, lower-priority
and suspended threads must not preempt; both interrupt domains use the
same final-return rule. None of this changes the clock quantum.

Correcting this reveals an earlier timing problem: with the artificial
1 ms clock, the high-priority delay thread now runs promptly, and the main
thread remains READY before later workers are created. That is not progress
toward the menu. An omission that let boot go further was still an omission;
we must fix the time model rather than preserve an accidental scheduling
shortcut. Engine agreement does not establish console fidelity.

The semantic interrupt identity changes from 3 to 4. Old checkpoints cannot
resume as if they were produced under the new return rule; their bytes-on-
wire format is unchanged and they remain useful forensic evidence.

Evidence: `docs/reverse-engineering/slice95-interrupt-return-preemption.md`.
