// Plugin Cookbook — measurements of the helper library (cookbook/dsp.hpp).
// SPDX-License-Identifier: MIT
//
// Every block is checked against what its documentation promises: filter responses, compressor curve,
// aliasing, limiter ceiling and latency, reverb decay, and so on. Run with `python3 tools/recipe.py test-dsp`.

#include "cookbook/dsp.hpp"

#include <cstdio>
#include <functional>
#include <vector>

using namespace cookbook;

static int failures = 0;

static void check(bool ok, const char* block, const char* what, double value)
{
    std::printf("  %s  %-14s %-58s %9.3f\n", ok ? "PASS" : "FAIL", block, what, value);
    if (!ok)
        ++failures;
}

static bool finite(const std::vector<float>& v)
{
    for (float x : v)
        if (!std::isfinite(x))
            return false;
    return true;
}

static constexpr double kSr = 48000.0;

// Magnitude of one frequency in a signal (Goertzel), as the amplitude of a sine.
static double amplitudeAt(const std::vector<float>& x, double hz, double sr = kSr, size_t from = 0)
{
    const double w = 2.0 * 3.14159265358979323846 * hz / sr, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < x.size(); ++i)
    {
        const double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / (double)(x.size() - from);
}

static std::vector<float> sine(double hz, size_t n, float amp = 1.f, double sr = kSr)
{
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = amp * (float)std::sin(2.0 * 3.14159265358979323846 * hz * (double)i / sr);
    return v;
}

static double peak(const std::vector<float>& v, size_t from = 0)
{
    double p = 0;
    for (size_t i = from; i < v.size(); ++i)
        p = std::max(p, (double)std::fabs(v[i]));
    return p;
}

// Steady-state gain in dB of a mono process at one frequency.
static double gainDbAt(const std::function<float(float)>& f, double hz)
{
    const size_t n = 24000, settle = 12000;
    std::vector<float> in = sine(hz, n), out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = f(in[i]);
    // whole periods only, so Goertzel is exact
    const size_t periods = (size_t)((n - settle) * hz / kSr);
    const size_t len = periods > 0 ? (size_t)(periods * kSr / hz) : n - settle;
    std::vector<float> tail(out.begin() + (long)(n - len), out.end());
    std::vector<float> ref(in.begin() + (long)(n - len), in.end());
    return 20.0 * std::log10(std::max(1e-9, amplitudeAt(tail, hz) / amplitudeAt(ref, hz)));
}

static void testSvf()
{
    auto resp = [](Svf::Type t, float fc, float q, float db, double hz) {
        Svf f;
        f.set(t, kSr, fc, q, db);
        return gainDbAt([&](float x) { return f.process(x); }, hz);
    };
    check(std::fabs(resp(Svf::Lowpass, 1000, 0.7071f, 0, 100)) < 0.1, "Svf", "lowpass 1 kHz: 0 dB at 100 Hz", resp(Svf::Lowpass, 1000, 0.7071f, 0, 100));
    check(std::fabs(resp(Svf::Lowpass, 1000, 0.7071f, 0, 1000) + 3.01) < 0.1, "Svf", "lowpass 1 kHz: -3 dB at cutoff", resp(Svf::Lowpass, 1000, 0.7071f, 0, 1000));
    check(resp(Svf::Lowpass, 1000, 0.7071f, 0, 10000) < -38, "Svf", "lowpass 1 kHz: 12 dB/oct (< -38 dB at 10 kHz)", resp(Svf::Lowpass, 1000, 0.7071f, 0, 10000));
    check(resp(Svf::Highpass, 1000, 0.7071f, 0, 100) < -38, "Svf", "highpass 1 kHz: < -38 dB at 100 Hz", resp(Svf::Highpass, 1000, 0.7071f, 0, 100));
    check(std::fabs(resp(Svf::Bandpass, 2000, 4, 0, 2000)) < 0.1, "Svf", "bandpass: 0 dB at center", resp(Svf::Bandpass, 2000, 4, 0, 2000));
    check(resp(Svf::Notch, 2000, 2, 0, 2000) < -40, "Svf", "notch: < -40 dB at center", resp(Svf::Notch, 2000, 2, 0, 2000));
    check(std::fabs(resp(Svf::Peak, 1000, 1, 6, 1000) - 6) < 0.1, "Svf", "peak +6 dB at center", resp(Svf::Peak, 1000, 1, 6, 1000));
    check(std::fabs(resp(Svf::LowShelf, 500, 0.7071f, -6, 40) + 6) < 0.2, "Svf", "low shelf -6 dB: -6 dB at 40 Hz", resp(Svf::LowShelf, 500, 0.7071f, -6, 40));
    check(std::fabs(resp(Svf::HighShelf, 2000, 0.7071f, 6, 15000) - 6) < 0.3, "Svf", "high shelf +6 dB: +6 dB at 15 kHz", resp(Svf::HighShelf, 2000, 0.7071f, 6, 15000));
    check(std::fabs(resp(Svf::Allpass, 1000, 0.7071f, 0, 3000)) < 0.1, "Svf", "allpass: 0 dB everywhere", resp(Svf::Allpass, 1000, 0.7071f, 0, 3000));
    // cutoff modulated every sample between 50 Hz and 18 kHz at high resonance: stays bounded
    Svf f;
    Noise noise;
    std::vector<float> out(kSr);
    for (size_t i = 0; i < out.size(); ++i)
    {
        f.set(Svf::Lowpass, kSr, expMap(0.5f + 0.5f * (float)std::sin(i * 0.01), 50.f, 18000.f), 8.f);
        out[i] = f.process(noise.next() * 0.5f);
    }
    check(finite(out) && peak(out) < 20, "Svf", "per-sample cutoff sweep at Q 8: bounded (peak)", peak(out));
}

