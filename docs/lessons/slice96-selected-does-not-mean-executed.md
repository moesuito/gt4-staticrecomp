# EXPLAIN: selected does not mean executed

The thread table showed the update loop running and the boot thread ready.
It was tempting to assume that a delay semaphore already contained a signal
and the update loop never really blocked. Live observation disproved that:
all 270 measured delays found the semaphore empty and selected the boot
thread after blocking.

Why did boot still not advance? The model charges one millisecond for each
service. The game had just requested a one-millisecond wait. Executing
WaitSema selects boot, but charging that service then advances the timer
by the entire remaining delay. Before boot executes anything, an interrupt
signals the semaphore and the higher-priority update loop resumes.

Thus **being selected 270 times does not mean executing 270 times**. Record
the restored PC and the interrupt entry, and inspect where execution happens
in the driver. Here the interrupt starts at the same PC before any guest
instruction. Sampling only the final thread table hides the whole sequence.

Two other traps mattered:

- The diagnostic total starts before TIM2 is enabled. Comparing that total
  directly against the game's timer base mixes epochs. Using TIM2 COUNT,
  MODE and the library's overflow counter showed a full millisecond left,
  not an already-expired timer.
- Removing handler/return charges did not help because the decisive charge
  belonged to an ordinary blocking service. A clean engine differential
  means both implementations reproduce that shortcut, not that a PS2 does.

Keep the verified priority rule. Replacing a bad clock with a guessed
smaller number merely hides the mechanism. Next obtain independent relative
timing and define guest-work accounting shared by both execution engines.
Details and limits: `../reverse-engineering/slice96-update-loop-and-delay-balance.md`.
