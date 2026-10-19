// Plugin Cookbook — offline test harness.
// SPDX-License-Identifier: MIT
//
// Built by `tools/recipe.py test|render` against one recipe's generated headers. It drives the exact
// same cookbook::Engine the plugins use, so results reflect what runs inside a DAW.
//
//   harness test [--quick]                          run the test suite, one JSON object per line on stdout
//   harness render IN.wav OUT.wav [--set sym=v] [--block N]

#include "RecipeKernel.hpp"
#include "cookbook/Engine.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
# include <malloc.h>
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Allocation tracking: counts heap allocations while gTrack is set (i.e. inside Engine::process).

static std::atomic<bool> gTrack{false};
static std::atomic<long> gAllocs{0};

#ifndef COOKBOOK_NO_ALLOC_HOOK

static void* trackedAlloc(std::size_t n)
{
    if (gTrack.load(std::memory_order_relaxed))
        gAllocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p)
        throw std::bad_alloc();
    return p;
}

static void* trackedAlignedAlloc(std::size_t n, std::size_t align)
{
    if (gTrack.load(std::memory_order_relaxed))
        gAllocs.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    void* p = _aligned_malloc(n ? n : 1, align);
#else
    void* p = nullptr;
    if (posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, n ? n : 1) != 0)
        p = nullptr;
#endif
    if (!p)
        throw std::bad_alloc();
    return p;
}

static void alignedFree(void* p)
{
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void* operator new(std::size_t n) { return trackedAlloc(n); }
void* operator new[](std::size_t n) { return trackedAlloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return trackedAlloc(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return trackedAlloc(n); } catch (...) { return nullptr; } }
void* operator new(std::size_t n, std::align_val_t a) { return trackedAlignedAlloc(n, (std::size_t)a); }
void* operator new[](std::size_t n, std::align_val_t a) { return trackedAlignedAlloc(n, (std::size_t)a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { alignedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { alignedFree(p); }

# if defined(__GLIBC__)
// On glibc we can also catch plain malloc/calloc/realloc (e.g. from C code or std:: internals).
extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* malloc(std::size_t n)
{
    if (gTrack.load(std::memory_order_relaxed))
        gAllocs.fetch_add(1, std::memory_order_relaxed);
    return __libc_malloc(n);
}
void* calloc(std::size_t a, std::size_t b)
{
    if (gTrack.load(std::memory_order_relaxed))
        gAllocs.fetch_add(1, std::memory_order_relaxed);
    return __libc_calloc(a, b);
}
void* realloc(void* p, std::size_t n)
{
    if (gTrack.load(std::memory_order_relaxed))
        gAllocs.fetch_add(1, std::memory_order_relaxed);
    return __libc_realloc(p, n);
}
}
# endif
#endif // COOKBOOK_NO_ALLOC_HOOK

// ---------------------------------------------------------------------------------------------------------------------

using Meta = recipe::Meta;
using Kernel = recipe::Kernel;
using Engine = cookbook::Engine<Kernel, Meta>;
using cookbook::ParamInfo;
using cookbook::ParamType;

constexpr uint32_t NI = Meta::kNumInputs;
constexpr uint32_t NO = Meta::kNumOutputs;
constexpr uint32_t NP = Meta::kNumParams;

using Buffer = std::vector<std::vector<float>>; // [channel][sample]

static bool gFailed = false;

static std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

static void report(const char* test, const char* status, const std::string& detail)
{
    if (std::strcmp(status, "fail") == 0)
        gFailed = true;
    std::printf("{\"test\": \"%s\", \"status\": \"%s\", \"detail\": \"%s\"}\n", test, status, jsonEscape(detail).c_str());
    std::fflush(stdout);
}

template <class... Args>
static std::string fmt(const char* f, Args... args)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), f, args...);
    return buf;
}

static double toDb(double v) { return 20.0 * std::log10(std::max(v, 1e-12)); }

// ---------------------------------------------------------------------------------------------------------------------
// Signals

static Buffer makeSilence(uint32_t channels, size_t n) { return Buffer(channels, std::vector<float>(n, 0.f)); }

