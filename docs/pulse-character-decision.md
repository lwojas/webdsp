# Pulse character — product decision record

Status: **Pulse has since been adjusted** (commit `e99c57a`: pluck, grit, independent pumper); this
record is kept as the design framing, not as a pending task. It is a sound-design/product question
about what Pulse should *be*. It is deliberately independent of the dual-filter work
([`dual-filter-proposal.md`](./dual-filter-proposal.md)): that proposal neither requires nor
implies any change to Pulse, and Pulse's direction should not be decided by it.

## The problem

Pulse is described in use as "everything becoming buzzy/resonant". Shipped Pulse is "Duck &
Breathe" ([`pulse-instrument-proposal.md`](./pulse-instrument-proposal.md),
[`pulse-instrument.md`](./pulse-instrument.md)): 3 detuned saws plus sub, pre-filter drive, a 24 dB
resonant low-pass opened by a beat-locked repeat contour, and a transport-locked pump.

What exists now (latest commit, `e99c57a`): independent pumper (`pumpRate`, `pumpAttack`), a
per-note **pluck** (filter snap up to +4 oct, ~10 ms pitch click, amp decay to a body) and **grit**
(scales pre-filter drive; 0.5 = original). Low grit with pluck is the clean/soft pluck; high grit
is the buzz.

## The options (a framing, not a recommendation)

| | Identity | Shape |
|---|---|---|
| **Keep Duck & Breathe** | Rhythmic sidechain-style lead/bass | As shipped; use `grit` and `pluck` to steer |
| **Pulse / Pluck** | Short, gentle, plucked | Pulse oscillator -> small pitch transient -> gentle low-pass -> short amp envelope -> optional resonator |
| **Resonant VA** | Classic resonant analogue | 2 oscillators -> resonant filter(s) -> filter modulation -> longer envelope |

Notes:

- The Pulse/Pluck shape is already partly present as the `pluck` and `grit` controls; the
  question is whether that should be the *default identity* rather than an option.
- "Resonant VA" overlaps heavily with Bass, PwmBeast and PercSynth (a resonant low-pass on
  oscillators). Moving Pulse there would add little new territory.
- An optional resonator stage is a new DSP block (comb/modal), not in the repo today, and would
  need its own scoping.

## What is needed to decide

- Listening: render Pulse at default, high `pluck` + low `grit`, and high `grit` to judge whether
  the buzz is a default-patch problem or an inherent-architecture one.
- Whether the rhythmic pump/repeat (the thing that makes Pulse distinct) should stay the point of
  the machine regardless of its tone.

No further Pulse change is implied by this record.
