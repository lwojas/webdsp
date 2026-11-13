import { useEffect, useMemo, useRef, useState } from "react";
import type { AudioRuntime } from "../../runtime";
import {
  InstrumentKind,
  MASTER_BUS,
  SUB_BASS_PRESETS,
  SUB_BASS_PROFILES,
  SubBassParam as P,
  harmonicBandLimit,
  harmonicSpectrum,
  harmonicWaveform,
  type InstrumentId,
} from "../../runtime";

interface Props {
  runtime: AudioRuntime;
}

// Each control: its range and how the knob maps (linear or log) and reads out. Ranges are the engine's
// (getInstrumentParams(InstrumentKind.SubBass)); they are repeated here only to lay the knobs out.
type Spec = { id: P; label: string; min: number; max: number; log?: boolean; fmt: (v: number) => string };
const sec = (v: number) => (v < 0.1 ? `${(v * 1000).toFixed(0)} ms` : `${v.toFixed(2)} s`);
const pct = (v: number) => `${Math.round(v * 100)}%`;
const oct = (v: number) => `${v > 0 ? "+" : ""}${v.toFixed(1)} oct`;
const SPEC: Record<string, Spec> = {
  shape: { id: P.Shape, label: "Shape", min: 0, max: 1, fmt: pct },
  tune: { id: P.Tune, label: "Tune", min: -100, max: 100, fmt: (v) => `${v > 0 ? "+" : ""}${Math.round(v)} ct` },
  level: { id: P.Level, label: "Level", min: 0, max: 1, fmt: pct },
  cutoff: { id: P.Cutoff, label: "Cutoff", min: 30, max: 16000, log: true, fmt: (v) => (v >= 1000 ? `${(v / 1000).toFixed(1)} kHz` : `${Math.round(v)} Hz`) },
  resonance: { id: P.Resonance, label: "Reso", min: 0, max: 1, fmt: pct },
  famount: { id: P.FilterAmount, label: "Env amt", min: -5, max: 5, fmt: oct },
  drive: { id: P.Drive, label: "Drive", min: 0, max: 1, fmt: pct },
  pitchAmount: { id: P.PitchAmount, label: "Pitch env", min: -24, max: 24, fmt: (v) => `${v > 0 ? "+" : ""}${v.toFixed(1)} st` },
  pitchDecay: { id: P.PitchDecay, label: "P decay", min: 0.005, max: 0.5, log: true, fmt: sec },
  fa: { id: P.FilterAttack, label: "A", min: 0.001, max: 2, log: true, fmt: sec },
  fd: { id: P.FilterDecay, label: "D", min: 0.01, max: 4, log: true, fmt: sec },
  fs: { id: P.FilterSustain, label: "S", min: 0, max: 1, fmt: pct },
  fr: { id: P.FilterRelease, label: "R", min: 0.01, max: 4, log: true, fmt: sec },
  aa: { id: P.AmpAttack, label: "A", min: 0.001, max: 2, log: true, fmt: sec },
  ad: { id: P.AmpDecay, label: "D", min: 0.01, max: 4, log: true, fmt: sec },
  as: { id: P.AmpSustain, label: "S", min: 0, max: 1, fmt: pct },
  ar: { id: P.AmpRelease, label: "R", min: 0.01, max: 4, log: true, fmt: sec },
};

const DEFAULTS: Record<number, number> = {
  [P.Shape]: 0, [P.Profile]: 0, [P.Octave]: 0, [P.Tune]: 0, [P.Level]: 0.85, [P.Drive]: 0, [P.FilterType]: 0,
  [P.Cutoff]: 12000, [P.Resonance]: 0, [P.FilterAttack]: 0.002, [P.FilterDecay]: 0.3, [P.FilterSustain]: 0.3,
  [P.FilterRelease]: 0.2, [P.FilterAmount]: 0, [P.PitchAmount]: 0, [P.PitchDecay]: 0.08, [P.AmpAttack]: 0.003,
  [P.AmpDecay]: 0.3, [P.AmpSustain]: 1, [P.AmpRelease]: 0.25, [P.Root]: 55,
};
const PRESET_NAMES = Object.keys(SUB_BASS_PRESETS) as (keyof typeof SUB_BASS_PRESETS)[];
const NOTES = [30, 40, 55, 80, 110, 220, 440];
const OCTAVES = [-2, -1, 0, 1];
const SAMPLE_RATE = 48000;