static Buffer makeSine(uint32_t channels, size_t n, double sr, double hz, float amp)
{
    Buffer b = makeSilence(channels, n);
    for (uint32_t c = 0; c < channels; ++c)
        for (size_t i = 0; i < n; ++i)
            b[c][i] = amp * (float)std::sin(2.0 * 3.14159265358979323846 * hz * (double)i / sr);
    return b;
}

static Buffer makeNoise(uint32_t channels, size_t n, float amp, uint32_t seed)
{
    Buffer b = makeSilence(channels, n);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.f, 1.f);
    for (uint32_t c = 0; c < channels; ++c)
        for (size_t i = 0; i < n; ++i)
            b[c][i] = amp * d(rng);
    return b;
}

static Buffer makeImpulse(uint32_t channels, size_t n)
{
    Buffer b = makeSilence(channels, n);
    for (uint32_t c = 0; c < channels; ++c)
        b[c][0] = 1.f;
    return b;
}

// ---------------------------------------------------------------------------------------------------------------------
// Parameter sets

using ParamSet = std::vector<float>;

static ParamSet paramsDefault()
{
    ParamSet s(NP);
    for (uint32_t i = 0; i < NP; ++i)
        s[i] = Meta::kParams[i].def;
    return s;
}
static ParamSet paramsMin()
{
    ParamSet s(NP);
    for (uint32_t i = 0; i < NP; ++i)
        s[i] = Meta::kParams[i].min;
    return s;
}
static ParamSet paramsMax()
{
    ParamSet s(NP);
    for (uint32_t i = 0; i < NP; ++i)
        s[i] = Meta::kParams[i].max;
    return s;
}
static ParamSet paramsRandom(std::mt19937& rng)
{
    ParamSet s(NP);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    for (uint32_t i = 0; i < NP; ++i)
    {
        const ParamInfo& p = Meta::kParams[i];
        float t = u(rng);
        if (p.scale == cookbook::Scale::Log && p.min > 0.f)
            s[i] = p.min * std::pow(p.max / p.min, t);
        else
            s[i] = p.min + t * (p.max - p.min);
        if (p.type != ParamType::Float)
            s[i] = std::round(s[i]);
    }
    return s;
}

static std::string describe(const ParamSet& s)
{
    std::string o;
    for (uint32_t i = 0; i < NP; ++i)
        o += fmt("%s%s=%g", i ? " " : "", Meta::kParams[i].symbol, s[i]);
    return o.empty() ? "(no parameters)" : o;
}

static void applyParams(Engine& e, const ParamSet& s)
{
    for (uint32_t i = 0; i < NP; ++i)
        e.setParameter(i, s[i]);
}

// ---------------------------------------------------------------------------------------------------------------------
// Running

struct RunStats
{
    double peak = 0.0;
    bool finite = true;
    long allocs = 0;
    double sumSq = 0.0;
    double sum = 0.0;
    size_t count = 0;
};

// Processes `in` through `e` in blocks; blockFn(i) returns the size of the i-th block.
// onBlock (optional) is called before every block (used for automation).
template <class BlockFn, class OnBlock>
static Buffer run(Engine& e, const Buffer& in, BlockFn blockFn, OnBlock onBlock, RunStats* stats = nullptr)
{
    const size_t n = in.empty() ? 0 : in[0].size();
    Buffer out = makeSilence(NO, n);
    const float* ip[NI];
    float* op[NO];
    size_t pos = 0, block = 0;
    while (pos < n)
    {
        const uint32_t bs = (uint32_t)std::min<size_t>(blockFn(block++), n - pos);
        onBlock();
        for (uint32_t c = 0; c < NI; ++c)
            ip[c] = in[c].data() + pos;
        for (uint32_t c = 0; c < NO; ++c)
            op[c] = out[c].data() + pos;
        const long before = gAllocs.load();
        gTrack = true;
        e.process(ip, op, bs);
        gTrack = false;
        if (stats)
            stats->allocs += gAllocs.load() - before;
        pos += bs;
    }
    if (stats)
    {
        for (uint32_t c = 0; c < NO; ++c)
            for (float v : out[c])
            {
                if (!std::isfinite(v))
                    stats->finite = false;
                else
                {
                    stats->peak = std::max(stats->peak, (double)std::fabs(v));
                    stats->sumSq += (double)v * v;
                    stats->sum += v;
                }
                ++stats->count;
            }
    }
    return out;
}

