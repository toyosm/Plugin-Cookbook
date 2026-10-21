// Plugin Cookbook — DSP helper library for recipe kernels.
// SPDX-License-Identifier: MIT
//
// Everything here is header-only, dependency-free and real-time safe *after*
// allocation. Classes that need memory have an explicit allocate()/setup()
// method that must be called from Kernel::prepare() (never from process()).
//
// All processing is single-channel. For stereo, keep one instance per channel.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cookbook {

// ---------------------------------------------------------------------------------------------------------------------
// Constants and small math helpers

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

template <class T>
inline T clamp(T v, T lo, T hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

inline float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

// Decibels <-> linear gain. dbToGain(-inf) is 0; values below -120 dB are treated as silence.
inline float dbToGain(float db) noexcept { return db <= -120.f ? 0.f : std::pow(10.f, db * 0.05f); }
inline float gainToDb(float gain) noexcept { return 20.f * std::log10(std::max(std::fabs(gain), 1e-6f)); }

// Rational tanh approximation, exact at 0 and saturating to exactly +/-1 for |x| >= 3. Smooth, cheap.
inline float fastTanh(float x) noexcept
{
    x = clamp(x, -3.f, 3.f);
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

inline float hardClip(float x, float limit = 1.f) noexcept { return clamp(x, -limit, limit); }

// Map a normalized 0..1 value onto [lo, hi] exponentially (for frequencies, times). lo must be > 0.
inline float expMap(float t, float lo, float hi) noexcept { return lo * std::pow(hi / lo, clamp(t, 0.f, 1.f)); }

// ---------------------------------------------------------------------------------------------------------------------
// One-pole smoother for internal values (parameters are already smoothed by the framework).

class Smoothed
{
public:
    // timeMs: time to reach ~99% of a step.
    void setup(double sampleRate, float timeMs) noexcept
    {
        const double tau = std::max(1e-6, timeMs * 0.001 / 4.6);
        a_ = timeMs <= 0.f ? 0.f : (float)std::exp(-1.0 / (tau * sampleRate));
    }
    void  setTarget(float t) noexcept { target_ = t; }
    void  setImmediate(float v) noexcept { target_ = value_ = v; }
    float next() noexcept { value_ = target_ + a_ * (value_ - target_); return value_; }
    float value() const noexcept { return value_; }

private:
    float a_ = 0.f, target_ = 0.f, value_ = 0.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// One-pole filters (6 dB/oct)

class OnePoleLowpass
{
public:
    void setCutoff(float hz, double sampleRate) noexcept
    {
        hz = clamp(hz, 1.f, (float)(sampleRate * 0.49));
        a_ = (float)std::exp(-2.0 * 3.14159265358979323846 * hz / sampleRate);
    }
    float process(float x) noexcept { y_ = x + a_ * (y_ - x); return y_; }
    void  reset(float v = 0.f) noexcept { y_ = v; }

private:
    float a_ = 0.f, y_ = 0.f;
};

class OnePoleHighpass
{
public:
    void  setCutoff(float hz, double sampleRate) noexcept { lp_.setCutoff(hz, sampleRate); }
    float process(float x) noexcept { return x - lp_.process(x); }
    void  reset() noexcept { lp_.reset(); }

private:
    OnePoleLowpass lp_;
};

// Removes DC offset (first-order highpass at ~10 Hz). Use after asymmetric waveshaping.
class DcBlocker
{
public:
    void setup(double sampleRate) noexcept { r_ = 1.f - (float)(2.0 * 3.14159265358979323846 * 10.0 / sampleRate); }
    float process(float x) noexcept
    {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }
    void reset() noexcept { x1_ = y1_ = 0.f; }

private:
    float r_ = 0.995f, x1_ = 0.f, y1_ = 0.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// Biquad (RBJ Audio EQ Cookbook), transposed direct form II.
// set() computes coefficients with trig functions: in process(), call it at most every ~16-32 samples
// or only when the controlling parameter changed.

class Biquad
{
public:
    enum Type { Lowpass, Highpass, Bandpass, Notch, Peak, LowShelf, HighShelf, Allpass };

    void set(Type type, double sampleRate, float freqHz, float q = 0.7071f, float gainDb = 0.f) noexcept
    {
        const double f  = clamp((double)freqHz, 10.0, sampleRate * 0.49);
        const double qq = std::max(0.05, (double)q);
        const double w0 = 2.0 * 3.14159265358979323846 * f / sampleRate;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * qq);
        const double A = std::pow(10.0, gainDb / 40.0);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

        switch (type)
        {
        case Lowpass:   b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Highpass:  b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Bandpass:  b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Notch:     b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Allpass:   b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Peak:      b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
        case LowShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s;       a1 = -2 * ((A - 1) + (A + 1) * cw);    a2 = (A + 1) + (A - 1) * cw - s;
            break;
        }
        case HighShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s;       a1 = 2 * ((A - 1) - (A + 1) * cw);      a2 = (A + 1) - (A - 1) * cw - s;
            break;
        }
        }
        b0_ = (float)(b0 / a0); b1_ = (float)(b1 / a0); b2_ = (float)(b2 / a0);
        a1_ = (float)(a1 / a0); a2_ = (float)(a2 / a0);
    }

    float process(float x) noexcept
    {
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

    void reset() noexcept { z1_ = z2_ = 0.f; }

private:
    float b0_ = 1.f, b1_ = 0.f, b2_ = 0.f, a1_ = 0.f, a2_ = 0.f;
    float z1_ = 0.f, z2_ = 0.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// Delay line with fractional reads.
//
//   prepare():  delay.allocate((size_t)(sampleRate * 2.0) + 4);   // 2 seconds max
//   process():  delay.push(x);  float y = delay.read(delaySamples);
//
// After push(x), read(0) returns x, read(1) the previous sample, and so on.

class DelayLine
{
public:
    void allocate(size_t maxDelaySamples)
    {
        size_t size = 4;
        while (size < maxDelaySamples + 4)
            size <<= 1;
        buffer_.assign(size, 0.f);
        mask_ = size - 1;
        maxDelay_ = (float)maxDelaySamples;
        write_ = 0;
    }

    void clear() noexcept { std::fill(buffer_.begin(), buffer_.end(), 0.f); write_ = 0; }

    void push(float x) noexcept
    {
        write_ = (write_ + 1) & mask_;
        buffer_[write_] = x;
    }

    // Linear interpolation. delaySamples is clamped to [0, maxDelay].
    float read(float delaySamples) const noexcept
    {
        const float d  = clamp(delaySamples, 0.f, maxDelay_);
        const size_t i = (size_t)d;
        const float frac = d - (float)i;
        const float s0 = buffer_[(write_ - i) & mask_];
        const float s1 = buffer_[(write_ - i - 1) & mask_];
        return s0 + frac * (s1 - s0);
    }

    // 4-point Hermite interpolation: smoother for modulated delays (chorus, vibrato). Clamped to [1, maxDelay].
    float readHermite(float delaySamples) const noexcept
    {
        const float d  = clamp(delaySamples, 1.f, maxDelay_);
        const size_t i = (size_t)d;
        const float t  = d - (float)i;
        const float xm1 = buffer_[(write_ - i + 1) & mask_];
        const float x0  = buffer_[(write_ - i) & mask_];
        const float x1  = buffer_[(write_ - i - 1) & mask_];
        const float x2  = buffer_[(write_ - i - 2) & mask_];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    float maxDelay() const noexcept { return maxDelay_; }

private:
    std::vector<float> buffer_ = std::vector<float>(4, 0.f);
    size_t mask_ = 3, write_ = 0;
    float maxDelay_ = 0.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// Low-frequency oscillator. Output in [-1, 1]. Call setRate() whenever the rate parameter moves (cheap).

class Lfo
{
public:
    enum Shape { Sine, Triangle, Saw, Square };

    void setRate(float hz, double sampleRate) noexcept { inc_ = (float)(std::max(0.f, hz) / sampleRate); }
    void reset(float phase = 0.f) noexcept { phase_ = phase - std::floor(phase); }
    float phase() const noexcept { return phase_; }

    float next(Shape shape = Sine) noexcept
    {
        float y;
        switch (shape)
        {
        case Triangle: y = 1.f - 4.f * std::fabs(phase_ - 0.5f); break;   // starts at -1
        case Saw:      y = 2.f * phase_ - 1.f; break;
        case Square:   y = phase_ < 0.5f ? 1.f : -1.f; break;
        default:       y = std::sin(kTwoPi * phase_); break;
        }
        phase_ += inc_;
        if (phase_ >= 1.f)
            phase_ -= std::floor(phase_);
        return y;
    }

private:
    float phase_ = 0.f, inc_ = 0.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// Peak envelope follower with separate attack/release. Returns a linear amplitude >= 0.

class EnvelopeFollower
{
public:
    void setup(double sampleRate, float attackMs, float releaseMs) noexcept
    {
        att_ = coeff(sampleRate, attackMs);
        rel_ = coeff(sampleRate, releaseMs);
    }
    float process(float x) noexcept
    {
        const float a = std::fabs(x);
        env_ = a + (a > env_ ? att_ : rel_) * (env_ - a);
        return env_;
    }
    void  reset() noexcept { env_ = 0.f; }
    float value() const noexcept { return env_; }

private:
    static float coeff(double sr, float ms) noexcept
    {
        return ms <= 0.f ? 0.f : (float)std::exp(-1.0 / (std::max(1e-6, ms * 0.001) * sr));
    }
    float att_ = 0.f, rel_ = 0.f, env_ = 0.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// 2x oversampler (linear-phase halfband FIR, 47 taps, ~-90 dB stopband). Use around nonlinear stages to
// reduce aliasing. Adds latency(): report it from Kernel::latency() so hosts can compensate.
//
//   prepare():  os.allocate(maxBlockSize); upBuf.assign(2 * maxBlockSize, 0.f);
//   process():  os.upsample(in, upBuf.data(), frames);
//               for (uint32_t i = 0; i < 2 * frames; ++i) upBuf[i] = shape(upBuf[i]);
//               os.downsample(upBuf.data(), out, frames);

class Oversampler2x
{
public:
    static constexpr int kTaps = 47;
    static constexpr int kCenter = (kTaps - 1) / 2; // 23 (odd), so odd-indexed taps are zero except the center
    static constexpr int kEven = (kTaps + 1) / 2;    // 24 even-indexed taps

    Oversampler2x() { design(); }

    void allocate(uint32_t /*maxBlockSize*/) { reset(); }

    void reset() noexcept
    {
        std::memset(upHist_, 0, sizeof(upHist_));
        std::memset(dnEven_, 0, sizeof(dnEven_));
        std::memset(dnOdd_, 0, sizeof(dnOdd_));
        upPos_ = dnEvenPos_ = dnOddPos_ = 0;
    }

    // Latency in base-rate samples (up + down).
    static constexpr uint32_t latency() noexcept { return kCenter; }

    // out must hold 2 * frames samples.
    void upsample(const float* in, float* out, uint32_t frames) noexcept
    {
        for (uint32_t n = 0; n < frames; ++n)
        {
            upPos_ = (upPos_ + kEven - 1) % kEven;
            upHist_[upPos_] = upHist_[upPos_ + kEven] = in[n];
            const float* h = upHist_ + upPos_; // h[j] = x[n - j]
            float even = 0.f;
            for (int j = 0; j < kEven; ++j)
                even += evenTaps_[j] * h[j];
            out[2 * n]     = 2.f * even;
            out[2 * n + 1] = 2.f * centerTap_ * h[(kCenter - 1) / 2]; // x[n - 11]
        }
    }

    // in holds 2 * frames samples at the oversampled rate.
    void downsample(const float* in, float* out, uint32_t frames) noexcept
    {
        for (uint32_t n = 0; n < frames; ++n)
        {
            // even-rate samples feed the even taps, odd-rate samples feed the center tap
            dnEvenPos_ = (dnEvenPos_ + kEven - 1) % kEven;
            dnEven_[dnEvenPos_] = dnEven_[dnEvenPos_ + kEven] = in[2 * n];
            dnOddPos_ = (dnOddPos_ + kEven - 1) % kEven;
            dnOdd_[dnOddPos_] = dnOdd_[dnOddPos_ + kEven] = in[2 * n + 1];

            const float* e = dnEven_ + dnEvenPos_; // e[j] = w[2n - 2j]
            const float* o = dnOdd_ + dnOddPos_;   // o[j] = w[2n + 1 - 2j]
            float acc = 0.f;
            for (int j = 0; j < kEven; ++j)
                acc += evenTaps_[j] * e[j];
            // center tap k = 23 multiplies w[2n - 23] = w[2(n - 12) + 1] = o[12]
            acc += centerTap_ * o[(kCenter + 1) / 2];
            out[n] = acc;
        }
    }

private:
    void design() noexcept
    {
        // Windowed-sinc halfband (Blackman-Harris window), normalized to unity DC gain.
        double h[kTaps];
        double sum = 0.0;
        for (int k = 0; k < kTaps; ++k)
        {
            const int m = k - kCenter;
            const double sinc = m == 0 ? 0.5 : std::sin(3.14159265358979323846 * m * 0.5) / (3.14159265358979323846 * m);
            const double t = 2.0 * 3.14159265358979323846 * k / (kTaps - 1);
            const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
            h[k] = sinc * w;
            sum += h[k];
        }
        for (int k = 0; k < kTaps; ++k)
            h[k] /= sum;
        for (int j = 0; j < kEven; ++j)
            evenTaps_[j] = (float)h[2 * j];
        centerTap_ = (float)h[kCenter];
    }

    float evenTaps_[kEven] = {};
    float centerTap_ = 0.5f;
    float upHist_[2 * kEven] = {};
    float dnEven_[2 * kEven] = {};
    float dnOdd_[2 * kEven] = {};
    int upPos_ = 0, dnEvenPos_ = 0, dnOddPos_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// State-variable filter (Simper's trapezoidal / TPT form). Same responses as Biquad, but it stays clean when
// the cutoff moves every sample (sweeps, envelope filters, wahs): set() is one tan(), cheap enough per sample.

class Svf
{
public:
    enum Type { Lowpass, Highpass, Bandpass, Notch, Peak, LowShelf, HighShelf, Allpass };

    // Bandpass has 0 dB at the center frequency. Peak/LowShelf/HighShelf use gainDb.
    void set(Type type, double sampleRate, float freqHz, float q = 0.7071f, float gainDb = 0.f) noexcept
    {
        const double f = clamp((double)freqHz, 5.0, sampleRate * 0.49);
        const double A = std::pow(10.0, gainDb / 40.0);
        double g = std::tan(3.14159265358979323846 * f / sampleRate);
        double k = 1.0 / std::max(0.05, (double)q);
        double m0 = 0, m1 = 0, m2 = 1;
        switch (type)
        {
        case Lowpass:   m0 = 0; m1 = 0;      m2 = 1;  break;
        case Highpass:  m0 = 1; m1 = -k;     m2 = -1; break;
        case Bandpass:  m0 = 0; m1 = k;      m2 = 0;  break;
        case Notch:     m0 = 1; m1 = -k;     m2 = 0;  break;
        case Allpass:   m0 = 1; m1 = -2 * k; m2 = 0;  break;
        case Peak:      k /= A; m0 = 1; m1 = k * (A * A - 1); m2 = 0; break;
        case LowShelf:  g /= std::sqrt(A); m0 = 1; m1 = k * (A - 1); m2 = A * A - 1; break;
        case HighShelf: g *= std::sqrt(A); m0 = A * A; m1 = k * (1 - A) * A; m2 = 1 - A * A; break;
        }
        const double a1 = 1.0 / (1.0 + g * (g + k));
        a1_ = (float)a1; a2_ = (float)(g * a1); a3_ = (float)(g * g * a1);
        m0_ = (float)m0; m1_ = (float)m1; m2_ = (float)m2;
    }

    float process(float x) noexcept
    {
        const float v3 = x - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.f * v1 - ic1_;
        ic2_ = 2.f * v2 - ic2_;
        return m0_ * x + m1_ * v1 + m2_ * v2;
    }

    void reset() noexcept { ic1_ = ic2_ = 0.f; }

private:
    float a1_ = 1.f, a2_ = 0.f, a3_ = 0.f, m0_ = 0.f, m1_ = 0.f, m2_ = 1.f;
    float ic1_ = 0.f, ic2_ = 0.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// Tilt EQ: one knob that makes the sound darker (negative dB) or brighter (positive), pivoting around
// pivotHz. Lows get -tilt/2 dB, highs +tilt/2 dB. setTilt() uses tan and pow: call it when the parameter moves.

class TiltEq
{
public:
    void setup(double sampleRate, float pivotHz = 800.f) noexcept { sampleRate_ = sampleRate; pivot_ = pivotHz; setTilt(tilt_); }
    // a gentle (Q 0.5) high shelf of the whole tilt, with the lows brought down by half of it
    void setTilt(float db) noexcept
    {
        tilt_ = db;
        shelf_.set(Svf::HighShelf, sampleRate_, pivot_, 0.5f, db);
        trim_ = dbToGain(-0.5f * db);
    }
    float process(float x) noexcept { return trim_ * shelf_.process(x); }
    void  reset() noexcept { shelf_.reset(); }

private:
    Svf shelf_;
    double sampleRate_ = 48000.0;
    float pivot_ = 800.f, tilt_ = 0.f, trim_ = 1.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// Antialiased saturation (first-order antiderivative antialiasing, "ADAA"). Drop-in replacements for
// std::tanh(x) and hardClip(x) with much less aliasing at high drive, for about the cost of the plain
// function; combine with Oversampler2x for very high gain. They delay the signal by half a sample.

class AdaaTanh
{
public:
    float process(float x) noexcept
    {
        const double d = (double)x - x1_;
        const double y = std::fabs(d) > 1e-5 ? (F((double)x) - F(x1_)) / d : std::tanh(0.5 * ((double)x + x1_));
        x1_ = x;
        return (float)y;
    }
    void reset() noexcept { x1_ = 0.0; }

private:
    // log(cosh(x)), written so it does not overflow for large |x|
    static double F(double x) noexcept { const double a = std::fabs(x); return a + std::log1p(std::exp(-2.0 * a)) - 0.69314718055994531; }
    double x1_ = 0.0;
};

class AdaaHardClip
{
public:
    float process(float x) noexcept
    {
        const double d = (double)x - x1_;
        const double y = std::fabs(d) > 1e-5 ? (F((double)x) - F(x1_)) / d : clamp(0.5 * ((double)x + x1_), -1.0, 1.0);
        x1_ = x;
        return (float)y;
    }
    void reset() noexcept { x1_ = 0.0; }

private:
    static double F(double x) noexcept { const double a = std::fabs(x); return a <= 1.0 ? 0.5 * x * x : a - 0.5; }
    double x1_ = 0.0;
};

// ---------------------------------------------------------------------------------------------------------------------
// Compressor: feed-forward, soft knee, smoothing in the dB domain (Giannoulis, Massberg & Reiss 2012).
//
//   per block:  comp.set(sampleRate, p.threshold, p.ratio, p.attack, p.release, 6.f);   // cheap
//   mono:       out[i] = comp.process(in[i]) * dbToGain(makeup);
//   stereo:     const float g = comp.gain(std::max(std::fabs(l), std::fabs(r)));  l *= g; r *= g;
//   meter:      comp.gainReductionDb()  (>= 0)

class Compressor
{
public:
    enum Detector { Peak, Rms };

    void set(double sampleRate, float thresholdDb, float ratio, float attackMs, float releaseMs, float kneeDb = 6.f,
             Detector detector = Peak) noexcept
    {
        thr_ = thresholdDb;
        slope_ = 1.f / std::max(1.f, ratio) - 1.f;
        knee_ = std::max(0.f, kneeDb);
        att_ = coeff(sampleRate, attackMs);
        rel_ = coeff(sampleRate, releaseMs);
        rmsA_ = coeff(sampleRate, 10.f);
        rms_ = detector == Rms;
    }

    // Linear gain (<= 1) for a detector sample (the input, or a sidechain / the louder of two channels).
    float gain(float detector) noexcept
    {
        float level = std::fabs(detector);
        if (rms_)
        {
            ms_ = level * level + rmsA_ * (ms_ - level * level);
            level = std::sqrt(ms_);
        }
        const float x = gainToDb(level);
        const float over = x - thr_;
        float gc;                                   // static gain change in dB (<= 0)
        if (2.f * over <= -knee_)
            gc = 0.f;
        else if (2.f * over < knee_)
            gc = slope_ * (over + 0.5f * knee_) * (over + 0.5f * knee_) / (2.f * knee_);
        else
            gc = slope_ * over;
        gs_ = gc + (gc < gs_ ? att_ : rel_) * (gs_ - gc);
        return dbToGain(gs_);
    }

    float process(float x) noexcept { return x * gain(x); }
    float gainReductionDb() const noexcept { return -gs_; }
    void  reset() noexcept { gs_ = 0.f; ms_ = 0.f; }

private:
    static float coeff(double sr, float ms) noexcept
    {
        return ms <= 0.f ? 0.f : (float)std::exp(-1.0 / (std::max(1e-6, ms * 0.001) * sr));
    }
    float thr_ = 0.f, slope_ = 0.f, knee_ = 6.f, att_ = 0.f, rel_ = 0.f, rmsA_ = 0.f;
    float gs_ = 0.f, ms_ = 0.f;
    bool  rms_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------
// Lookahead limiter: the output never exceeds the ceiling, without distortion on transients. Delays the
// signal by latency() samples: report it from Kernel::latency().
//
//   prepare():  lim.allocate(sampleRate, 5.f);          // lookahead in ms
//   process():  lim.set(p.ceiling, p.release);  then per sample  out = lim.process(x)  (mono)
//                                                             or   lim.processStereo(l, r)  (linked)

class Limiter
{
public:
    void allocate(double sampleRate, float lookaheadMs = 5.f)
    {
        sampleRate_ = sampleRate;
        n_ = std::max<uint32_t>(1, (uint32_t)std::lround(lookaheadMs * 0.001 * sampleRate));
        delay_[0].allocate(n_);
        delay_[1].allocate(n_);
        box_.assign(n_, 1.f);
        qv_.assign(n_ + 1, 1.f);
        qi_.assign(n_ + 1, 0);
        set(0.f, 100.f);
        reset();
    }

    void set(float ceilingDb, float releaseMs) noexcept
    {
        ceiling_ = dbToGain(std::min(0.f, ceilingDb));
        rel_ = releaseMs <= 0.f ? 0.f : (float)std::exp(-1.0 / (std::max(1e-6, releaseMs * 0.001) * sampleRate_));
    }

    uint32_t latency() const noexcept { return n_ - 1; }

    float process(float x) noexcept
    {
        const float g = nextGain(std::fabs(x));
        delay_[0].push(x);
        return delay_[0].read((float)(n_ - 1)) * g;
    }

    void processStereo(float& l, float& r) noexcept
    {
        const float g = nextGain(std::max(std::fabs(l), std::fabs(r)));
        delay_[0].push(l);
        delay_[1].push(r);
        l = delay_[0].read((float)(n_ - 1)) * g;
        r = delay_[1].read((float)(n_ - 1)) * g;
    }

    // Current gain reduction in dB (>= 0), for a meter.
    float gainReductionDb() const noexcept { return -gainToDb((float)(sum_ / n_)); }

    void reset() noexcept
    {
        delay_[0].clear();
        delay_[1].clear();
        std::fill(box_.begin(), box_.end(), 1.f);
        sum_ = n_;
        boxPos_ = 0;
        qHead_ = qSize_ = 0;
        t_ = 0;
        released_ = 1.f;
    }

private:
    float nextGain(float peak) noexcept
    {
        // 1. the gain this sample needs, with an exponential release
        const float need = peak > ceiling_ ? ceiling_ / peak : 1.f;
        released_ = need < released_ ? need : need + rel_ * (released_ - need);
        // 2. the minimum over the lookahead window (monotonic queue)
        const size_t cap = qv_.size();
        while (qSize_ > 0 && qv_[(qHead_ + qSize_ - 1) % cap] >= released_)
            --qSize_;
        qv_[(qHead_ + qSize_) % cap] = released_;
        qi_[(qHead_ + qSize_) % cap] = t_;
        ++qSize_;
        while (qi_[qHead_] + n_ <= t_)
            qHead_ = (qHead_ + 1) % cap, --qSize_;
        const float held = qv_[qHead_];
        ++t_;
        // 3. averaged over the same window, so the gain is down when the peak comes out of the delay
        sum_ += (double)held - box_[boxPos_];
        box_[boxPos_] = held;
        boxPos_ = (boxPos_ + 1) % n_;
        return std::min(1.f, (float)(sum_ / n_));
    }

    double sampleRate_ = 48000.0, sum_ = 1.0;
    uint32_t n_ = 1, boxPos_ = 0;
    size_t qHead_ = 0, qSize_ = 0;
    uint64_t t_ = 0;
    float ceiling_ = 1.f, rel_ = 0.f, released_ = 1.f;
    DelayLine delay_[2];
    std::vector<float> box_, qv_;
    std::vector<uint64_t> qi_;
};

// ---------------------------------------------------------------------------------------------------------------------
// Noise gate with hysteresis and hold: mutes the signal while it stays under the threshold.
//
//   per block:  gate.set(sampleRate, p.threshold, 1.f, 50.f, 100.f);   // attack, hold, release in ms
//   mono:       out = gate.process(x);     stereo: g = gate.gain(std::max(|l|, |r|)); l *= g; r *= g;

class NoiseGate
{
public:
    void set(double sampleRate, float thresholdDb, float attackMs, float holdMs, float releaseMs,
             float rangeDb = -80.f, float hysteresisDb = 4.f) noexcept
    {
        open_ = dbToGain(thresholdDb);
        close_ = dbToGain(thresholdDb - std::max(0.f, hysteresisDb));
        floor_ = dbToGain(std::min(0.f, rangeDb));
        hold_ = (uint32_t)std::max(0.0, holdMs * 0.001 * sampleRate);
        att_ = coeff(sampleRate, attackMs);
        rel_ = coeff(sampleRate, releaseMs);
        env_.setup(sampleRate, 0.1f, 20.f);
    }

    float gain(float detector) noexcept
    {
        const float e = env_.process(detector);
        if (e >= open_)
            isOpen_ = true, holdLeft_ = hold_;
        else if (e < close_ && isOpen_)
        {
            if (holdLeft_ > 0)
                --holdLeft_;
            else
                isOpen_ = false;
        }
        const float target = isOpen_ ? 1.f : floor_;
        g_ = target + (target > g_ ? att_ : rel_) * (g_ - target);
        return g_;
    }

    float process(float x) noexcept { return x * gain(x); }
    bool  isOpen() const noexcept { return isOpen_; }
    void  reset() noexcept { env_.reset(); g_ = 0.f; isOpen_ = false; holdLeft_ = 0; }

private:
    static float coeff(double sr, float ms) noexcept
    {
        return ms <= 0.f ? 0.f : (float)std::exp(-1.0 / (std::max(1e-6, ms * 0.001) * sr));
    }
    EnvelopeFollower env_;
    float open_ = 0.f, close_ = 0.f, floor_ = 0.f, att_ = 0.f, rel_ = 0.f, g_ = 0.f;
    uint32_t hold_ = 0, holdLeft_ = 0;
    bool isOpen_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------
// Pitch shifter (two crossfaded delay taps): ratio 2 = an octave up, 0.5 = an octave down. A classic
// "digital" shifter, best for doubling, harmonies and shimmer; very low or high ratios warble. Mono.
//
//   prepare():  ps.allocate(sampleRate);        process():  ps.setRatio(semitonesToRatio(p.shift[i]));  y = ps.process(x);

inline float semitonesToRatio(float semitones) noexcept { return std::pow(2.f, semitones / 12.f); }

class PitchShifter
{
public:
    void allocate(double sampleRate, float windowMs = 50.f)
    {
        window_ = std::max(16.f, (float)(windowMs * 0.001 * sampleRate));
        delay_.allocate((size_t)window_ + 8);
        reset();
    }
    void setRatio(float ratio) noexcept { step_ = (1.f - clamp(ratio, 0.25f, 4.f)) / window_; }
    float process(float x) noexcept
    {
        delay_.push(x);
        phase_ += step_;
        phase_ -= std::floor(phase_);
        float p2 = phase_ + 0.5f;
        if (p2 >= 1.f)
            p2 -= 1.f;
        // sin^2 windows that sum to 1: each tap fades out as it wraps
        const float w1 = std::sin(kPi * phase_), w2 = std::sin(kPi * p2);
        return w1 * w1 * delay_.readHermite(1.f + phase_ * window_) + w2 * w2 * delay_.readHermite(1.f + p2 * window_);
    }
    void reset() noexcept { delay_.clear(); phase_ = 0.f; }

private:
    DelayLine delay_;
    float window_ = 2400.f, phase_ = 0.f, step_ = 0.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// Stereo reverb (Freeverb by Jezar at Dreampoint, public domain): 8 damped combs and 4 allpasses per side,
// tuned for 44.1 kHz and scaled to the sample rate. Returns the wet signal only: mix it with the dry.
//
//   prepare():  rev.allocate(sampleRate);       process():  rev.set(p.size, p.damping, p.width);
//                                                            rev.process(inL, inR, wetL, wetR);

class Reverb
{
public:
    void allocate(double sampleRate)
    {
        static const int kComb[kCombs] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static const int kAllpass[kAllpasses] = { 556, 441, 341, 225 };
        const double k = sampleRate / 44100.0;
        for (int side = 0; side < 2; ++side)
        {
            const int spread = side ? 23 : 0;
            for (int i = 0; i < kCombs; ++i)
                comb_[side][i].buf.assign(std::max<size_t>(1, (size_t)((kComb[i] + spread) * k)), 0.f);
            for (int i = 0; i < kAllpasses; ++i)
                allpass_[side][i].buf.assign(std::max<size_t>(1, (size_t)((kAllpass[i] + spread) * k)), 0.f);
        }
        reset();
    }

    // size, damping, width: 0..1. size 1 is a long hall (about 4 s), 0 a small room.
    void set(float size, float damping, float width) noexcept
    {
        feedback_ = 0.7f + 0.28f * clamp(size, 0.f, 1.f);
        damp_ = 0.4f * clamp(damping, 0.f, 1.f);
        const float w = clamp(width, 0.f, 1.f);
        wet1_ = 0.5f * w + 0.5f;
        wet2_ = 0.5f * (1.f - w);
    }

    void process(float inL, float inR, float& outL, float& outR) noexcept
    {
        const float in = (inL + inR) * 0.015f;
        float acc[2] = { 0.f, 0.f };
        for (int side = 0; side < 2; ++side)
        {
            for (int i = 0; i < kCombs; ++i)
            {
                Line& c = comb_[side][i];
                const float y = c.buf[c.pos];
                c.store = y * (1.f - damp_) + c.store * damp_;
                if (std::fabs(c.store) < 1e-15f)
                    c.store = 0.f; // no denormals in the decaying tail
                c.buf[c.pos] = in + c.store * feedback_;
                if (++c.pos >= c.buf.size())
                    c.pos = 0;
                acc[side] += y;
            }
            for (int i = 0; i < kAllpasses; ++i)
            {
                Line& a = allpass_[side][i];
                const float b = a.buf[a.pos];
                a.buf[a.pos] = acc[side] + b * 0.5f;
                if (++a.pos >= a.buf.size())
                    a.pos = 0;
                acc[side] = b - acc[side];
            }
        }
        outL = acc[0] * wet1_ + acc[1] * wet2_;
        outR = acc[1] * wet1_ + acc[0] * wet2_;
    }

    void reset() noexcept
    {
        for (auto& side : comb_)
            for (Line& l : side)
                std::fill(l.buf.begin(), l.buf.end(), 0.f), l.pos = 0, l.store = 0.f;
        for (auto& side : allpass_)
            for (Line& l : side)
                std::fill(l.buf.begin(), l.buf.end(), 0.f), l.pos = 0;
    }

private:
    static constexpr int kCombs = 8, kAllpasses = 4;
    struct Line
    {
        std::vector<float> buf = std::vector<float>(1, 0.f);
        size_t pos = 0;
        float store = 0.f;
    };
    Line comb_[2][kCombs], allpass_[2][kAllpasses];
    float feedback_ = 0.84f, damp_ = 0.2f, wet1_ = 1.f, wet2_ = 0.f;
};

// ---------------------------------------------------------------------------------------------------------------------
// White noise in [-1, 1) (xorshift32): for hiss, dither, noise sources. Each instance has its own sequence.

class Noise
{
public:
    explicit Noise(uint32_t seed = 22222u) noexcept : seed_(seed ? seed : 1u), state_(seed_) {}
    void reset() noexcept { state_ = seed_; } // the same sequence again
    float next() noexcept
    {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return (float)(int32_t)state_ * (1.f / 2147483648.f);
    }

private:
    uint32_t seed_, state_;
};

} // namespace cookbook