const toPos = (v: number, s: Spec) => (s.log ? Math.log(v / s.min) / Math.log(s.max / s.min) : (v - s.min) / (s.max - s.min));
const fromPos = (p: number, s: Spec) => {
  const c = Math.min(Math.max(p, 0), 1);
  return s.log ? s.min * Math.pow(s.max / s.min, c) : s.min + c * (s.max - s.min);
};

// The Sub Bass as a compact hardware-style voice: OSC / FILTER / ENV / MOD sections of knobs, with the oscillator's
// waveform and harmonics drawn from the same maths the engine uses (src/runtime/subBass.ts). Shape is the big knob.
// An ordinary client of the runtime (createInstrument / setInstrumentParameter / instrumentNoteOn); the DSP is
// native/src/dsp/harmonic_osc.h and native/src/instruments/subbass.h.
export function SubBassPanel({ runtime }: Props) {
  const [v, setV] = useState<Record<number, number>>(DEFAULTS);
  const [preset, setPreset] = useState<string | null>(null);
  const [playing, setPlaying] = useState(false);
  const [semis, setSemis] = useState(0);
  const inst = useRef<InstrumentId | null>(null);
  const values = useRef(v);
  values.current = v;
  const scope = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const id = runtime.createInstrument(InstrumentKind.SubBass, { bus: MASTER_BUS });
    inst.current = id;
    for (const [p, val] of Object.entries(values.current)) runtime.setInstrumentParameter(id, Number(p), val);
    return () => {
      runtime.removeInstrument(id);
      inst.current = null;
    };
  }, [runtime]);

  function set(p: number, val: number) {
    setV((cur) => ({ ...cur, [p]: val }));
    setPreset(null);
    if (inst.current !== null) runtime.setInstrumentParameter(inst.current, p, val);
  }

  function applyPreset(name: keyof typeof SUB_BASS_PRESETS) {
    const next = { ...DEFAULTS, ...SUB_BASS_PRESETS[name] } as Record<number, number>;
    setV(next);
    setPreset(name);
    if (inst.current !== null) for (const [p, val] of Object.entries(next)) runtime.setInstrumentParameter(inst.current, Number(p), val);
  }

  // A held note every 0.9 s so changes are heard on a repeating bass note.
  useEffect(() => {
    if (!playing) return;
    const hit = () => {
      if (inst.current !== null) runtime.instrumentNoteOn(inst.current, { velocity: 0.9, duration: 0.6, pitch: semis });
    };
    hit();
    const timer = window.setInterval(hit, 900);
    return () => window.clearInterval(timer);
  }, [runtime, playing, semis]);

  // The oscillator's waveform and harmonic bars at the current pitch, from the shared harmonic maths.
  const f0 = v[P.Root] * Math.pow(2, v[P.Octave] + v[P.Tune] / 1200 + semis / 12);
  const limit = harmonicBandLimit(f0 / SAMPLE_RATE, 0.42 - 0.12 * v[P.Drive]);
  const wave = useMemo(() => harmonicWaveform(v[P.Profile], v[P.Shape], 256, limit), [v[P.Profile], v[P.Shape], limit]);
  const spec = useMemo(() => harmonicSpectrum(v[P.Profile], v[P.Shape], limit), [v[P.Profile], v[P.Shape], limit]);
  useEffect(() => {
    const c = scope.current;
    if (!c) return;
    const ctx = c.getContext("2d");
    if (!ctx) return;
    const css = getComputedStyle(c);
    const col = (n: string, d: string) => css.getPropertyValue(n).trim() || d;
    const w = c.width, h = c.height, split = Math.floor(w * 0.62);
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = col("--panel", "#1d2026");
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = col("--border", "#2c3038");
    ctx.beginPath();
    ctx.moveTo(0, h / 2);
    ctx.lineTo(split, h / 2);
    ctx.stroke();
    // Two cycles of the waveform, scaled to the largest peak so the shape reads at any Shape.
    const peak = Math.max(...wave.map(Math.abs), 1);
    ctx.strokeStyle = col("--accent", "#4f8cff");
    ctx.lineWidth = 2;
    ctx.beginPath();
    for (let i = 0; i <= wave.length * 2; i++) {
      const x = (i / (wave.length * 2)) * (split - 12) + 6;
      const y = h / 2 - (wave[i % wave.length] / peak) * (h / 2 - 8);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
    // Harmonic bars (dB, 40 dB range), the first 24.
    ctx.fillStyle = col("--accent", "#4f8cff");
    const bars = 24, bx = split + 8, bw = (w - bx - 6) / bars;
    for (let n = 0; n < bars; n++) {
      const db = 20 * Math.log10(Math.max(spec[n], 1e-6));
      const hh = Math.max(0, ((db + 40) / 40) * (h - 12));
      ctx.globalAlpha = spec[n] > 0 ? 1 : 0.15;
      ctx.fillRect(bx + n * bw, h - 4 - hh, Math.max(bw - 1.5, 1), Math.max(hh, 1));
    }
    ctx.globalAlpha = 1;
  }, [wave, spec]);

  const knob = (key: string, big = false) => (
    <Knob key={key} spec={SPEC[key]} value={v[SPEC[key].id]} big={big} onChange={(x) => set(SPEC[key].id, x)} />
  );

  return (
    <section className="panel sb">
      <h2>Sub Bass <span className="sb__sub">harmonic sine bass</span></h2>

      <div className="sb__row">
        {PRESET_NAMES.map((n) => (
          <button key={n} className={preset === n ? "sb__chip sb__chip--on" : "sb__chip"} onClick={() => applyPreset(n)}>{n}</button>
        ))}
        <button className="sb__play" onClick={() => setPlaying((p) => !p)}>{playing ? "Stop" : "Play"}</button>
      </div>

      <canvas ref={scope} width={480} height={110} className="sb__scope" aria-label="Oscillator waveform and harmonics" />

      <div className="sb__group">
        <h3>Osc</h3>
        <div className="sb__knobs">
          {knob("shape", true)}
          <div className="sb__stack">
            <Seg label="Profile" options={SUB_BASS_PROFILES.map((n, i) => ({ label: n, value: i }))} value={v[P.Profile]} onChange={(x) => set(P.Profile, x)} />
            <Seg label="Octave" options={OCTAVES.map((o) => ({ label: o > 0 ? `+${o}` : `${o}`, value: o }))} value={v[P.Octave]} onChange={(x) => set(P.Octave, x)} />
          </div>
          {knob("tune")}
          {knob("level")}
        </div>
        <div className="sb__notes">
          {NOTES.map((hz) => (
            <button key={hz} className={v[P.Root] === hz ? "sb__chip sb__chip--on" : "sb__chip"} onClick={() => set(P.Root, hz)}>{hz} Hz</button>
          ))}
          <label className="sb__semis">+{semis} st<input type="range" min={0} max={24} step={1} value={semis} onChange={(e) => setSemis(Number(e.target.value))} aria-label="Note offset" /></label>
        </div>
      </div>

      <div className="sb__group">
        <h3>Filter</h3>
        <div className="sb__knobs">
          <Seg label="Type" options={[{ label: "LP 24", value: 0 }, { label: "LP 12", value: 1 }]} value={v[P.FilterType]} onChange={(x) => set(P.FilterType, x)} />
          {knob("cutoff")}
          {knob("resonance")}
          {knob("famount")}
        </div>
      </div>

      <div className="sb__group">
        <h3>Env</h3>
        <div className="sb__envs">
          <div><h4>Amp</h4><div className="sb__knobs">{["aa", "ad", "as", "ar"].map((k) => knob(k))}</div></div>
          <div><h4>Filter</h4><div className="sb__knobs">{["fa", "fd", "fs", "fr"].map((k) => knob(k))}</div></div>
        </div>
      </div>

      <div className="sb__group">
        <h3>Mod</h3>
        <div className="sb__knobs">
          {knob("pitchAmount")}
          {knob("pitchDecay")}
          {knob("drive")}
        </div>
        <p className="hint">
          Shape is the oscillator's own harmonics (0 is a genuinely pure sine); Drive is anti-aliased saturation after
          it. The display shows the oscillator before Drive and the filter.
        </p>
      </div>
    </section>
  );
}