static Buffer runFixed(Engine& e, const Buffer& in, uint32_t block, RunStats* stats = nullptr)
{
    return run(e, in, [block](size_t) { return block; }, [] {}, stats);
}

static std::unique_ptr<Engine> makeEngine(double sr, uint32_t maxBlock, cookbook::EngineOptions opts = {false})
{
    std::unique_ptr<Engine> e(new Engine(opts));
    e->prepare(sr, maxBlock);
    return e;
}

static double rmsTail(const Buffer& b, size_t tailSamples)
{
    double s = 0.0;
    size_t cnt = 0;
    for (const auto& ch : b)
    {
        const size_t start = ch.size() > tailSamples ? ch.size() - tailSamples : 0;
        for (size_t i = start; i < ch.size(); ++i)
        {
            s += (double)ch[i] * ch[i];
            ++cnt;
        }
    }
    return cnt ? std::sqrt(s / cnt) : 0.0;
}

static double maxDiff(const Buffer& a, const Buffer& b)
{
    double d = 0.0;
    for (size_t c = 0; c < a.size(); ++c)
        for (size_t i = 0; i < a[c].size(); ++i)
        {
            const double x = std::fabs((double)a[c][i] - b[c][i]);
            if (!(x <= d))
                d = std::isfinite(x) ? x : 1e9;
        }
    return d;
}

// ---------------------------------------------------------------------------------------------------------------------
// Tests

static void testInfo()
{
    auto e = makeEngine(48000, 512);
    report("interface", "pass", fmt("%u in / %u out, %u parameters, latency %u samples",
                                    NI, NO, NP, e->latency()));
}

static void testChannels()
{
    const double sr = 48000;
    const Buffer noise = makeNoise(NI, 9600, 0.25f, 1);

    // Clean reference
    RunStats clean;
    {
        auto e = makeEngine(sr, 512);
        runFixed(*e, noise, 512, &clean);
    }

    // Extra input slot holds NaN: a NaN output now (and not before) means the kernel read in[NI].
    {
        cookbook::EngineOptions o; o.safety = false; o.poisonExtraInput = true;
        auto e = makeEngine(sr, 512, o);
        RunStats s;
        runFixed(*e, noise, 512, &s);
        if (clean.finite && !s.finite)
            report("channels", "fail", fmt("kernel reads in[%u], but the plugin only has %u input%s", NI, NI, NI > 1 ? "s" : ""));
    }

    // Outputs pre-filled with NaN, extra output is a canary.
    {
        cookbook::EngineOptions o; o.safety = false; o.poisonOutputs = true;
        auto e = makeEngine(sr, 512, o);
        RunStats s;
        runFixed(*e, noise, 512, &s);
        const cookbook::PoisonReport& r = e->poisonReport();
        if (r.wroteExtraOutput)
            report("channels", "fail", fmt("kernel writes out[%u], but the plugin only has %u output%s", NO, NO, NO > 1 ? "s" : ""));
        else if (r.unwrittenOutput && clean.finite)
            report("channels", "fail", fmt("some output samples are never written: every out[c][i] for c < %u and i < frames must be assigned", NO));
        else
            report("channels", "pass", fmt("reads only in[0..%u], writes every sample of out[0..%u]", NI - 1, NO - 1));
    }
}

