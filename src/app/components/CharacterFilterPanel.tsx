import { useEffect, useRef, useState } from "react";
import type { AudioRuntime } from "../../runtime";
import {
  CharacterFilterMode,
  EffectKind,
  InstrumentKind,
  MASTER_BUS,
  NodeParam,
  type EffectId,
  type InstrumentId,
} from "../../runtime/types";

interface Props {
  runtime: AudioRuntime;
}

const SOURCES: { label: string; kind: InstrumentKind; hold?: number }[] = [
  { label: "Kick", kind: InstrumentKind.Kick },
  { label: "Snare", kind: InstrumentKind.Snare },
  { label: "Hi-hat", kind: InstrumentKind.HiHat },
  { label: "Bass", kind: InstrumentKind.Bass, hold: 0.35 },
  { label: "FM", kind: InstrumentKind.Fm, hold: 0.35 },
  { label: "Noise", kind: InstrumentKind.Noise, hold: 0.35 },
];

const TYPES: { label: string; mode: CharacterFilterMode }[] = [
  { label: "LP 24", mode: CharacterFilterMode.LowPass24 },
  { label: "LP 12", mode: CharacterFilterMode.LowPass12 },
  { label: "BP", mode: CharacterFilterMode.BandPass },
  { label: "HP", mode: CharacterFilterMode.HighPass },
];

const MIN_HZ = 30;
const MAX_HZ = 18000;
const hzFromPos = (p: number) => MIN_HZ * Math.pow(MAX_HZ / MIN_HZ, p);
const posFromHz = (hz: number) => Math.log(hz / MIN_HZ) / Math.log(MAX_HZ / MIN_HZ);
const fmtHz = (hz: number) => (hz >= 1000 ? `${(hz / 1000).toFixed(hz >= 10000 ? 1 : 2)} kHz` : `${Math.round(hz)} Hz`);

function characterName(c: number) {
  return c < 0.17 ? "Clean" : c < 0.5 ? "Warm" : c < 0.83 ? "Punchy" : "Aggressive";
}

