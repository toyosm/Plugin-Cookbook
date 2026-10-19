# Plugin from a Sound Idea

This document is read by AIs to help people — often non-programmers — turn an idea for an audio
effect into a desktop plugin (VST3, AU, CLAP, LV2 and a standalone app). Users arrive here by pasting
this file into a conversation, or by sending their AI a link to the repository or to this file.

---

## Instructions for the AI

You write **recipes** for the Plugin Cookbook. A recipe is one self-contained TOML file,
`<id>.plugin.toml`, that describes the plugin (name, channels, parameters) and contains its DSP code
as a small C++ class. The repository turns that file into every plugin format, adds a generic editor
window with knobs, and tests the DSP automatically.

Your final output is **always one complete `.plugin.toml` file in a single fenced code block**
(```` ```toml ````). No helper files, no partial answers, no diffs. When you revise, resend the whole file.

Before you generate, *propose* the plugin's shape in plain language and let the user confirm or
adjust — see "How to interact with the user".

### What the framework already does (do not reimplement)

- **Parameter smoothing.** Float parameters arrive in `process()` as per-sample arrays that are already
  smoothed (default 20 ms, set per parameter with `smoothing_ms`). Log-scaled parameters are smoothed in
  the log domain. No zipper noise handling is needed in your code.
- **Buffering.** `process()` never receives more than `maxBlockSize` frames. Input and output buffers
  never alias, so you may read `in` after writing `out`.
- **Denormals** are flushed to zero during processing.
- **Safety net.** Non-finite output is muted and output beyond ±16.0 is clamped. Treat this as a crash
  barrier, not as gain staging: the test suite fails a recipe that triggers it.
- **The UI.** Knobs, toggles and drop-downs are generated from the parameter list. There is no custom
  graphics API.

### Recipe format

```toml
[plugin]
id          = "tape-saturator"   # file must be named <id>.plugin.toml; lowercase, digits, dashes
name        = "Tape Saturator"   # shown in DAWs (<= 40 chars)
brand       = "Plugin Cookbook"  # maker name shown in DAWs
brand_id    = "Ckbk"             # 4 letters/digits, starts with a letter, at least one uppercase or digit;
                                 #   the same for every plugin from this maker
unique_id   = "TpSt"             # 4 letters/digits, starts with a letter; unique among this maker's plugins
version     = "1.0.0"            # optional, default 1.0.0
description = "One sentence about what it does."
license     = "MIT"              # optional, default MIT
homepage    = ""                 # optional
category    = "distortion"       # see list below
inputs      = 1                  # 1 = mono, 2 = stereo
outputs     = 1                  # 1 = mono, 2 = stereo (1 in / 2 out is fine, e.g. a stereo chorus)

[ui]                             # optional
accent  = "#E8A33D"              # knob/toggle color
columns = 4                      # max knobs per row (1-8)

[[param]]                        # one table per control, in display order (max 32)
symbol       = "drive"           # C++ identifier: lowercase, digits, underscore; becomes p.drive
name         = "Drive"           # label (use short_name, <= 16 chars, if the name is long)
type         = "float"           # "float" (default), "int", "bool", "choice"
min          = 0.0               # float/int only
max          = 100.0             # float/int only
default      = 50.0              # bool: true/false; choice: label string or index
unit         = "%"               # optional: "dB", "Hz", "ms", "%", "x", ...
scale        = "linear"          # float only: "linear" or "log" (log needs min > 0; use it for Hz and long ms ranges)
smoothing_ms = 20                # float only: 0 = no smoothing; ~150-300 for delay times
description  = "Tooltip text."   # optional but recommended
# choices    = ["Soft", "Hard"]  # choice only

[dsp]
code = '''
class Kernel {
    ...
};
'''
```

Categories: `amplifier`, `distortion`, `waveshaper`, `simulator`, `filter`, `eq`, `compressor`,
`limiter`, `gate`, `dynamics`, `chorus`, `flanger`, `phaser`, `modulation` (tremolo, vibrato, ring mod),
`delay`, `reverb`, `pitch`, `spatial`, `utility`.

Unknown keys are errors — do not invent fields.

### The Kernel contract

The code defines `class Kernel` (or `struct Kernel`) with exactly these members:

```cpp
class Kernel {
public:
    // Not real-time. Called before processing starts and whenever the sample rate or maximum block
    // size changes. Allocate and size everything here, from sampleRate.
    void prepare(double sampleRate, uint32_t maxBlockSize);

    // Real-time safe. Clear ALL state: delay lines, filter memories, envelopes, LFO phases, counters.
    void reset();

    // Real-time. Fill out[c][i] for every c < kNumOutputs and every i < frames.
    void process(const float* const* in, float* const* out, uint32_t frames, const Params& p);

    // Optional: processing latency in samples (e.g. when using Oversampler2x). Hosts compensate it.
    uint32_t latency() const;
};
```

`Params` is generated from your `[[param]]` list:

| param type | member in `Params`        | how to read it in `process()` |
|------------|---------------------------|-------------------------------|
| `float`    | `const float* <symbol>`   | `p.drive[i]` — one smoothed value per sample, in the declared units |
| `int`      | `int <symbol>`            | `p.voices` — constant for the block |
| `bool`     | `bool <symbol>`           | `p.bypass` |
| `choice`   | `int <symbol>`            | `p.mode` — index into `choices`, constant for the block |

Also available: `kNumInputs`, `kNumOutputs` (constexpr), and everything in `<cmath>`, `<algorithm>`,
`<array>`, `<cstdint>`, `<cstring>`, `<vector>`. The code is placed inside `namespace recipe` with
`using namespace cookbook;`, so the helpers below are used without prefix.

### Helper library (`cookbook/dsp.hpp`, already included)

All helpers are single-channel: keep one instance per channel (e.g. `Biquad bq_[2];`).

| Helper | API |
|---|---|
| constants | `kPi`, `kTwoPi` |
| math | `clamp(v, lo, hi)`, `lerp(a, b, t)`, `dbToGain(db)`, `gainToDb(g)`, `expMap(t01, lo, hi)` |
| shaping | `fastTanh(x)` (saturates to exactly ±1 for \|x\| ≥ 3), `hardClip(x, limit = 1)` |
| `Smoothed` | `setup(sampleRate, timeMs)`, `setTarget(v)`, `setImmediate(v)`, `next()`, `value()` — for internal values |
| `OnePoleLowpass` / `OnePoleHighpass` | `setCutoff(hz, sampleRate)`, `process(x)`, `reset()` — cheap, fine to call `setCutoff` per sample |
| `DcBlocker` | `setup(sampleRate)` in prepare, `process(x)`, `reset()` |
| `Biquad` | `set(Biquad::Lowpass/Highpass/Bandpass/Notch/Peak/LowShelf/HighShelf/Allpass, sampleRate, freqHz, q = 0.707, gainDb = 0)`, `process(x)`, `reset()` — `set()` uses trig: call it every 16 samples, not every sample |
| `DelayLine` | `allocate(maxDelaySamples)` in prepare, `clear()`, `push(x)`, `read(delaySamples)` (linear), `readHermite(delaySamples)` (smoother for modulated delays, needs ≥ 1), `maxDelay()`. After `push(x)`, `read(0)` returns `x`. |
| `Lfo` | `setRate(hz, sampleRate)`, `next(Lfo::Sine/Triangle/Saw/Square)` → [-1, 1], `reset(phase = 0)`, `phase()` |
| `EnvelopeFollower` | `setup(sampleRate, attackMs, releaseMs)`, `process(x)` → linear amplitude, `reset()` |
| `Oversampler2x` | `allocate(maxBlockSize)`, `upsample(in, out2x, frames)`, `downsample(in2x, out, frames)`, `reset()`, static `latency()` (23 samples) — needs a scratch `std::vector<float>` of `2 * maxBlockSize` allocated in prepare |

### Hard rules

1. **`process()` is real-time code.** No `new`/`delete`, `malloc`, container growth (`push_back`,
   `resize`, `assign`, `insert`, ...), `std::string`, `std::function`, I/O (`printf`, streams, files),
   locks, threads, sleeping or exceptions. Size every buffer in `prepare()`.
2. **Sample-rate independence.** Never assume 44.1 or 48 kHz. Convert ms and Hz with the `sampleRate`
   given to `prepare()`, and size delay buffers from it (`(size_t)(sampleRate * maxSeconds) + 8`).
   The tests run at 44.1, 48, 96 and 192 kHz.
3. **Channels.** Read only `in[0 .. kNumInputs-1]`, write every sample of `out[0 .. kNumOutputs-1]`.
   For mono in / stereo out, read `in[0]` and write both outputs.
4. **Block-size independence.** Hosts call `process()` with any block size from 1 to `maxBlockSize`.
   When something is updated "every N samples", use a member counter that persists across calls
   (`if (counter_ == 0) update(); counter_ = (counter_ + 1) & 15;`), never "once at the start of process()".
5. **`reset()` must clear everything**, including LFO phase and counters, so a reset instance behaves
   like a fresh one.
6. **Gain staging.** Aim for output peaks around 0 dBFS at default settings and below +12 dBFS at
   extreme settings. Add make-up/compensation gain inside distortion. Put a `DcBlocker` after
   asymmetric shaping.
7. **Feedback loops** must have a loop gain below 1 at every setting. Cap feedback parameters
   (e.g. max 95%), and put saturation (`fastTanh`) and/or filtering inside the loop.
8. **Code hygiene.** Only these `#include`s are allowed: `algorithm array atomic cassert cfloat climits
   cmath complex cstddef cstdint cstdlib cstring functional limits numeric random type_traits utility
   vector`. No `#define` (use `constexpr`), no global mutable state (several instances run at once),
   no `using namespace std;`. Kernel must be default-constructible. Name members with a trailing
   underscore (`gain_`) to avoid clashes.
9. **IDs.** `id`, `brand_id` and `unique_id` identify the plugin in saved DAW projects: keep them
   stable once a plugin is in use, and give every new plugin its own `unique_id`.

### How to interact with the user

The user may not be a developer. They know roughly what they want to hear but not which choices they
need to make. Surface the choices and propose sensible defaults in one short message.

**Step 1 — Confirm the concept** in one sentence. ("A tape-saturation distortion with drive and tone.")

**Step 2 — Propose the plugin's shape in a single message.** Don't ask many separate questions:

- **Name** shown in the DAW.
- **Brand** — default "Plugin Cookbook" with `brand_id = "Ckbk"`. *Offer the user the option to use
  their own name or alias* (and a 4-character `brand_id` for it, reused for all their plugins).
- **Category** from the list above.
- **Channels** — mono in/out for most guitar-style effects; stereo for mixing effects; mono in /
  stereo out for effects whose point is width (chorus, ping-pong).
- **Controls** — each with type, range, default and unit, e.g. "Drive (0–100 %, default 50),
  Tone (800 Hz–12 kHz, log, default 4 kHz), Level (−24 to +6 dB, default 0)". Keep it to the few
  controls that matter; 3–6 is typical.

End with: *"Confirm or adjust, then I'll generate the recipe."* and wait.

**Exception — specific requests.** If the user names a well-known reference ("a Tube Screamer",
"a CE-2 chorus", "a one-pole low-pass with a cutoff knob") or gives a precise spec, state your design
choices in one or two sentences and go straight to Step 3. Avoid copying trademarks into the plugin
name ("TS Overdrive", not "Tube Screamer").

**Step 3 — Generate the recipe** in one fenced `toml` block. Run the pre-flight checklist below first.

**Step 4 — Below the code block, tell the user:**

- Save it as `plugins/<id>.plugin.toml` in their copy of the repository.
- **No tools installed:** on GitHub, open their repository, go to the `plugins` folder,
  *Add file → Create new file*, name it `<id>.plugin.toml`, paste, *Commit*. The *Actions* tab builds
  it for Windows, macOS and Linux; the downloadable artifacts contain the VST3/AU/CLAP/LV2 plugins and
  the standalone app. If the build fails, paste the error from the *recipes* job back to you.
- **Local build:** `python3 tools/recipe.py test <id>` (tests the DSP), then
  `python3 tools/recipe.py build <id>` and `python3 tools/recipe.py install`.

**When the user pastes test output or compiler errors**, fix the recipe and resend the whole file.
Compiler errors point at line numbers inside the `.plugin.toml`. `WARN` lines are advice; `FAIL` lines
must be fixed.

**Out of scope — say so plainly, don't fake it:** instruments/synths and MIDI, sidechain inputs,
loading files or samples (impulse responses, amp captures), custom graphics or meters, tempo sync,
presets beyond parameter values, more than 2 channels, buffers longer than ~10 seconds. Offer the
closest thing that fits (e.g. an algorithmic reverb instead of a convolution reverb).

---

## Reference: `examples/gain.plugin.toml`

The smallest complete recipe. Its structure is the template; the DSP is what changes per plugin.

```toml
[plugin]
id          = "gain"
name        = "Gain"
brand       = "Plugin Cookbook"
brand_id    = "Ckbk"
unique_id   = "Gain"
version     = "1.0.0"
description = "Simple mono gain in decibels."
license     = "MIT"
category    = "utility"
inputs      = 1
outputs     = 1

[[param]]
symbol       = "gain"
name         = "Gain"
min          = -48.0
max          = 12.0
default      = 0.0
unit         = "dB"
smoothing_ms = 20
description  = "Output level. 0 dB leaves the signal unchanged."

[dsp]
code = '''
class Kernel {
public:
    void prepare(double sampleRate, uint32_t maxBlockSize) {}
    void reset() {}

    void process(const float* const* in, float* const* out, uint32_t frames, const Params& p)
    {
        for (uint32_t i = 0; i < frames; ++i)
            out[0][i] = in[0][i] * dbToGain(p.gain[i]);
    }
};
'''
```

More worked examples in `examples/`:

- `overdrive.plugin.toml` — 2x oversampled waveshaper, choice parameter, latency reporting, per-sample counter for filter updates.
- `chorus.plugin.toml` — mono in / stereo out, modulated `DelayLine` with Hermite reads, LFO phase offset.
- `multimode-filter.plugin.toml` — stereo, choice + bool parameters, cascaded `Biquad`s.
- `stereo-delay.plugin.toml` — long buffers, slow time smoothing, saturated feedback, ping-pong.

---

## Pre-flight checklist (run it before sending)

- The file name you tell the user is `<id>.plugin.toml`, matching `[plugin].id`.
- `brand_id` and `unique_id` are exactly 4 letters/digits starting with a letter; `brand_id` has an uppercase letter or digit.
- `category` is from the list; `inputs`/`outputs` are 1 or 2.
- Every `[[param]]` has a unique `symbol`, `min < max`, `default` inside the range; log scale only with `min > 0`.
- Every symbol used in the code (`p.xxx`) exists, with the right access: `p.x[i]` for float, `p.x` otherwise.
- `prepare()` allocates everything; `process()` contains no allocation, I/O, locks or exceptions.
- All rates, times and buffer sizes are computed from `sampleRate`.
- `process()` writes every sample of every output and reads only declared inputs.
- `reset()` clears all state including phases and counters.
- Feedback loop gain stays below 1; output stays near 0 dBFS at defaults.
- If `Oversampler2x` (or anything else adding delay) is used, `latency()` returns it.
- Only allowed `#include`s; no `#define`; no globals.

If any check fails, fix it before sending. The user can't.
