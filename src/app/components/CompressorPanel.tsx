import { useEffect, useRef, useState } from "react";
import type { AudioRuntime } from "../../runtime";
import {
  COMPRESSOR_PRESETS,
  DEFAULT_DRUM_PEAK_DB,
  EffectKind,
  GainReductionHistory,
  InstrumentKind,
  LoudnessChange,
  MASTER_BUS,
  NodeParam,
  compressorCurveDb,
  compressorMeter,
  peakToDb,
  presetSettings,
  reductionCharacter,
  staticGainReductionDb,
  type CompressorSettings,
  type EffectId,
  type InstrumentId,
} from "../../runtime";

interface Props {
  runtime: AudioRuntime;
}

// A two-bar drum loop: 16 steps, 120 bpm. Kick, snare, hats, clap and tom, so the compressor sees a whole kit,
// including low end, rather than a single hit.
const STEP_MS = 125;
const PATTERN: { kind: InstrumentKind; steps: [number, number][] }[] = [
  { kind: InstrumentKind.Kick, steps: [[0, 1], [4, 0.9], [8, 1], [10, 0.7], [12, 0.9], [14, 0.8]] },
  { kind: InstrumentKind.Snare, steps: [[4, 0.95], [12, 1], [15, 0.45]] },
  { kind: InstrumentKind.HiHat, steps: [[0, 0.5], [2, 0.7], [4, 0.5], [6, 0.7], [8, 0.5], [10, 0.7], [12, 0.5], [14, 0.85]] },
  { kind: InstrumentKind.Clap, steps: [[12, 0.6]] },
  { kind: InstrumentKind.Tom, steps: [[7, 0.7], [11, 0.6]] },
];

// Control ranges. The engine accepts more (attack 0.1 ms..1 s, release 1 ms..3 s); these are the useful part for a bus.
const RATIO_MAX = 20;
const ATTACK = [0.0001, 0.1];
const RELEASE = [0.01, 1];
const logPos = (v: number, lo: number, hi: number) => Math.log(v / lo) / Math.log(hi / lo);
const fromLogPos = (p: number, lo: number, hi: number) => lo * Math.pow(hi / lo, p);

const fmtMs = (s: number) => (s >= 0.1 ? `${Math.round(s * 1000)} ms` : s >= 0.01 ? `${(s * 1000).toFixed(0)} ms` : `${(s * 1000).toFixed(1)} ms`);
const fmtDb = (db: number) => `${db > 0 ? "+" : db < 0 ? "−" : ""}${Math.abs(db).toFixed(1)} dB`;

const DB_MIN = -48;
const GR_SCALE = 18; // the meter shows 0 .. -18 dB of reduction

const INITIAL: CompressorSettings = presetSettings(COMPRESSOR_PRESETS[0]);

function colors(el: HTMLElement) {
  const css = getComputedStyle(el);
  const v = (n: string, d: string) => css.getPropertyValue(n).trim() || d;
  return { fg: v("--fg", "#e4e6eb"), muted: v("--muted", "#8a8f98"), border: v("--border", "#2c3038"), accent: v("--accent", "#4f8cff"), gr: v("--gr", "#f0a04b"), bg: v("--panel", "#1d2026") };
}