// A rotary knob drawn in SVG: drag vertically (or use the arrow keys / wheel); double-click resets.
function Knob({ spec, value, big, onChange }: { spec: Spec; value: number; big?: boolean; onChange: (v: number) => void }) {
  const pos = Math.min(Math.max(toPos(value, spec), 0), 1);
  const drag = useRef<{ y: number; p: number } | null>(null);
  const size = big ? 96 : 56;
  const angle = -135 + pos * 270;
  const r = size / 2 - 6;
  const polar = (a: number, rad: number) => [size / 2 + rad * Math.sin((a * Math.PI) / 180), size / 2 - rad * Math.cos((a * Math.PI) / 180)];
  const [x0, y0] = polar(-135, r);
  const [x1, y1] = polar(angle, r);
  const [tx, ty] = polar(angle, r - 4);
  const [hx, hy] = polar(angle, r - 14 - (big ? 8 : 0));
  const move = (p: number) => onChange(fromPos(p, spec));
  return (
    <div className={big ? "sb__knob sb__knob--big" : "sb__knob"}>
      <svg
        width={size}
        height={size}
        role="slider"
        tabIndex={0}
        aria-label={spec.label}
        aria-valuemin={spec.min}
        aria-valuemax={spec.max}
        aria-valuenow={value}
        aria-valuetext={spec.fmt(value)}
        onPointerDown={(e) => {
          (e.currentTarget as SVGSVGElement).setPointerCapture(e.pointerId);
          drag.current = { y: e.clientY, p: pos };
        }}
        onPointerMove={(e) => {
          if (drag.current) move(drag.current.p + (drag.current.y - e.clientY) / 160);
        }}
        onPointerUp={() => (drag.current = null)}
        onDoubleClick={() => onChange(DEFAULTS[spec.id] ?? value)}
        onWheel={(e) => move(pos - Math.sign(e.deltaY) * 0.02)}
        onKeyDown={(e) => {
          if (e.key === "ArrowUp" || e.key === "ArrowRight") move(pos + 0.02);
          else if (e.key === "ArrowDown" || e.key === "ArrowLeft") move(pos - 0.02);
        }}
      >
        <path d={`M ${x0} ${y0} A ${r} ${r} 0 1 1 ${polar(135, r)[0]} ${polar(135, r)[1]}`} className="sb__track" fill="none" strokeWidth={big ? 5 : 3} strokeLinecap="round" />
        <path d={`M ${x0} ${y0} A ${r} ${r} 0 ${angle + 135 > 180 ? 1 : 0} 1 ${x1} ${y1}`} className="sb__arc" fill="none" strokeWidth={big ? 5 : 3} strokeLinecap="round" />
        <circle cx={size / 2} cy={size / 2} r={r - 6} className="sb__cap" />
        <line x1={hx} y1={hy} x2={tx} y2={ty} className="sb__tick" strokeWidth={big ? 3 : 2} strokeLinecap="round" />
      </svg>
      <span className="sb__label">{spec.label}</span>
      <output className="sb__value">{spec.fmt(value)}</output>
    </div>
  );
}

function Seg(props: { label: string; options: { label: string; value: number }[]; value: number; onChange: (v: number) => void }) {
  return (
    <div className="sb__seg" role="radiogroup" aria-label={props.label}>
      <span className="sb__label">{props.label}</span>
      <div>
        {props.options.map((o) => (
          <button key={o.label} role="radio" aria-checked={props.value === o.value} className={props.value === o.value ? "sb__chip sb__chip--on" : "sb__chip"} onClick={() => props.onChange(o.value)}>
            {o.label}
          </button>
        ))}
      </div>
    </div>
  );
}