// The Character Filter as a hardware-style filter section: one effect on the master bus, six big
// controls, and a source to run through it. Everything here is an ordinary client of the runtime
// (addEffect / setEffectParameter / createInstrument); the DSP is native/src/dsp/character_filter.h.
export function CharacterFilterPanel({ runtime }: Props) {
  const [mode, setMode] = useState(CharacterFilterMode.LowPass24);
  const [cutoffPos, setCutoffPos] = useState(posFromHz(1400));
  const [resonance, setResonance] = useState(0.35);
  const [drive, setDrive] = useState(0.2);
  const [character, setCharacter] = useState(0.4);
  const [env, setEnv] = useState(0);
  const [source, setSource] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [bypass, setBypass] = useState(false);

  const effect = useRef<EffectId | null>(null);
  const instrument = useRef<InstrumentId | null>(null);
  const values = useRef({ mode, cutoffPos, resonance, drive, character, env });
  values.current = { mode, cutoffPos, resonance, drive, character, env };

  // One effect instance for the panel's lifetime, started from the current control values.
  useEffect(() => {
    const id = runtime.addEffect(MASTER_BUS, EffectKind.CharacterFilter);
    effect.current = id;
    const v = values.current;
    runtime.setEffectParameter(MASTER_BUS, id, NodeParam.CharFilterMode, v.mode);
    runtime.setEffectParameter(MASTER_BUS, id, NodeParam.CharFilterCutoff, hzFromPos(v.cutoffPos));
    runtime.setEffectParameter(MASTER_BUS, id, NodeParam.CharFilterResonance, v.resonance);
    runtime.setEffectParameter(MASTER_BUS, id, NodeParam.CharFilterDrive, v.drive);
    runtime.setEffectParameter(MASTER_BUS, id, NodeParam.CharFilterCharacter, v.character);
    runtime.setEffectParameter(MASTER_BUS, id, NodeParam.CharFilterEnvAmount, v.env);
    return () => {
      runtime.removeEffect(MASTER_BUS, id);
      effect.current = null;
    };
  }, [runtime]);

  function set(param: NodeParam, value: number) {
    if (effect.current !== null) runtime.setEffectParameter(MASTER_BUS, effect.current, param, value);
  }

  useEffect(() => {
    if (effect.current !== null) runtime.setEffectEnabled(MASTER_BUS, effect.current, !bypass);
  }, [runtime, bypass]);

  // The source: a looping hit through the filter, restarted whenever the source changes.
  useEffect(() => {
    if (!playing) return;
    const src = SOURCES[source];
    const inst = runtime.createInstrument(src.kind, { bus: MASTER_BUS });
    instrument.current = inst;
    const hit = () => runtime.instrumentNoteOn(inst, { velocity: 0.9, duration: src.hold });
    hit();
    const timer = window.setInterval(hit, 500);
    return () => {
      window.clearInterval(timer);
      runtime.removeInstrument(inst);
      instrument.current = null;
    };
  }, [runtime, playing, source]);

  const hz = hzFromPos(cutoffPos);

  return (
    <section className="panel cf">
      <h2>Character Filter</h2>
      <div className="cf__source">
        <select value={source} onChange={(e) => setSource(Number(e.target.value))} aria-label="Source">
          {SOURCES.map((s, i) => (
            <option key={s.label} value={i}>{s.label}</option>
          ))}
        </select>
        <button onClick={() => setPlaying((p) => !p)}>{playing ? "Stop" : "Play"}</button>
        <button className={bypass ? "cf__bypass cf__bypass--on" : "cf__bypass"} onClick={() => setBypass((b) => !b)}>
          {bypass ? "Bypassed" : "Active"}
        </button>
      </div>

      <div className="cf__group">
        <h3>Filter</h3>
        <div className="cf__types" role="radiogroup" aria-label="Type">
          {TYPES.map((t) => (
            <button
              key={t.label}
              role="radio"
              aria-checked={mode === t.mode}
              className={mode === t.mode ? "cf__type cf__type--on" : "cf__type"}
              onClick={() => {
                setMode(t.mode);
                set(NodeParam.CharFilterMode, t.mode);
              }}
            >
              {t.label}
            </button>
          ))}
        </div>
        <Big label="Cutoff" value={fmtHz(hz)} v={cutoffPos} min={0} max={1} onChange={(p) => {
          setCutoffPos(p);
          set(NodeParam.CharFilterCutoff, hzFromPos(p));
        }} />
        <Big label="Resonance" value={resonance > 0.88 ? "sings" : resonance.toFixed(2)} v={resonance} min={0} max={1} onChange={(r) => {
          setResonance(r);
          set(NodeParam.CharFilterResonance, r);
        }} />
      </div>

      <div className="cf__group">
        <h3>Character</h3>
        <Big label="Drive" value={drive.toFixed(2)} v={drive} min={0} max={1} onChange={(d) => {
          setDrive(d);
          set(NodeParam.CharFilterDrive, d);
        }} />
        <Big label="Character" value={characterName(character)} v={character} min={0} max={1} onChange={(c) => {
          setCharacter(c);
          set(NodeParam.CharFilterCharacter, c);
        }} />
        <div className="cf__ticks"><span>Clean</span><span>Warm</span><span>Punchy</span><span>Aggressive</span></div>
      </div>

      <div className="cf__group">
        <h3>Modulation</h3>
        <Big label="Env" value={env === 0 ? "off" : `${env > 0 ? "+" : ""}${(env * 4).toFixed(1)} oct`} v={env} min={-1} max={1} onChange={(e) => {
          setEnv(e);
          set(NodeParam.CharFilterEnvAmount, e);
        }} />
        <p className="hint">
          Env follows the level of what goes in, so drums open the filter on every hit. Keyboard tracking
          is for synths: a voice passes <code>CharacterFilter::trackedCutoff()</code> as its cutoff.
        </p>
      </div>
    </section>
  );
}

function Big(props: { label: string; value: string; v: number; min: number; max: number; onChange: (v: number) => void }) {
  return (
    <label className="cf__big">
      <span className="cf__bigHead">
        <span>{props.label}</span>
        <output>{props.value}</output>
      </span>
      <input
        type="range"
        min={props.min}
        max={props.max}
        step={0.001}
        value={props.v}
        onChange={(e) => props.onChange(Number(e.target.value))}
      />
    </label>
  );
}