// The compressor as a drum-bus tool: a transfer-curve and gain-reduction display over the six controls, with four
// musical starting points. An ordinary client of the runtime; the DSP is native/src/dsp/compressor.h and the
// numbers on the display come from its meter (MeterSnapshot.dynamics), not from guessing at the audio.
export function CompressorPanel({ runtime }: Props) {
  const [s, setS] = useState<CompressorSettings>(INITIAL);
  const [preset, setPreset] = useState<number | null>(0);
  const [playing, setPlaying] = useState(false);
  const [dry, setDry] = useState(false);
  const [readout, setReadout] = useState({ inDb: -120, grDb: 0, outDb: -120 });

  const effect = useRef<EffectId | null>(null);
  const settings = useRef(s);
  settings.current = s;
  const dryTrim = useRef(0); // dB the dry side is raised/lowered by, frozen when A/B flips
  const loudness = useRef(new LoudnessChange());
  const dryRef = useRef(dry);
  dryRef.current = dry;

  // Meter accumulators written by the engine callback, drained once per animation frame.
  const acc = useRef({ inPk: 0, outPk: 0, gr: 0 });
  const live = useRef({ inDb: -120, outDb: -120, gr: 0, peakWindow: [] as number[] });
  const history = useRef(new GainReductionHistory(180));
  const curveCanvas = useRef<HTMLCanvasElement>(null);
  const meterCanvas = useRef<HTMLCanvasElement>(null);

  function push(next: CompressorSettings, bypass: boolean) {
    const id = effect.current;
    if (id === null) return;
    const set = (p: NodeParam, v: number) => runtime.setEffectParameter(MASTER_BUS, id, p, v);
    // Dry keeps the effect running at 1:1 (exactly no reduction) with the loudness change applied as plain gain,
    // so A/B compares character, not volume.
    set(NodeParam.CompThreshold, next.threshold);
    set(NodeParam.CompRatio, bypass ? 1 : next.ratio);
    set(NodeParam.CompAttack, next.attack);
    set(NodeParam.CompRelease, next.release);
    set(NodeParam.CompKnee, next.knee);
    set(NodeParam.CompMakeup, bypass ? Math.max(-24, Math.min(24, dryTrim.current)) : next.makeup);
  }

  useEffect(() => {
    const id = runtime.addEffect(MASTER_BUS, EffectKind.Compressor);
    effect.current = id;
    push(settings.current, false);
    const off = runtime.onMeters((m) => {
      const c = compressorMeter(m, MASTER_BUS);
      const a = acc.current;
      a.inPk = Math.max(a.inPk, c.inPeak);
      a.outPk = Math.max(a.outPk, c.outPeak);
      a.gr = Math.min(a.gr, c.gainReductionDb);
      if (!dryRef.current) loudness.current.push(c.inPeak, c.gainReductionDb, settings.current.makeup);
    });
    return () => {
      off();
      runtime.removeEffect(MASTER_BUS, id);
      effect.current = null;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [runtime]);

  function update(patch: Partial<CompressorSettings>) {
    const next = { ...settings.current, ...patch };
    setS(next);
    setPreset(null);
    push(next, dryRef.current);
  }

  function loadPreset(i: number) {
    // Fit the threshold to what is actually playing, else to a normal drum level.
    const recent = Math.max(...live.current.peakWindow, -120);
    const peak = playing && recent > -40 ? recent : DEFAULT_DRUM_PEAK_DB;
    const next = presetSettings(COMPRESSOR_PRESETS[i], peak);
    setS(next);
    setPreset(i);
    push(next, dryRef.current);
  }

  function toggleDry() {
    const next = !dry;
    if (next) dryTrim.current = loudness.current.db();
    dryRef.current = next;
    setDry(next);
    push(settings.current, next);
  }

  // The drum loop.
  useEffect(() => {
    if (!playing) return;
    const insts: { id: InstrumentId; steps: [number, number][] }[] = PATTERN.map((p) => ({
      id: runtime.createInstrument(p.kind, { bus: MASTER_BUS }),
      steps: p.steps,
    }));
    let step = 0;
    const tick = () => {
      for (const i of insts) {
        for (const [at, vel] of i.steps) if (at === step) runtime.instrumentNoteOn(i.id, { velocity: vel, duration: 0.2 });
      }
      step = (step + 1) % 16;
    };
    tick();
    const timer = window.setInterval(tick, STEP_MS);
    return () => {
      window.clearInterval(timer);
      for (const i of insts) runtime.removeInstrument(i.id);
    };
  }, [runtime, playing]);

  // Drawing: one rAF loop, nothing here touches React state except a ~10 Hz readout.
  useEffect(() => {
    const cc = curveCanvas.current;
    const mc = meterCanvas.current;
    if (!cc || !mc) return;
    const colorSet = colors(cc);
    let raf = 0;
    let lastReadout = 0;
    let lastFrame = performance.now();
    const L = live.current;
    const fall = (cur: number, now: number, dt: number) => Math.max(now, cur - 30 * dt);

    const frame = (t: number) => {
      const dt = Math.min(0.1, (t - lastFrame) / 1000);
      lastFrame = t;
      const a = acc.current;
      const inDb = a.inPk > 1e-6 ? peakToDb(a.inPk) : -120;
      const outDb = a.outPk > 1e-6 ? peakToDb(a.outPk) : -120;
      L.inDb = fall(L.inDb, inDb, dt);
      L.outDb = fall(L.outDb, outDb, dt);
      // Gain reduction: instant on the way down, eased on the way back so a 5 ms dip is still visible.
      L.gr = Math.min(a.gr, Math.min(0, L.gr + 40 * dt));
      L.peakWindow.push(inDb);
      if (L.peakWindow.length > 180) L.peakWindow.shift();
      history.current.push(a.gr);
      a.inPk = a.outPk = a.gr = 0;

      drawCurve(cc, colorSet, settings.current, dryRef.current, L.inDb, L.gr);
      drawMeters(mc, colorSet, settings.current, L.inDb, L.outDb, L.gr, history.current.values(), dryRef.current);
      if (t - lastReadout > 100) {
        lastReadout = t;
        setReadout({ inDb: L.inDb, grDb: L.gr, outDb: L.outDb });
      }
      raf = requestAnimationFrame(frame);
    };
    raf = requestAnimationFrame(frame);
    return () => cancelAnimationFrame(raf);
  }, []);

  const over = readout.inDb - s.threshold;
  const expected = staticGainReductionDb(readout.inDb, s);
  const active = readout.inDb > -100;

  return (
    <section className="panel comp">
      <h2>Compressor <span className="comp__sub">drum bus</span></h2>
      <div className="comp__top">
        <button onClick={() => setPlaying((p) => !p)}>{playing ? "Stop drums" : "Play drums"}</button>
        <button
          className={dry ? "comp__ab comp__ab--dry" : "comp__ab"}
          onClick={toggleDry}
          title="Switches between the compressed bus and the same bus uncompressed, raised or lowered by what the compressor was doing to the level, so it is not just a loudness comparison."
        >
          {dry ? "Dry (level-matched)" : "Compressed"}
        </button>
      </div>

      <div className="comp__presets" role="group" aria-label="Presets">
        {COMPRESSOR_PRESETS.map((p, i) => (
          <button key={p.name} title={p.purpose} className={preset === i ? "comp__preset comp__preset--on" : "comp__preset"} onClick={() => loadPreset(i)}>
            {p.name}
          </button>
        ))}
      </div>

      <div className="comp__display">
        <canvas ref={curveCanvas} width={440} height={440} aria-label="Transfer curve" />
        <canvas ref={meterCanvas} width={600} height={440} aria-label="Level and gain reduction" />
      </div>

      <p className="comp__story">
        {dry ? (
          <>Dry: compression is off and the level is matched to what it was doing.</>
        ) : !active ? (
          <>Play something to see what it does. Threshold {fmtDb(s.threshold)}, {s.ratio.toFixed(1)}:1.</>
        ) : over <= -s.knee / 2 ? (
          <>Peaks are {Math.abs(over).toFixed(1)} dB <b>below</b> the threshold: nothing to compress. Lower the threshold.</>
        ) : (
          <>
            Peaks are <b>{Math.max(0, over).toFixed(1)} dB over</b> the threshold. At {s.ratio.toFixed(1)}:1 that settles at{" "}
            <b>{fmtDb(expected)}</b>; the meter reads <b>{fmtDb(readout.grDb)}</b> ({reductionCharacter(readout.grDb)}) because attack lets
            the start of each hit through.
          </>
        )}
      </p>

      <div className="comp__controls">
        <Big label="Threshold" value={fmtDb(s.threshold)} v={s.threshold} min={-60} max={0} step={0.5} onChange={(v) => update({ threshold: v })} />
        <Big label="Ratio" value={s.ratio < 1.05 ? "1:1" : `${s.ratio.toFixed(1)}:1`} v={logPos(s.ratio, 1, RATIO_MAX)} min={0} max={1} step={0.001} onChange={(p) => update({ ratio: fromLogPos(p, 1, RATIO_MAX) })} />
        <Big label="Attack" value={fmtMs(s.attack)} v={logPos(s.attack, ATTACK[0], ATTACK[1])} min={0} max={1} step={0.001} onChange={(p) => update({ attack: fromLogPos(p, ATTACK[0], ATTACK[1]) })} />
        <Big label="Release" value={fmtMs(s.release)} v={logPos(s.release, RELEASE[0], RELEASE[1])} min={0} max={1} step={0.001} onChange={(p) => update({ release: fromLogPos(p, RELEASE[0], RELEASE[1]) })} />
        <Big label="Knee" value={`${s.knee.toFixed(0)} dB`} v={s.knee} min={0} max={24} step={1} onChange={(v) => update({ knee: v })} />
        <Big label="Makeup" value={fmtDb(s.makeup)} v={s.makeup} min={-24} max={24} step={0.1} onChange={(v) => update({ makeup: v })} />
      </div>
      <p className="hint">
        Presets set the threshold from what is playing (or a normal drum level), so each lands on its intended gain reduction
        whatever the kit's level. Makeup is always explicit; nothing is added behind your back.
      </p>
    </section>
  );
}

function Big(props: { label: string; value: string; v: number; min: number; max: number; step: number; onChange: (v: number) => void }) {
  return (
    <label className="cf__big">
      <span className="cf__bigHead">
        <span>{props.label}</span>
        <output>{props.value}</output>
      </span>
      <input type="range" min={props.min} max={props.max} step={props.step} value={props.v} onChange={(e) => props.onChange(Number(e.target.value))} />
    </label>
  );
}

type Palette = ReturnType<typeof colors>;

// Transfer curve: input level across, output level up. The dim diagonal is 1:1; the bright curve is what the compressor
// does; the shaded wedge between them is the reduction; the dashed line is the threshold. The dot is the live input peak.
function drawCurve(cv: HTMLCanvasElement, c: Palette, s: CompressorSettings, dry: boolean, inDb: number, grDb: number) {
  const ctx = cv.getContext("2d");
  if (!ctx) return;
  const W = cv.width;
  const H = cv.height;
  const pad = 36;
  const x = (db: number) => pad + ((db - DB_MIN) / -DB_MIN) * (W - pad - 10);
  const y = (db: number) => H - pad - ((db - DB_MIN) / -DB_MIN) * (H - pad - 10);
  ctx.clearRect(0, 0, W, H);
  ctx.font = "20px system-ui, sans-serif";
  ctx.lineWidth = 1;
  ctx.strokeStyle = c.border;
  ctx.fillStyle = c.muted;
  for (let db = DB_MIN; db <= 0; db += 12) {
    ctx.beginPath(); ctx.moveTo(x(db), y(DB_MIN)); ctx.lineTo(x(db), y(0)); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(x(DB_MIN), y(db)); ctx.lineTo(x(0), y(db)); ctx.stroke();
    ctx.textAlign = "center"; ctx.fillText(String(db), x(db), H - 10);
    ctx.textAlign = "right"; ctx.fillText(String(db), pad - 6, y(db) + 7);
  }
  const ratio = dry ? 1 : s.ratio;
  const eff = { threshold: s.threshold, ratio, knee: s.knee };
  // reduction wedge
  ctx.beginPath();
  for (let db = DB_MIN; db <= 0; db += 0.5) (db === DB_MIN ? ctx.moveTo : ctx.lineTo).call(ctx, x(db), y(db));
  for (let db = 0; db >= DB_MIN; db -= 0.5) ctx.lineTo(x(db), y(compressorCurveDb(db, eff)));
  ctx.closePath();
  ctx.fillStyle = withAlpha(c.gr, 0.22);
  ctx.fill();
  // 1:1
  ctx.strokeStyle = c.muted; ctx.setLineDash([4, 6]); ctx.lineWidth = 1.5;
  ctx.beginPath(); ctx.moveTo(x(DB_MIN), y(DB_MIN)); ctx.lineTo(x(0), y(0)); ctx.stroke();
  // threshold
  ctx.setLineDash([8, 6]); ctx.strokeStyle = c.gr;
  ctx.beginPath(); ctx.moveTo(x(s.threshold), y(DB_MIN)); ctx.lineTo(x(s.threshold), y(0)); ctx.stroke();
  ctx.setLineDash([]);
  // the curve
  ctx.strokeStyle = c.accent; ctx.lineWidth = 3.5;
  ctx.beginPath();
  for (let db = DB_MIN; db <= 0; db += 0.5) (db === DB_MIN ? ctx.moveTo : ctx.lineTo).call(ctx, x(db), y(compressorCurveDb(db, eff)));
  ctx.stroke();
  // live position: the peak sits on the diagonal, the line down to the dot is the reduction in progress
  if (inDb > DB_MIN) {
    const out = Math.max(DB_MIN, inDb + grDb);
    ctx.strokeStyle = c.gr; ctx.lineWidth = 3;
    ctx.beginPath(); ctx.moveTo(x(inDb), y(inDb)); ctx.lineTo(x(inDb), y(out)); ctx.stroke();
    ctx.fillStyle = c.fg;
    ctx.beginPath(); ctx.arc(x(inDb), y(out), 7, 0, Math.PI * 2); ctx.fill();
  }
  ctx.fillStyle = c.muted; ctx.textAlign = "left";
  ctx.fillText("out dB", pad + 8, 24);
  ctx.textAlign = "right"; ctx.fillText("in dB", W - 8, H - 34);
}

// Three bars (input peak, gain reduction, output peak) and the gain-reduction history to their right, newest at the right edge.
function drawMeters(cv: HTMLCanvasElement, c: Palette, s: CompressorSettings, inDb: number, outDb: number, grDb: number, hist: Float32Array, dry: boolean) {
  const ctx = cv.getContext("2d");
  if (!ctx) return;
  const W = cv.width;
  const H = cv.height;
  const top = 34;
  const bot = H - 34;
  ctx.clearRect(0, 0, W, H);
  ctx.font = "20px system-ui, sans-serif";
  const levelY = (db: number) => bot - ((Math.max(DB_MIN, Math.min(0, db)) - DB_MIN) / -DB_MIN) * (bot - top);
  const bars: { x: number; label: string }[] = [{ x: 8, label: "in" }, { x: 78, label: "GR" }, { x: 148, label: "out" }];
  const bw = 50;
  ctx.textAlign = "center";
  for (const b of bars) {
    ctx.fillStyle = c.border;
    ctx.fillRect(b.x, top, bw, bot - top);
    ctx.fillStyle = c.muted;
    ctx.fillText(b.label, b.x + bw / 2, H - 8);
  }
  ctx.fillStyle = c.accent;
  ctx.fillRect(bars[0].x, levelY(inDb), bw, bot - levelY(inDb));
  ctx.fillRect(bars[2].x, levelY(outDb), bw, bot - levelY(outDb));
  // threshold tick across the input bar
  ctx.strokeStyle = c.gr; ctx.lineWidth = 3;
  ctx.beginPath(); ctx.moveTo(bars[0].x - 4, levelY(s.threshold)); ctx.lineTo(bars[0].x + bw + 4, levelY(s.threshold)); ctx.stroke();
  // GR hangs down from the top: 0 dB at the top, GR_SCALE at the bottom
  const grY = (g: number) => top + (Math.min(GR_SCALE, -g) / GR_SCALE) * (bot - top);
  ctx.fillStyle = c.gr;
  ctx.fillRect(bars[1].x, top, bw, grY(grDb) - top);
  drawGrTicks(ctx, c, grY, bars[2].x + bw + 6, 0);
  // history
  const hx = 250;
  const hw = W - hx - 8;
  ctx.fillStyle = c.border;
  ctx.globalAlpha = 0.5;
  ctx.fillRect(hx, top, hw, bot - top);
  ctx.globalAlpha = 1;
  ctx.strokeStyle = c.border; ctx.lineWidth = 1;
  for (const g of [-3, -6, -12]) {
    ctx.beginPath(); ctx.moveTo(hx, grY(g)); ctx.lineTo(hx + hw, grY(g)); ctx.stroke();
  }
  ctx.fillStyle = withAlpha(c.gr, 0.35);
  ctx.beginPath();
  ctx.moveTo(hx, top);
  for (let i = 0; i < hist.length; i++) ctx.lineTo(hx + (i / (hist.length - 1)) * hw, grY(dry ? 0 : hist[i]));
  ctx.lineTo(hx + hw, top);
  ctx.closePath();
  ctx.fill();
  ctx.strokeStyle = c.gr; ctx.lineWidth = 2.5;
  ctx.beginPath();
  for (let i = 0; i < hist.length; i++) {
    const px = hx + (i / (hist.length - 1)) * hw;
    (i === 0 ? ctx.moveTo : ctx.lineTo).call(ctx, px, grY(hist[i]));
  }
  ctx.stroke();
  ctx.fillStyle = c.muted; ctx.textAlign = "left";
  ctx.fillText("gain reduction, last 3 s", hx, 22);
  ctx.textAlign = "right";
  ctx.fillText(`${(-grDb).toFixed(1)} dB`, W - 8, H - 8);
}

function drawGrTicks(ctx: CanvasRenderingContext2D, c: Palette, grY: (g: number) => number, x: number, _o: number) {
  ctx.fillStyle = c.muted; ctx.textAlign = "left"; ctx.font = "16px system-ui, sans-serif";
  for (const g of [0, -3, -6, -12]) ctx.fillText(String(g), x, grY(g) + 6);
  ctx.font = "20px system-ui, sans-serif";
}

function withAlpha(color: string, a: number) {
  const m = /^#([0-9a-f]{6})$/i.exec(color.trim());
  if (!m) return color;
  const n = parseInt(m[1], 16);
  return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`;
}