static void testTilt()
{
    TiltEq t;
    t.setup(kSr, 800);
    t.setTilt(0);
    const double flat = gainDbAt([&](float x) { return t.process(x); }, 3000);
    check(std::fabs(flat) < 0.01, "TiltEq", "0 dB tilt is flat", flat);
    t.setTilt(6);
    const double lo = gainDbAt([&](float x) { return t.process(x); }, 30);
    const double hi = gainDbAt([&](float x) { return t.process(x); }, 16000);
    check(std::fabs(lo + 3) < 0.2, "TiltEq", "+6 dB tilt: -3 dB at 30 Hz", lo);
    check(std::fabs(hi - 3) < 0.2, "TiltEq", "+6 dB tilt: +3 dB at 16 kHz", hi);
}

// Aliasing of a driven 5 kHz sine at 44.1 kHz: the 7th and 9th harmonics fold to 9.1 kHz and 900 Hz.
static double aliasDb(const std::function<float(float)>& f)
{
    const double sr = 44100.0;
    std::vector<float> in = sine(5000, 44100, 8.f, sr), out(in.size());
    for (size_t i = 0; i < in.size(); ++i)
        out[i] = f(in[i]);
    const double a = std::hypot(amplitudeAt(out, 9100, sr, 1000), amplitudeAt(out, 900, sr, 1000));
    return 20.0 * std::log10(std::max(1e-9, a / amplitudeAt(out, 5000, sr, 1000)));
}

static void testAdaa()
{
    AdaaTanh at;
    AdaaHardClip ah;
    const double plainTanh = aliasDb([](float x) { return std::tanh(x); });
    const double adaaTanh = aliasDb([&](float x) { return at.process(x); });
    const double plainClip = aliasDb([](float x) { return hardClip(x); });
    const double adaaClip = aliasDb([&](float x) { return ah.process(x); });
    check(adaaTanh < plainTanh - 6, "AdaaTanh", "aliasing at least 6 dB lower than tanh (dB less)", plainTanh - adaaTanh);
    check(adaaClip < plainClip - 6, "AdaaHardClip", "aliasing at least 6 dB lower than hardClip (dB less)", plainClip - adaaClip);
    // small signals pass almost unchanged (half a sample late)
    at.reset();
    const double small = gainDbAt([&](float x) { return at.process(0.01f * x) * 100.f; }, 100);
    check(std::fabs(small) < 0.05, "AdaaTanh", "small signals: 0 dB", small);
    std::vector<float> o(1000);
    for (size_t i = 0; i < o.size(); ++i)
        o[i] = ah.process(i % 2 ? 1e6f : -1e6f);
    check(finite(o) && peak(o) <= 1.0001, "AdaaHardClip", "huge input stays within +/-1 (peak)", peak(o));
}

static void testCompressor()
{
    // -10 dBFS sine, threshold -20, ratio 4, hard knee: 2.5 dB over -> 7.5 dB of reduction
    Compressor c;
    c.set(kSr, -20, 4, 5, 50, 0, Compressor::Rms);
    const float amp = dbToGain(-10.f) * std::sqrt(2.f); // RMS -10 dB
    const double g = gainDbAt([&](float x) { return c.process(amp * x) / amp; }, 1000);
    check(std::fabs(g + 7.5) < 0.6, "Compressor", "RMS -10 dB, thr -20, 4:1 -> -7.5 dB", g);
    check(std::fabs(c.gainReductionDb() - 7.5) < 1.0, "Compressor", "gainReductionDb() reports it", c.gainReductionDb());
    Compressor below;
    below.set(kSr, -20, 4, 5, 50, 6);
    const double u = gainDbAt([&](float x) { return below.process(0.01f * x) * 100.f; }, 1000);
    check(std::fabs(u) < 0.01, "Compressor", "-40 dB signal is untouched", u);
    // attack: a step from silence to 0 dB settles within ~5 attack times
    Compressor step;
    step.set(kSr, -20, 10, 2, 100, 0);
    for (int i = 0; i < (int)(0.010 * kSr); ++i)
        step.gain(1.f);
    check(step.gainReductionDb() > 17, "Compressor", "2 ms attack: > 17 dB reduction after 10 ms", step.gainReductionDb());
}