static void testStability(bool quick, long& totalAllocs)
{
    std::vector<double> rates = quick ? std::vector<double>{48000} : std::vector<double>{44100, 48000, 96000, 192000};
    std::mt19937 rng(1234);
    std::vector<std::pair<std::string, ParamSet>> sets = {
        {"defaults", paramsDefault()}, {"all-min", paramsMin()}, {"all-max", paramsMax()}};
    const int nRandom = quick ? 3 : 8;
    for (int i = 0; i < nRandom; ++i)
        sets.push_back({fmt("random#%d", i + 1), paramsRandom(rng)});

    double worstPeak = 0.0;
    std::string worstWhere;
    std::string firstNonFinite;
    int runs = 0;

    for (double sr : rates)
    {
        const size_t n = (size_t)(sr * (quick ? 0.5 : 1.0));
        const std::vector<std::pair<std::string, Buffer>> signals = {
            {"sine 220 Hz -6 dBFS", makeSine(NI, n, sr, 220.0, 0.5f)},
            {"noise -12 dBFS", makeNoise(NI, n, 0.25f, 7)},
            {"impulse", makeImpulse(NI, n)},
            {"full-scale sine 1 kHz", makeSine(NI, n, sr, 1000.0, 1.0f)},
        };
        for (const auto& set : sets)
            for (const auto& sig : signals)
            {
                auto e = makeEngine(sr, 512);
                applyParams(*e, set.second);
                e->reset();
                RunStats s;
                runFixed(*e, sig.second, 512, &s);
                ++runs;
                totalAllocs += s.allocs;
                const std::string where = fmt("%s, %s, %g Hz", set.first.c_str(), sig.first.c_str(), sr);
                if (!s.finite && firstNonFinite.empty())
                    firstNonFinite = where + " [" + describe(set.second) + "]";
                if (s.peak > worstPeak)
                {
                    worstPeak = s.peak;
                    worstWhere = where;
                }
            }
    }

    if (!firstNonFinite.empty())
        report("stability", "fail", "NaN/Inf output with " + firstNonFinite);
    else if (worstPeak > Engine::kSafetyLimit)
        report("stability", "fail", fmt("output reaches %+.1f dBFS (%s) — the plugin's safety clamp would engage; reduce gain/feedback",
                                        toDb(worstPeak), worstWhere.c_str()));
    else if (worstPeak > 4.0)
        report("stability", "warn", fmt("output reaches %+.1f dBFS (%s) — louder than +12 dBFS, check gain staging",
                                        toDb(worstPeak), worstWhere.c_str()));
    else
        report("stability", "pass", fmt("%d runs finite, max peak %+.1f dBFS (%s)", runs, toDb(worstPeak), worstWhere.c_str()));
}

static void testAutomation(long& totalAllocs)
{
    const double sr = 48000;
    const Buffer noise = makeNoise(NI, (size_t)(sr * 3), 0.25f, 11);
    std::mt19937 rng(99);
    std::uniform_int_distribution<int> bs(1, 512);
    auto e = makeEngine(sr, 512);
    RunStats s;
    run(*e, noise, [&](size_t) { return (uint32_t)bs(rng); }, [&] { applyParams(*e, paramsRandom(rng)); }, &s);
    totalAllocs += s.allocs;
    if (!s.finite)
        report("automation", "fail", "NaN/Inf output while parameters jump every block (random block sizes 1..512)");
    else if (s.peak > Engine::kSafetyLimit)
        report("automation", "fail", fmt("output reaches %+.1f dBFS under fast automation", toDb(s.peak)));
    else
        report("automation", s.peak > 4.0 ? "warn" : "pass",
               fmt("random parameter jumps every block, block sizes 1..512: peak %+.1f dBFS", toDb(s.peak)));
}

static void testSilence()
{
    const double sr = 48000;
    auto e = makeEngine(sr, 512);
    RunStats s;
    runFixed(*e, makeSilence(NI, (size_t)(sr * 2)), 512, &s);
    if (!s.finite)
        report("silence", "fail", "NaN/Inf with silent input");
    else if (s.peak > 1e-4)
        report("silence", "warn", fmt("outputs %+.1f dBFS with silent input at default settings — fine for noise/oscillators, "
                                      "otherwise look for DC offset or self-oscillation", toDb(s.peak)));
    else
        report("silence", "pass", "silent input gives silent output");
}

