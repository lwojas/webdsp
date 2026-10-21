# External references

Pointers to third-party material worth knowing about. Not techniques (see [`techniques.md`](./techniques.md)),
and nothing here is to be vendored or ported.

## The Usual Suspects: Gearmulator technical docs

Source: <https://theusualsuspects.io/technical/> (six pages, summarised below, fetched 2026-10-21).

**Relevance to webdsp: low for DSP technique, useful for context.** Gearmulator emulates the
original hardware instruction by instruction (Motorola DSP56300 family, plus 68000 / H8S MCUs)
and runs the unmodified firmware. The pages document the chip, memory and bus plumbing, not the
oscillator, filter or effect algorithms. They tell us *what* the classic synths are built from,
not *how* the audio math works, so there is nothing to port. Treat as background for the
design lineage of our Hypersaw, FM and wavetable-adjacent kinds.

| Page | URL | Useful facts |
|---|---|---|
| How the Emulator Works | `/technical/how-it-works` | Bit-exact firmware execution; UI is a MIDI/SysEx controller decoupled from the engine; emulator-only SysEx extensions (LCD, LEDs, LFO phase). No DSP technique content. |
| Osirus (Access Virus A/B/C/TI) | `/technical/osirus-c` | Subtractive + FM; wavetable and **Hypersaw** oscillators added from the TI. Twin multimode resonant filter, series / parallel / split, 12 to 36 dB/oct. A/B/C run at 46,875 Hz (12 MHz / 256), TI at 32 to 96 kHz. Hardware latency ~324 samples (A/B/C), ~216 (TI). Algorithms themselves not documented. |
| Xenia (Waldorf Microwave II/XT) | `/technical/xenia` | **40 kHz** sample rate (audio ceiling ~20 kHz, steep anti-alias), 24-bit fixed point. Wavetable data: 128 samples x 8-bit signed per wave, 506 factory + 10 user waves, 32 + 32 tables of 64 wave indices. 10 voices per DSP56303, up to 30. |
| Vavra (Waldorf microQ) | `/technical/vavra` | DSP56362 at 101.6 MHz, 44.1 kHz, 2,304 cycles per frame, 25 voices per DSP (up to 75 with three). Mostly ESAI / DMA sync detail. |
| NodalRed2x (Nord Lead 2X) | `/technical/nodalred2x` | Two DSP56362 at **98.2 kHz** sample rate, each owning a fixed half of the voices. No oscillator/filter detail. |
| JE-8086 (Roland JP-8000/8080) | `/technical/je8086` | ESP ASICs, not DSPs. Origin of the **Supersaw** (seven detuned saws), which our Hypersaw kind follows. Page says "more details to follow"; a 39C3 (2025) talk may go deeper. |

### Takeaways

- Hypersaw lineage: Virus Hypersaw and JP-8000 Supersaw are the same idea (7 detuned saws); no
  published detune/mix curves here, so keep tuning ours by ear.
- Wavetable sizing precedent: Microwave used tiny tables (128 x 8-bit) and a 40 kHz rate; small
  tables are musically viable if we ever add a wavetable kind.
- Revisit the JE-8086 page when its details land, and look for the 39C3 talk.

## Idea: dual-filter routing

The Virus line's "twin multimode resonant filter, series / parallel / split, 12 to 36 dB/oct" is
the only filter detail the site gives (no topology maths), so it is inspiration only. Our own
design and evaluation: [`../dual-filter-proposal.md`](../dual-filter-proposal.md). The separate
question of Pulse's character is in [`../pulse-character-decision.md`](../pulse-character-decision.md).
