// Plugin Cookbook — format-independent processing engine.
// SPDX-License-Identifier: MIT
//
// The Engine owns a recipe Kernel and does everything that is the same for every plugin:
//   - parameter clamping and per-sample smoothing (linear or logarithmic domain)
//   - splitting host buffers into chunks no larger than the prepared block size
//   - copying inputs to scratch so kernels never see aliased in/out buffers
//   - flushing denormals (FTZ/DAZ) during processing
//   - an output safety net (non-finite samples are muted, runaway levels are clamped)
//   - optional "poison" instrumentation used by the offline test harness
//
// It is used verbatim by the DPF plugin wrapper and by the test harness, so what the
// harness measures is exactly what runs inside the DAW.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "cookbook/ParamInfo.hpp"

#if defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
# include <xmmintrin.h>
# define COOKBOOK_X86_FTZ 1
#endif

namespace cookbook {

// ---------------------------------------------------------------------------------------------------------------------
// Denormal protection

class ScopedFlushDenormals
{
public:
    ScopedFlushDenormals() noexcept
    {
#if defined(COOKBOOK_X86_FTZ)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040); // FTZ | DAZ
#elif defined(__aarch64__)
        uint64_t fpcr;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
        saved_ = fpcr;
        fpcr |= (1ull << 24); // FZ
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
    }
    ~ScopedFlushDenormals() noexcept
    {
#if defined(COOKBOOK_X86_FTZ)
        _mm_setcsr((unsigned)saved_);
#elif defined(__aarch64__)
        const uint64_t fpcr = saved_;
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
    }

private:
    uint64_t saved_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// Kernel interface detection (gives readable compile errors for malformed kernels)

namespace detail {

template <class K, class = void> struct HasPrepare : std::false_type {};
template <class K>
struct HasPrepare<K, decltype(std::declval<K&>().prepare(double{}, uint32_t{}), void())> : std::true_type {};

template <class K, class = void> struct HasReset : std::false_type {};
template <class K>
struct HasReset<K, decltype(std::declval<K&>().reset(), void())> : std::true_type {};

template <class K, class P, class = void> struct HasProcess : std::false_type {};
template <class K, class P>
struct HasProcess<K, P, decltype(std::declval<K&>().process(std::declval<const float* const*>(),
                                                             std::declval<float* const*>(),
                                                             uint32_t{},
                                                             std::declval<const P&>()), void())> : std::true_type {};

template <class K, class = void> struct HasLatency : std::false_type {};
template <class K>
struct HasLatency<K, decltype((void)static_cast<uint32_t>(std::declval<const K&>().latency()), void())> : std::true_type {};

} // namespace detail

template <class Kernel, class Meta>
constexpr bool checkKernel()
{
    static_assert(std::is_default_constructible<Kernel>::value,
                  "Kernel must be default-constructible (do allocations in prepare(), not in a constructor with arguments)");
    static_assert(detail::HasPrepare<Kernel>::value,
                  "Kernel is missing: void prepare(double sampleRate, uint32_t maxBlockSize)");
    static_assert(detail::HasReset<Kernel>::value,
                  "Kernel is missing: void reset()");
    static_assert(detail::HasProcess<Kernel, typename Meta::Params>::value,
                  "Kernel is missing: void process(const float* const* in, float* const* out, uint32_t frames, const Params& p)");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Engine

struct EngineOptions
{
    bool safety = true;            // mute non-finite output, clamp to +/- kSafetyLimit
    // Test harness instrumentation (never enabled in plugins):
    bool poisonExtraInput = false; // in[numInputs] points at NaNs: reading it makes the output NaN
    bool poisonOutputs = false;    // outputs are pre-filled with NaN; out[numOutputs] is a canary buffer
};

struct PoisonReport
{
    bool wroteExtraOutput = false; // kernel wrote out[numOutputs] (more outputs than declared)
    bool unwrittenOutput = false;  // some declared output sample was left untouched
};

template <class Kernel, class Meta>
class Engine
{
public:
    static constexpr uint32_t kNumInputs = Meta::kNumInputs;
    static constexpr uint32_t kNumOutputs = Meta::kNumOutputs;
    static constexpr uint32_t kNumParams = Meta::kNumParams;
    static constexpr float kSafetyLimit = 16.f; // +24 dBFS: no legitimate effect output gets here

    using Params = typename Meta::Params;

    explicit Engine(EngineOptions opts = {}) : opts_(opts)
    {
        static_assert(checkKernel<Kernel, Meta>(), "");
        for (uint32_t i = 0; i < kNumParams; ++i)
        {
            targets_[i] = Meta::kParams[i].def;
            state_[i] = toSmoothDomain(i, targets_[i]);
            coeff_[i] = 0.f;
        }
    }

    // Non-realtime. Called whenever sample rate or maximum block size changes.
    void prepare(double sampleRate, uint32_t maxBlockSize)
    {
        sampleRate_ = sampleRate;
        maxBlock_ = std::max<uint32_t>(1, maxBlockSize);

        uint32_t numFloat = 0;
        for (uint32_t i = 0; i < kNumParams; ++i)
            if (Meta::kParams[i].type == ParamType::Float)
                ++numFloat;

        paramBuf_.assign((size_t)std::max<uint32_t>(1, numFloat) * maxBlock_, 0.f);
        inScratch_.assign((size_t)(kNumInputs + 1) * maxBlock_, 0.f);
        outCanary_.assign(maxBlock_, 0.f);

        uint32_t slot = 0;
        for (uint32_t i = 0; i < kNumParams; ++i)
        {
            const ParamInfo& p = Meta::kParams[i];
            if (p.type == ParamType::Float)
            {
                floatSlot_[i] = (int)slot;
                floatPtrs_[slot] = paramBuf_.data() + (size_t)slot * maxBlock_;
                ++slot;
                const double tau = std::max(1e-6, p.smoothingMs * 0.001 / 4.6); // ~99% settle in smoothingMs
                coeff_[i] = p.smoothingMs <= 0.f ? 0.f : (float)std::exp(-1.0 / (tau * sampleRate));
            }
            else
            {
                floatSlot_[i] = -1;
                coeff_[i] = 0.f;
            }
            state_[i] = toSmoothDomain(i, targets_[i]);
        }
        for (; slot < kMaxSlots; ++slot)
            floatPtrs_[slot] = paramBuf_.data();

        for (uint32_t c = 0; c <= kNumInputs; ++c)
            inPtrs_[c] = inScratch_.data() + (size_t)c * maxBlock_;
        // the extra input slot only exists to catch kernels that read too many channels
        std::fill(inScratch_.begin() + (size_t)kNumInputs * maxBlock_, inScratch_.end(),
                  opts_.poisonExtraInput ? std::numeric_limits<float>::quiet_NaN() : 0.f);

        kernel_.prepare(sampleRate, maxBlock_);
        kernel_.reset();
        prepared_ = true;
    }

    // Realtime-safe. Clears all DSP state (delay lines, filters) and snaps parameter smoothing.
    void reset() noexcept
    {
        for (uint32_t i = 0; i < kNumParams; ++i)
            state_[i] = toSmoothDomain(i, targets_[i]);
        if (prepared_)
            kernel_.reset();
    }

    void setParameter(uint32_t index, float value) noexcept
    {
        if (index >= kNumParams)
            return;
        const ParamInfo& p = Meta::kParams[index];
        if (!(value == value)) // NaN from a misbehaving host
            value = p.def;
        value = std::min(std::max(value, p.min), p.max);
        if (p.type != ParamType::Float)
            value = std::round(value);
        targets_[index] = value;
    }

    float getParameter(uint32_t index) const noexcept { return index < kNumParams ? targets_[index] : 0.f; }

    uint32_t latency() const noexcept { return latencyImpl(detail::HasLatency<Kernel>{}); }

    double sampleRate() const noexcept { return sampleRate_; }
    uint32_t maxBlockSize() const noexcept { return maxBlock_; }
    Kernel& kernel() noexcept { return kernel_; }
    const PoisonReport& poisonReport() const noexcept { return poisonReport_; }
    uint32_t safetyTrips() const noexcept { return safetyTrips_; }

    // Realtime-safe. inputs/outputs are host buffers with `frames` samples per channel; they may alias.
    void process(const float* const* inputs, float* const* outputs, uint32_t frames) noexcept
    {
        if (!prepared_)
        {
            for (uint32_t c = 0; c < kNumOutputs; ++c)
                std::memset(outputs[c], 0, sizeof(float) * frames);
            return;
        }

        ScopedFlushDenormals ftz;

        uint32_t offset = 0;
        while (offset < frames)
        {
            const uint32_t n = std::min(frames - offset, maxBlock_);

            for (uint32_t c = 0; c < kNumInputs; ++c)
                std::memcpy(const_cast<float*>(inPtrs_[c]), inputs[c] + offset, sizeof(float) * n);

            renderParameters(n);

            float discrete[kNumParams > 0 ? kNumParams : 1];
            for (uint32_t i = 0; i < kNumParams; ++i)
                discrete[i] = targets_[i];

            const Params params = Meta::bind(floatPtrs_, discrete);

            float* outPtrs[kNumOutputs + 1];
            for (uint32_t c = 0; c < kNumOutputs; ++c)
                outPtrs[c] = outputs[c] + offset;
            outPtrs[kNumOutputs] = outCanary_.data();

            if (opts_.poisonOutputs)
            {
                for (uint32_t c = 0; c < kNumOutputs; ++c)
                    std::fill(outPtrs[c], outPtrs[c] + n, std::numeric_limits<float>::quiet_NaN());
                std::fill(outCanary_.begin(), outCanary_.begin() + n, kCanary);
            }

            kernel_.process(inPtrs_, outPtrs, n, params);

            if (opts_.poisonOutputs)
                checkPoison(outPtrs, n);

            if (opts_.safety)
                applySafety(outPtrs, n);

            offset += n;
        }
    }

private:
    static constexpr uint32_t kMaxSlots = kNumParams > 0 ? kNumParams : 1;
    static constexpr float kCanary = 12345.678f;

    float toSmoothDomain(uint32_t i, float v) const noexcept
    {
        return Meta::kParams[i].scale == Scale::Log ? std::log(std::max(v, 1e-9f)) : v;
    }
    float fromSmoothDomain(uint32_t i, float v) const noexcept
    {
        return Meta::kParams[i].scale == Scale::Log ? std::exp(v) : v;
    }

    void renderParameters(uint32_t n) noexcept
    {
        for (uint32_t i = 0; i < kNumParams; ++i)
        {
            const int slot = floatSlot_[i];
            if (slot < 0)
                continue;
            float* buf = const_cast<float*>(floatPtrs_[slot]);
            const float target = toSmoothDomain(i, targets_[i]);
            const float a = coeff_[i];
            float s = state_[i];

            const ParamInfo& p = Meta::kParams[i];
            const float eps = Meta::kParams[i].scale == Scale::Log ? 1e-5f : (p.max - p.min) * 1e-6f;

            if (a == 0.f || std::fabs(s - target) <= eps)
            {
                s = target;
                const float v = targets_[i];
                std::fill(buf, buf + n, v);
            }
            else
            {
                for (uint32_t k = 0; k < n; ++k)
                {
                    s = target + a * (s - target);
                    buf[k] = std::min(std::max(fromSmoothDomain(i, s), p.min), p.max);
                }
            }
            state_[i] = s;
        }
    }

    void checkPoison(float* const* outPtrs, uint32_t n) noexcept
    {
        for (uint32_t c = 0; c < kNumOutputs; ++c)
            for (uint32_t k = 0; k < n; ++k)
                if (outPtrs[c][k] != outPtrs[c][k])
                {
                    poisonReport_.unwrittenOutput = true;
                    break;
                }
        for (uint32_t k = 0; k < n; ++k)
            if (outCanary_[k] != kCanary)
            {
                poisonReport_.wroteExtraOutput = true;
                break;
            }
    }

    void applySafety(float* const* outPtrs, uint32_t n) noexcept
    {
        bool bad = false;
        for (uint32_t c = 0; c < kNumOutputs; ++c)
        {
            float* o = outPtrs[c];
            for (uint32_t k = 0; k < n; ++k)
            {
                const float v = o[k];
                if (!(std::fabs(v) <= kSafetyLimit)) // catches NaN, Inf and runaway levels
                {
                    if (v != v || std::isinf(v))
                        bad = true;
                    else
                        o[k] = v > 0.f ? kSafetyLimit : -kSafetyLimit;
                }
            }
        }
        if (bad)
        {
            ++safetyTrips_;
            for (uint32_t c = 0; c < kNumOutputs; ++c)
                std::memset(outPtrs[c], 0, sizeof(float) * n);
            kernel_.reset(); // recover from a blown-up state
        }
    }

    uint32_t latencyImpl(std::true_type) const noexcept { return static_cast<uint32_t>(kernel_.latency()); }
    uint32_t latencyImpl(std::false_type) const noexcept { return 0; }

    EngineOptions opts_;
    Kernel kernel_;
    bool prepared_ = false;
    double sampleRate_ = 48000.0;
    uint32_t maxBlock_ = 0;

    float targets_[kMaxSlots] = {};
    float state_[kMaxSlots] = {};
    float coeff_[kMaxSlots] = {};
    int floatSlot_[kMaxSlots] = {};
    const float* floatPtrs_[kMaxSlots] = {};
    std::vector<float> paramBuf_;

    std::vector<float> inScratch_;
    const float* inPtrs_[kNumInputs + 1] = {};
    std::vector<float> outCanary_;

    PoisonReport poisonReport_;
    uint32_t safetyTrips_ = 0;
};

} // namespace cookbook