static void testTail(bool quick)
{
    const double sr = 48000;
    const double seconds = quick ? 8.0 : 20.0;
    const std::vector<std::pair<std::string, ParamSet>> sets = {{"defaults", paramsDefault()}, {"all-max", paramsMax()}};
    for (const auto& set : sets)
    {
        auto e = makeEngine(sr, 512);
        applyParams(*e, set.second);
        e->reset();
        RunStats s;
        const size_t total = (size_t)(sr * seconds);
        const Buffer out = runFixed(*e, makeImpulse(NI, total), 512, &s);
        // energy in the second quarter vs the last quarter (windows long enough to contain periodic echoes):
        // long tails are fine as long as they decay
        Buffer firstHalf = out;
        for (auto& ch : firstHalf)
            ch.resize(total / 2);
        const double early = rmsTail(firstHalf, total / 4);
        const double late = rmsTail(out, total / 4);
        const char* test = set.first == "defaults" ? "tail" : "tail (all-max)";
        if (!s.finite)
            report(test, "fail", "NaN/Inf after an impulse");
        else if (late <= 1e-4) // -80 dB
            report(test, "pass", fmt("impulse response decays below -80 dB within %.0f s (%s)", seconds, set.first.c_str()));
        else if (late < early * 0.89) // at least 1 dB quieter
            report(test, "pass", fmt("long tail, but decaying: %.1f dB lower in the last quarter of %.0f s (%s)",
                                     toDb(late) - toDb(early), seconds, set.first.c_str()));
        else
            report(test, "warn", fmt("still %.1f dBFS RMS %.0f s after an impulse and not decaying (%s) — runaway feedback, "
                                     "DC, or an oscillator?", toDb(late), seconds, set.first.c_str()));
    }
}

static void testDc()
{
    const double sr = 48000;
    auto e = makeEngine(sr, 512);
    const Buffer out = runFixed(*e, makeSine(NI, (size_t)sr, sr, 220.0, 0.5f), 512);
    double worst = 0.0;
    for (const auto& ch : out)
    {
        double sum = 0.0;
        for (size_t i = ch.size() / 2; i < ch.size(); ++i)
            sum += ch[i];
        worst = std::max(worst, std::fabs(sum / (ch.size() / 2)));
    }
    if (worst > 0.01)
        report("dc offset", "warn", fmt("output has DC offset %.3f with a sine input — add a DcBlocker after asymmetric shaping", worst));
    else
        report("dc offset", "pass", fmt("DC offset %.4f", worst));
}

static void testReset()
{
    const double sr = 48000;
    const Buffer noise = makeNoise(NI, (size_t)(sr * 0.5), 0.25f, 3);
    auto e = makeEngine(sr, 512);
    const Buffer a = runFixed(*e, noise, 512);
    std::mt19937 rng(5);
    applyParams(*e, paramsMax());
    runFixed(*e, makeNoise(NI, (size_t)(sr * 0.5), 0.5f, 4), 512);
    applyParams(*e, paramsRandom(rng));
    runFixed(*e, makeNoise(NI, (size_t)(sr * 0.5), 0.5f, 5), 512);
    applyParams(*e, paramsDefault());
    e->reset();
    const Buffer b = runFixed(*e, noise, 512);
    const double d = maxDiff(a, b);
    if (d > 1e-5)
        report("reset", "warn", fmt("output after reset() differs from a fresh instance (max diff %.2g) — reset() should clear "
                                    "every delay line, filter, envelope and LFO phase", d));
    else
        report("reset", "pass", "reset() restores a clean state");
}

static void testBlockSize()
{
    const double sr = 48000;
    const Buffer noise = makeNoise(NI, (size_t)sr, 0.25f, 21);
    auto a = makeEngine(sr, 512);
    auto b = makeEngine(sr, 512);
    const Buffer oa = runFixed(*a, noise, 512);
    std::mt19937 rng(77);
    std::uniform_int_distribution<int> bs(1, 512);
    const Buffer ob = run(*b, noise, [&](size_t) { return (uint32_t)bs(rng); }, [] {});
    const double d = maxDiff(oa, ob);
    if (d > 1e-4)
        report("block size", "warn", fmt("output depends on how the host splits buffers (max diff %.2g) — update coefficients "
                                         "with a running per-sample counter instead of once per process() call", d));
    else
        report("block size", "pass", "identical output for fixed and random block sizes");
}

