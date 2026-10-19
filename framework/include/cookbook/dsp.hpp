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

} // namespace cookbook