static void testLimiter()
{
    Limiter l;
    l.allocate(kSr, 5);
    l.set(-1, 50);
    Noise noise(7);
    std::vector<float> out(2 * (size_t)kSr);
    for (size_t i = 0; i < out.size(); ++i)
    {
        float x = noise.next() * 0.3f;
        if (i % 4801 == 0)
            x = 4.f; // +12 dBFS spikes
        out[i] = l.process(x);
    }
    check(peak(out) <= dbToGain(-1.f) * 1.0001, "Limiter", "never exceeds the -1 dB ceiling (peak)", peak(out));
    // latency: an impulse below the ceiling comes out exactly latency() samples later, untouched
    l.reset();
    std::vector<float> imp(1000, 0.f);
    for (size_t i = 0; i < imp.size(); ++i)
        imp[i] = l.process(i == 0 ? 0.5f : 0.f);
    check(std::fabs(imp[l.latency()] - 0.5f) < 1e-6, "Limiter", "latency() is exact (samples)", (double)l.latency());
    float a = 2.f, b = 0.1f;
    l.reset();
    double maxL = 0;
    for (int i = 0; i < 2000; ++i)
    {
        float x = i == 300 ? a : 0.f, y = i == 300 ? b : 0.f;
        l.processStereo(x, y);
        maxL = std::max(maxL, (double)std::fabs(x));
    }
    check(maxL <= dbToGain(-1.f) * 1.0001, "Limiter", "stereo, linked: within the ceiling (peak)", maxL);
}

static void testGate()
{
    NoiseGate g;
    g.set(kSr, -40, 1, 20, 50);
    g.reset();
    const double loud = gainDbAt([&](float x) { return g.process(0.5f * x) * 2.f; }, 500);
    check(std::fabs(loud) < 0.01, "NoiseGate", "above threshold: 0 dB", loud);
    std::vector<float> out((size_t)kSr);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = g.process(0.001f * (float)std::sin(i * 0.05)); // -60 dB
    const double tail = 20 * std::log10(std::max(1e-12, peak(out, out.size() / 2) / 0.001));
    check(tail < -60, "NoiseGate", "below threshold: muted (dB)", tail);
}

static void testPitch()
{
    PitchShifter ps;
    ps.allocate(kSr);
    ps.setRatio(semitonesToRatio(12));
    std::vector<float> in = sine(440, (size_t)kSr), out(in.size());
    for (size_t i = 0; i < in.size(); ++i)
        out[i] = ps.process(in[i]);
    const double up = amplitudeAt(out, 880, kSr, 4800), orig = amplitudeAt(out, 440, kSr, 4800);
    check(up > 4 * orig, "PitchShifter", "+12 semitones: 880 Hz dominates 440 Hz (ratio)", up / std::max(1e-9, orig));
    check(finite(out) && peak(out) < 1.05, "PitchShifter", "output within the input level (peak)", peak(out));
}

static void testReverb()
{
    Reverb r;
    r.allocate(kSr);
    r.set(0.5f, 0.5f, 1.f);
    std::vector<float> L(6 * (size_t)kSr), R(L.size());
    for (size_t i = 0; i < L.size(); ++i)
        r.process(i == 0 ? 1.f : 0.f, i == 0 ? 1.f : 0.f, L[i], R[i]);
    const double early = peak(L, 0), late = peak(L, 5 * (size_t)kSr);
    check(finite(L) && finite(R), "Reverb", "impulse response is finite", 0);
    check(late < early * 1e-3, "Reverb", "size 0.5: decays more than 60 dB within 5 s (dB)", 20 * std::log10(std::max(1e-12, late / early)));
    double diff = 0;
    for (size_t i = 0; i < L.size(); ++i)
        diff = std::max(diff, (double)std::fabs(L[i] - R[i]));
    check(diff > 1e-3, "Reverb", "width 1: left and right differ", diff);
    // loudest setting with full-scale noise stays bounded
    r.reset();
    r.set(1.f, 0.f, 1.f);
    Noise n;
    double p = 0;
    for (size_t i = 0; i < 10 * (size_t)kSr; ++i)
    {
        float a, b;
        r.process(n.next(), n.next(), a, b);
        p = std::max(p, (double)std::fabs(a));
    }
    check(std::isfinite(p) && p < 8, "Reverb", "size 1, no damping, full-scale noise: bounded (peak)", p);
}

static void testNoise()
{
    Noise n;
    double sum = 0, sq = 0, lo = 1, hi = -1;
    const int count = 1 << 20;
    for (int i = 0; i < count; ++i)
    {
        const double x = n.next();
        sum += x, sq += x * x, lo = std::min(lo, x), hi = std::max(hi, x);
    }
    check(std::fabs(sum / count) < 0.005, "Noise", "mean near 0", sum / count);
    check(std::fabs(sq / count - 1.0 / 3.0) < 0.005, "Noise", "uniform: mean square 1/3", sq / count);
    check(lo >= -1 && hi < 1, "Noise", "within [-1, 1) (max)", hi);
}

int main()
{
    testSvf();
    testTilt();
    testAdaa();
    testCompressor();
    testLimiter();
    testGate();
    testPitch();
    testReverb();
    testNoise();
    std::printf(failures ? "\n%d DSP checks FAILED.\n" : "\nAll DSP checks passed.\n", failures);
    return failures ? 1 : 0;
}