static void testCpu()
{
    const double sr = 48000;
    const double seconds = 10.0;
    const Buffer noise = makeNoise(NI, (size_t)(sr * seconds), 0.25f, 8);
    double worst = 0.0;
    std::string worstSet;
    const std::vector<std::pair<std::string, ParamSet>> sets = {{"defaults", paramsDefault()}, {"all-max", paramsMax()}};
    for (const auto& set : sets)
    {
        auto e = makeEngine(sr, 128);
        applyParams(*e, set.second);
        e->reset();
        const auto t0 = std::chrono::steady_clock::now();
        runFixed(*e, noise, 128);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el > worst)
        {
            worst = el;
            worstSet = set.first;
        }
    }
    const double pct = 100.0 * worst / seconds;
    report("cpu", pct > 25.0 ? "warn" : "info",
           fmt("%.2f%% of one core at 48 kHz, 128-sample blocks (%.0fx realtime, worst case: %s)",
               pct, seconds / std::max(worst, 1e-9), worstSet.c_str()));
}

static int runTests(bool quick)
{
    long allocs = 0;
    testInfo();
    testChannels();
    testStability(quick, allocs);
    testAutomation(allocs);
    if (allocs > 0)
        report("realtime", "fail", fmt("%ld heap allocation%s inside process() — allocate everything in prepare()",
                                       allocs, allocs > 1 ? "s" : ""));
    else
#if defined(COOKBOOK_NO_ALLOC_HOOK)
        report("realtime", "info", "allocation tracking disabled in sanitizer builds");
#else
        report("realtime", "pass", "no heap allocations inside process()");
#endif
    testSilence();
    testTail(quick);
    testDc();
    testReset();
    testBlockSize();
    testCpu();
    return gFailed ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// WAV I/O (PCM 16/24/32-bit and float32 in, float32 out)

static uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool readWav(const char* path, Buffer& out, double& sr)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4))
        return false;
    uint16_t fmtTag = 0, channels = 0, bits = 0;
    const unsigned char* data = nullptr;
    size_t dataSize = 0;
    for (size_t pos = 12; pos + 8 <= d.size();)
    {
        const uint32_t sz = rd32(&d[pos + 4]);
        if (!std::memcmp(&d[pos], "fmt ", 4) && pos + 8 + 16 <= d.size())
        {
            fmtTag = rd16(&d[pos + 8]);
            channels = rd16(&d[pos + 10]);
            sr = rd32(&d[pos + 12]);
            bits = rd16(&d[pos + 22]);
            if (fmtTag == 0xFFFE && sz >= 26)
                fmtTag = rd16(&d[pos + 8 + 24]);
        }
        else if (!std::memcmp(&d[pos], "data", 4))
        {
            data = &d[pos + 8];
            dataSize = std::min<size_t>(sz, d.size() - pos - 8);
        }
        pos += 8 + sz + (sz & 1);
    }
    if (!data || !channels || !(fmtTag == 1 || fmtTag == 3))
        return false;
    const size_t bps = bits / 8;
    const size_t frames = dataSize / (bps * channels);
    out = makeSilence(channels, frames);
    for (size_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < channels; ++c)
        {
            const unsigned char* p = data + (i * channels + c) * bps;
            float v = 0.f;
            if (fmtTag == 3 && bits == 32) { std::memcpy(&v, p, 4); }
            else if (bits == 16) v = (int16_t)rd16(p) / 32768.f;
            else if (bits == 24) v = (float)((int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8) / 8388608.f;
            else if (bits == 32) v = (float)((int32_t)rd32(p) / 2147483648.0);
            out[c][i] = v;
        }
    return true;
}

static bool writeWav(const char* path, const Buffer& b, double sr)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    const uint32_t ch = (uint32_t)b.size(), frames = b.empty() ? 0 : (uint32_t)b[0].size();
    const uint32_t dataSize = frames * ch * 4;
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + dataSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(3); w16((uint16_t)ch); w32((uint32_t)sr); w32((uint32_t)sr * ch * 4); w16((uint16_t)(ch * 4)); w16(32);
    f.write("data", 4); w32(dataSize);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < ch; ++c)
            f.write((const char*)&b[c][i], 4);
    return (bool)f;
}

static int render(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: harness render IN.wav OUT.wav [--set sym=value] [--block N]\n");
        return 2;
    }
    Buffer wav;
    double sr = 48000;
    if (!readWav(argv[2], wav, sr))
    {
        std::fprintf(stderr, "could not read %s (PCM 16/24/32-bit or float32 WAV expected)\n", argv[2]);
        return 2;
    }
    uint32_t block = 512;
    std::unique_ptr<Engine> e(new Engine());
    for (int i = 4; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--block") && i + 1 < argc)
            block = (uint32_t)std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--set") && i + 1 < argc)
        {
            const std::string kv = argv[++i];
            const size_t eq = kv.find('=');
            bool found = false;
            for (uint32_t p = 0; p < NP && eq != std::string::npos; ++p)
                if (kv.substr(0, eq) == Meta::kParams[p].symbol)
                {
                    std::string val = kv.substr(eq + 1);
                    float v = 0.f;
                    if (Meta::kParams[p].type == ParamType::Choice)
                    {
                        v = (float)std::atof(val.c_str());
                        for (uint32_t c = 0; c < Meta::kParams[p].numChoices; ++c)
                            if (val == Meta::kParams[p].choices[c])
                                v = (float)c;
                    }
                    else if (val == "true" || val == "on")
                        v = 1.f;
                    else if (val == "false" || val == "off")
                        v = 0.f;
                    else
                        v = (float)std::atof(val.c_str());
                    e->setParameter(p, v);
                    found = true;
                }
            if (!found)
            {
                std::fprintf(stderr, "unknown parameter in --set %s\n", kv.c_str());
                return 2;
            }
        }
    }
    e->prepare(sr, block);

    // map file channels onto the plugin's inputs
    Buffer in = makeSilence(NI, wav[0].size());
    for (uint32_t c = 0; c < NI; ++c)
        for (size_t i = 0; i < in[c].size(); ++i)
        {
            if (wav.size() >= NI)
                in[c][i] = wav[c][i];
            else if (NI == 1)
                in[c][i] = wav[0][i];
            else
                in[c][i] = wav[c % wav.size()][i];
        }
    if (NI == 1 && wav.size() > 1)
        for (size_t i = 0; i < in[0].size(); ++i)
        {
            float s = 0.f;
            for (const auto& ch : wav)
                s += ch[i];
            in[0][i] = s / (float)wav.size();
        }

    // append latency + 2 s of silence so tails are not cut off
    const size_t extra = e->latency() + (size_t)(sr * 2.0);
    for (auto& ch : in)
        ch.resize(ch.size() + extra, 0.f);

    RunStats s;
    const Buffer out = runFixed(*e, in, block, &s);
    if (!writeWav(argv[3], out, sr))
    {
        std::fprintf(stderr, "could not write %s\n", argv[3]);
        return 2;
    }
    std::printf("rendered %s -> %s: %.2f s at %g Hz, peak %+.1f dBFS, latency %u samples%s\n", argv[2], argv[3],
                out[0].size() / sr, sr, toDb(s.peak), e->latency(), e->safetyTrips() ? " (safety mute engaged!)" : "");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && !std::strcmp(argv[1], "test"))
        return runTests(argc >= 3 && !std::strcmp(argv[2], "--quick"));
    if (argc >= 2 && !std::strcmp(argv[1], "render"))
        return render(argc, argv);
    std::fprintf(stderr, "usage: harness test [--quick] | harness render IN.wav OUT.wav [--set sym=value] [--block N]\n");
    return 2;
}
