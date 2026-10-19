// Plugin Cookbook — standalone app: the recipe's Engine on RtAudio, with the generic controls and an
// audio settings panel (system, devices, channels, sample rate, buffer size).
// SPDX-License-Identifier: MIT
//
// Replaces DPF's own standalone ("jack" format), which always uses the default devices at a fixed
// 48 kHz. Settings and parameter values are remembered in standalone.ini:
//   macOS    ~/Library/Application Support/<label>/
//   Windows  %APPDATA%\<label>\
//   Linux    $XDG_CONFIG_HOME/<label>/ (default ~/.config/<label>/)

// RtAudio first: DGL's headers pull `using namespace DGL` in, and its Point clashes with CoreAudio's.
#include "RtAudio.h"

#include "DistrhoPluginInfo.h"
#include "RecipeAssets.hpp"
#include "RecipeKernel.hpp"
#include "cookbook/Engine.hpp"

#include "Application.hpp"
#include "cookbook/Controls.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
# include <direct.h>
#else
# include <sys/stat.h>
#endif

USE_NAMESPACE_DGL

namespace {

namespace cui = cookbook::ui;
using Meta = recipe::Meta;

constexpr uint32_t kNumParams = Meta::kNumParams;
constexpr uint32_t kParamSlots = kNumParams > 0 ? kNumParams : 1;
constexpr float kBarH = 34.f;
constexpr unsigned kBufferSizes[] = { 32, 64, 128, 256, 512, 1024, 2048 };

// ---------------------------------------------------------------------------------------------------------------------
// Settings file

std::string configDir()
{
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    return std::string(base ? base : ".") + "\\" COOKBOOK_LABEL;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/" COOKBOOK_LABEL;
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
        if (xdg[0])
            return std::string(xdg) + "/" COOKBOOK_LABEL;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/" COOKBOOK_LABEL;
#endif
}

void makeDirs(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); ++i)
    {
        if (i < path.size() && path[i] != '/' && path[i] != '\\')
            continue;
        const std::string part = path.substr(0, i);
#ifdef _WIN32
        _mkdir(part.c_str());
#else
        mkdir(part.c_str(), 0755);
#endif
    }
}

struct Settings
{
    std::string api;    // RtAudio API name ("core", "wasapi", "alsa", "pulse"); empty = first with devices
    std::string input;  // device names; empty or missing = system default
    std::string output;
    unsigned inFirst = 0;  // first device channel used for input / output
    unsigned outFirst = 0;
    unsigned sampleRate = 0; // 0 = pick automatically
    unsigned bufferSize = 256;
};

// MinGW cross-compilers built with win32 threads have no standard mutex. This only guards the error
// text, which is rare and short, so a spin lock is plenty.
class SpinLock
{
public:
    void lock() noexcept { while (flag_.test_and_set(std::memory_order_acquire)) {} }
    void unlock() noexcept { flag_.clear(std::memory_order_release); }

    struct Scoped
    {
        explicit Scoped(SpinLock& l) noexcept : lock(l) { lock.lock(); }
        ~Scoped() { lock.unlock(); }
        SpinLock& lock;
    };

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// ---------------------------------------------------------------------------------------------------------------------
// Audio: owns the Engine and the RtAudio stream

class AudioHost
{
public:
    struct Device
    {
        unsigned id;
        std::string name;
        unsigned ins, outs;
        std::vector<unsigned> rates;
        unsigned preferredRate;
        bool defaultIn, defaultOut;
    };

    Settings settings;
    std::atomic<float> params[kParamSlots];

    // Valid after open(); read and written on the UI thread only.
    std::vector<RtAudio::Api> apis;
    RtAudio::Api api = RtAudio::UNSPECIFIED;
    std::vector<Device> devices;
    int inDevice = -1, outDevice = -1; // index into devices
    unsigned inChannels = 0, outChannels = 0;
    std::vector<unsigned> rates; // offered sample rates
    unsigned rate = 0, buffer = 0;
    bool running = false;
    std::string status;

    std::atomic<float> peakIn { 0.f }, peakOut { 0.f };
    std::atomic<uint32_t> xruns { 0 };

    AudioHost()
    {
        for (uint32_t i = 0; i < kNumParams; ++i)
            params[i].store(Meta::kParams[i].def);
        sInstance = this;
    }

    ~AudioHost()
    {
        close();
        sInstance = nullptr;
    }

    static std::string settingsPath() { return configDir() + "/standalone.ini"; }

    void load()
    {
        std::ifstream f(settingsPath());
        std::string line;
        while (std::getline(f, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
            const unsigned num = (unsigned)std::strtoul(val.c_str(), nullptr, 10);
            if (key == "api") settings.api = val;
            else if (key == "input") settings.input = val;
            else if (key == "output") settings.output = val;
            else if (key == "in_first") settings.inFirst = num;
            else if (key == "out_first") settings.outFirst = num;
            else if (key == "sample_rate") settings.sampleRate = num;
            else if (key == "buffer_size") settings.bufferSize = num ? num : 256;
            else if (key.compare(0, 6, "param.") == 0)
            {
                for (uint32_t i = 0; i < kNumParams; ++i)
                {
                    const cookbook::ParamInfo& p = Meta::kParams[i];
                    if (key.compare(6, std::string::npos, p.symbol) != 0)
                        continue;
                    float v = std::strtof(val.c_str(), nullptr);
                    if (!(v == v))
                        v = p.def;
                    params[i].store(std::min(std::max(v, p.min), p.max));
                }
            }
        }
    }

    void save() const
    {
        makeDirs(configDir());
        std::ofstream f(settingsPath(), std::ios::trunc);
        if (!f)
            return;
        f << "api=" << settings.api << "\n"
          << "input=" << settings.input << "\n"
          << "output=" << settings.output << "\n"
          << "in_first=" << settings.inFirst << "\n"
          << "out_first=" << settings.outFirst << "\n"
          << "sample_rate=" << settings.sampleRate << "\n"
          << "buffer_size=" << settings.bufferSize << "\n";
        for (uint32_t i = 0; i < kNumParams; ++i)
            f << "param." << Meta::kParams[i].symbol << "=" << params[i].load() << "\n";
    }

    // (Re)opens the stream from `settings`, falling back to defaults for anything that is unavailable.
    // `settings` is updated to what was actually opened.
    void open()
    {
        close();
        audio_.reset();
        devices.clear();
        rates.clear();
        inDevice = outDevice = -1;
        inChannels = outChannels = 0;
        rate = buffer = 0;
        status.clear();

        apis.clear();
        std::vector<RtAudio::Api> compiled;
        RtAudio::getCompiledApi(compiled);
        for (RtAudio::Api a : compiled)
            if (a != RtAudio::RTAUDIO_DUMMY)
                apis.push_back(a);

        RtAudio::Api wanted = RtAudio::UNSPECIFIED;
        for (RtAudio::Api a : apis)
            if (RtAudio::getApiName(a) == settings.api)
                wanted = a;

        try
        {
            audio_.reset(new RtAudio(wanted));
            audio_->showWarnings(false);
        }
        catch (const RtAudioError& e)
        {
            audio_.reset();
            status = e.getMessage();
            return;
        }
        api = audio_->getCurrentApi();
        settings.api = RtAudio::getApiName(api);

        scanDevices();

        if (Meta::kNumInputs > 0)
            inDevice = findDevice(settings.input, true);
        outDevice = findDevice(settings.output, false);
        if (outDevice < 0)
        {
            status = "No output device found.";
            return;
        }

        const Device& out = devices[outDevice];
        settings.output = out.name;
        settings.outFirst = std::min(settings.outFirst, out.outs - 1);
        outChannels = std::min(2u, out.outs - settings.outFirst);

        if (inDevice >= 0)
        {
            const Device& in = devices[inDevice];
            settings.input = in.name;
            settings.inFirst = std::min(settings.inFirst, in.ins - 1);
            inChannels = std::min(Meta::kNumInputs, in.ins - settings.inFirst);
        }

        chooseSampleRate();

        RtAudio::StreamParameters outParams;
        outParams.deviceId = out.id;
        outParams.nChannels = outChannels;
        outParams.firstChannel = settings.outFirst;

        RtAudio::StreamParameters inParams;
        if (inDevice >= 0)
        {
            inParams.deviceId = devices[inDevice].id;
            inParams.nChannels = inChannels;
            inParams.firstChannel = settings.inFirst;
        }

        RtAudio::StreamOptions options;
        options.flags = RTAUDIO_NONINTERLEAVED | RTAUDIO_SCHEDULE_REALTIME;
#ifndef __APPLE__
        options.flags |= RTAUDIO_MINIMIZE_LATENCY;
#endif
        options.streamName = DISTRHO_PLUGIN_NAME;

        unsigned frames = settings.bufferSize;
        takeStreamError();
        try
        {
            audio_->openStream(&outParams, inDevice >= 0 ? &inParams : nullptr, RTAUDIO_FLOAT32, rate, &frames,
                               &AudioHost::audioCallback, this, &options, &AudioHost::errorCallback);
        }
        catch (const RtAudioError& e)
        {
            status = e.getMessage();
        }
        if (!audio_->isStreamOpen())
        {
            if (status.empty())
                status = takeStreamError();
            if (status.empty())
                status = "Could not open the audio device.";
            return;
        }

        rate = audio_->getStreamSampleRate();
        buffer = std::max(1u, frames);
        settings.sampleRate = rate;
        settings.bufferSize = buffer;

        // Not running yet, so this is the one safe place to allocate.
        engine_.prepare(rate, buffer);
        scratch_.assign((size_t)Meta::kNumOutputs * buffer, 0.f);
        silence_.assign(buffer, 0.f);
        streamInChannels_ = inChannels;
        streamOutChannels_ = outChannels;
        streamBuffer_ = buffer;
        xruns.store(0);

        try
        {
            audio_->startStream();
            running = true;
        }
        catch (const RtAudioError& e)
        {
            status = e.getMessage();
        }
        if (!running && status.empty())
            status = takeStreamError();
    }

    void close()
    {
        if (!audio_)
            return;
        try
        {
            if (audio_->isStreamRunning())
                audio_->stopStream();
            if (audio_->isStreamOpen())
                audio_->closeStream();
        }
        catch (const RtAudioError&) {}
        running = false;
        peakIn.store(0.f);
        peakOut.store(0.f);
    }

    // Called on the UI thread to notice errors that happened while the stream was running.
    void poll()
    {
        if (!running)
            return;
        const std::string err = takeStreamError();
        if (!err.empty() || (audio_ && !audio_->isStreamRunning()))
        {
            running = false;
            status = err.empty() ? "The audio stream stopped." : err;
        }
    }

private:
    void scanDevices()
    {
        const unsigned count = audio_->getDeviceCount();
        for (unsigned i = 0; i < count; ++i)
        {
            RtAudio::DeviceInfo info;
            try
            {
                info = audio_->getDeviceInfo(i);
            }
            catch (const RtAudioError&)
            {
                continue;
            }
            if (!info.probed || (info.inputChannels == 0 && info.outputChannels == 0))
                continue;
            devices.push_back({ i, info.name, info.inputChannels, info.outputChannels, info.sampleRates,
                                info.preferredSampleRate, info.isDefaultInput, info.isDefaultOutput });
        }
    }

    int findDevice(const std::string& name, const bool input) const
    {
        auto channels = [input](const Device& d) { return input ? d.ins : d.outs; };
        if (!name.empty())
            for (size_t i = 0; i < devices.size(); ++i)
                if (devices[i].name == name && channels(devices[i]) > 0)
                    return (int)i;
        for (size_t i = 0; i < devices.size(); ++i)
            if ((input ? devices[i].defaultIn : devices[i].defaultOut) && channels(devices[i]) > 0)
                return (int)i;
        for (size_t i = 0; i < devices.size(); ++i)
            if (channels(devices[i]) > 0)
                return (int)i;
        return -1;
    }

    // Offer only the rates both devices support; pick the saved one, else the output's preferred rate,
    // else 48 kHz, else 44.1 kHz.
    void chooseSampleRate()
    {
        const Device& out = devices[outDevice];
        for (unsigned r : out.rates)
            if (inDevice < 0 || std::find(devices[inDevice].rates.begin(), devices[inDevice].rates.end(), r) !=
                                    devices[inDevice].rates.end())
                rates.push_back(r);

        auto offered = [this](unsigned r) {
            return r != 0 && std::find(rates.begin(), rates.end(), r) != rates.end();
        };
        if (rates.empty())
        {
            rate = out.preferredRate ? out.preferredRate : 48000;
            rates.push_back(rate);
        }
        else if (offered(settings.sampleRate)) rate = settings.sampleRate;
        else if (offered(out.preferredRate)) rate = out.preferredRate;
        else if (offered(48000)) rate = 48000;
        else if (offered(44100)) rate = 44100;
        else rate = rates.front();
    }

    std::string takeStreamError()
    {
        SpinLock::Scoped lock(errorLock_);
        std::string err;
        err.swap(streamError_);
        return err;
    }

    static void errorCallback(const RtAudioError::Type type, const std::string& text)
    {
        if (type == RtAudioError::WARNING || type == RtAudioError::DEBUG_WARNING || !sInstance)
            return;
        SpinLock::Scoped lock(sInstance->errorLock_);
        sInstance->streamError_ = text;
    }

    static int audioCallback(void* out, void* in, const unsigned frames, double, const RtAudioStreamStatus st,
                             void* user)
    {
        static_cast<AudioHost*>(user)->process(static_cast<float*>(out), static_cast<const float*>(in), frames, st);
        return 0;
    }

    // Realtime. Buffers are non-interleaved: device channel c starts at buf + c * frames.
    void process(float* out, const float* in, const unsigned frames, const RtAudioStreamStatus st) noexcept
    {
        if (st)
            xruns.fetch_add(1, std::memory_order_relaxed);

        for (uint32_t i = 0; i < kNumParams; ++i)
            engine_.setParameter(i, params[i].load(std::memory_order_relaxed));

        float inPeak = 0.f, outPeak = 0.f;
        for (unsigned c = 0; c < streamInChannels_; ++c)
            for (unsigned k = 0; k < frames; ++k)
                inPeak = std::max(inPeak, std::fabs(in[c * frames + k]));

        constexpr uint32_t kIns = Meta::kNumInputs > 0 ? Meta::kNumInputs : 1;
        constexpr uint32_t kOuts = Meta::kNumOutputs;

        for (unsigned done = 0; done < frames;)
        {
            const unsigned n = std::min(frames - done, streamBuffer_);

            // A stereo plugin fed by one device channel gets it on both inputs.
            const float* ins[kIns];
            for (uint32_t c = 0; c < kIns; ++c)
                ins[c] = streamInChannels_ > 0 ? in + std::min<unsigned>(c, streamInChannels_ - 1) * frames + done
                                               : silence_.data();

            float* outs[kOuts];
            for (uint32_t c = 0; c < kOuts; ++c)
                outs[c] = scratch_.data() + (size_t)c * streamBuffer_;

            engine_.process(ins, outs, n);

            // Mono plugin output goes to both device channels; stereo into a mono device is mixed down.
            for (unsigned d = 0; d < streamOutChannels_; ++d)
            {
                float* dst = out + d * frames + done;
                if (streamOutChannels_ == 1 && kOuts == 2)
                    for (unsigned k = 0; k < n; ++k)
                        dst[k] = 0.5f * (outs[0][k] + outs[kOuts - 1][k]);
                else
                    std::memcpy(dst, outs[std::min<unsigned>(d, kOuts - 1)], sizeof(float) * n);
                for (unsigned k = 0; k < n; ++k)
                    outPeak = std::max(outPeak, std::fabs(dst[k]));
            }
            done += n;
        }

        if (inPeak > peakIn.load(std::memory_order_relaxed))
            peakIn.store(inPeak, std::memory_order_relaxed);
        if (outPeak > peakOut.load(std::memory_order_relaxed))
            peakOut.store(outPeak, std::memory_order_relaxed);
    }

    static AudioHost* sInstance;

    std::unique_ptr<RtAudio> audio_;
    cookbook::Engine<recipe::Kernel, Meta> engine_;
    std::vector<float> scratch_, silence_;
    unsigned streamInChannels_ = 0, streamOutChannels_ = 0, streamBuffer_ = 1;
    SpinLock errorLock_;
    std::string streamError_;
};

AudioHost* AudioHost::sInstance = nullptr;

// ---------------------------------------------------------------------------------------------------------------------
// Window

std::string channelLabel(const unsigned first, const unsigned count)
{
    char buf[32];
    if (count >= 2)
        std::snprintf(buf, sizeof(buf), "%u + %u", first + 1, first + 2);
    else
        std::snprintf(buf, sizeof(buf), "%u", first + 1);
    return buf;
}

class CookbookStandaloneWindow : public ImGuiStandaloneWindow,
                                 private cui::ControlsHost
{
public:
    CookbookStandaloneWindow(Application& app, AudioHost& host)
        : ImGuiStandaloneWindow(app),
          host_(host),
          controls_(recipe::kAssets, recipe::kNumAssets)
    {
        const double s = getScaleFactor();
        Window::setTitle(DISTRHO_PLUGIN_NAME);
        Window::setResizable(true);
        if (Meta::kFreeLayout)
            setGeometryConstraints(Meta::kBaseWidth * 0.5 * s, (Meta::kBaseHeight * 0.5 + kBarH) * s, true);
        else
            setGeometryConstraints(300 * s, (180 + kBarH) * s);
        setSize(DISTRHO_UI_DEFAULT_WIDTH * s, (DISTRHO_UI_DEFAULT_HEIGHT + kBarH) * s);
        cui::applyStyle(COOKBOOK_UI_ACCENT, COOKBOOK_UI_LIGHT, COOKBOOK_UI_BACKGROUND);
        for (uint32_t i = 0; i < kNumParams; ++i)
            controls_.values[i] = host_.params[i].load();
        done();
    }

    ~CookbookStandaloneWindow() override
    {
        controls_.releaseImages();
    }

protected:
    void onImGuiDisplay() override
    {
        const float s = getScaleFactor();
        const float width = getWidth();
        const float height = getHeight();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, height));

        if (ImGui::Begin("##cookbook", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse))
        {
            const float top = (cui::headerHeight(COOKBOOK_UI_SUBTITLE) + 8.f) * s;
            const float area = height - kBarH * s;
            if (showSettings_)
            {
                cui::drawHeader(s, width, DISTRHO_PLUGIN_NAME, DISTRHO_PLUGIN_BRAND, COOKBOOK_UI_SUBTITLE,
                                COOKBOOK_UI_ACCENT);
                drawSettings(s, width, top, area);
            }
            else
            {
                controls_.drawBackground(width, area);
                if (!Meta::kFreeLayout)
                    cui::drawHeader(s, width, DISTRHO_PLUGIN_NAME, DISTRHO_PLUGIN_BRAND, COOKBOOK_UI_SUBTITLE,
                                    COOKBOOK_UI_ACCENT);
                controls_.draw(s, width, top, COOKBOOK_UI_ACCENT, *this, area);
            }
            drawBar(s, width, height);
        }
        ImGui::End();
    }

    // ImGuiWidget is already an IdleCallback (it repaints at 60 fps); hook it for deferred work.
    void idleCallback() override
    {
        if (resizeTo_[0] != 0)
        {
            setSize(resizeTo_[0], resizeTo_[1]);
            resizeTo_[0] = resizeTo_[1] = 0;
        }
        if (reopen_)
        {
            reopen_ = false;
            host_.open();
            markDirty();
        }
        host_.poll();
        if (dirty_ && getApp().getTime() - dirtySince_ > 1.0)
        {
            dirty_ = false;
            host_.save();
        }
        ImGuiStandaloneWindow::idleCallback();
    }

private:
    void beginEdit(uint32_t) override {}
    void endEdit(uint32_t) override {}
    void setValue(const uint32_t index, const float value) override
    {
        host_.params[index].store(value);
        markDirty();
    }

    void markDirty()
    {
        if (!dirty_)
            dirtySince_ = getApp().getTime();
        dirty_ = true;
    }

    void toggleSettings()
    {
        const float s = getScaleFactor();
        showSettings_ = !showSettings_;
        if (showSettings_)
        {
            savedSize_[0] = getWidth();
            savedSize_[1] = getHeight();
            resizeTo_[0] = std::max<uint>(savedSize_[0], 470 * s);
            resizeTo_[1] = std::max<uint>(savedSize_[1], 400 * s);
        }
        else if (savedSize_[0] != 0)
        {
            resizeTo_[0] = savedSize_[0];
            resizeTo_[1] = savedSize_[1];
        }
    }

    void applyChange()
    {
        reopen_ = true;
    }

    // ----------------------------------------------------------------------------------------------------------------

    void drawBar(const float s, const float width, const float height)
    {
        const float y = height - kBarH * s;
        const ImVec2 wp = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(wp.x, wp.y + y), ImVec2(wp.x + width, wp.y + height),
                          ImGui::GetColorU32(ImGuiCol_MenuBarBg));
        dl->AddLine(ImVec2(wp.x, wp.y + y), ImVec2(wp.x + width, wp.y + y), ImGui::GetColorU32(ImGuiCol_Border));

        const float rowY = y + (kBarH * s - ImGui::GetFrameHeight()) * 0.5f;
        ImGui::SetCursorPos(ImVec2(10.f * s, rowY));
        if (ImGui::Button(showSettings_ ? "Done" : "Audio settings"))
            toggleSettings();

        // meters on the right, summary in between
        const float meterW = 54.f * s;
        const float metersX = width - 10.f * s - 2.f * (meterW + 26.f * s);
        ImGui::SameLine();
        const float textY = y + (kBarH * s - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorPosY(textY);
        char summary[96];
        if (host_.running)
            std::snprintf(summary, sizeof(summary), "%.1f kHz  %u", host_.rate / 1000.0, host_.buffer);
        else
            std::snprintf(summary, sizeof(summary), "No audio");
        const ImVec4 summaryColor = host_.running ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] : cui::rgb(0xe5534b);
        if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(summary).x < metersX - 6.f * s)
        {
            ImGui::TextColored(summaryColor, "%s", summary);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", summaryTooltip().c_str());
        }

        meterIn_ = std::max(host_.peakIn.exchange(0.f), meterIn_ * 0.86f);
        meterOut_ = std::max(host_.peakOut.exchange(0.f), meterOut_ * 0.86f);
        drawMeter("IN", meterIn_, metersX, textY, meterW, s);
        drawMeter("OUT", meterOut_, metersX + meterW + 26.f * s, textY, meterW, s);
    }

    void drawMeter(const char* label, const float peak, const float x, const float y, const float w, const float s)
    {
        ImGui::SetCursorPos(ImVec2(x, y));
        ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s", label);
        const float lx = x + 24.f * s;
        const ImVec2 wp = ImGui::GetWindowPos();
        const float h = 6.f * s;
        const float my = wp.y + y + (ImGui::GetTextLineHeight() - h) * 0.5f;
        const float db = peak > 1e-6f ? 20.f * std::log10(peak) : -120.f;
        const float t = std::min(1.f, std::max(0.f, (db + 60.f) / 60.f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(wp.x + lx, my), ImVec2(wp.x + lx + w, my + h), ImGui::GetColorU32(ImGuiCol_FrameBgActive), 2.f);
        if (t > 0.f)
            dl->AddRectFilled(ImVec2(wp.x + lx, my), ImVec2(wp.x + lx + w * t, my + h),
                              ImGui::GetColorU32(peak >= 0.99f ? cui::rgb(0xe5534b) : cui::rgb(COOKBOOK_UI_ACCENT)), 2.f);
    }

    std::string summaryTooltip() const
    {
        std::string text;
        if (host_.inDevice >= 0)
            text += "In: " + host_.devices[host_.inDevice].name + " (" +
                    channelLabel(host_.settings.inFirst, host_.inChannels) + ")\n";
        if (host_.outDevice >= 0)
            text += "Out: " + host_.devices[host_.outDevice].name + " (" +
                    channelLabel(host_.settings.outFirst, host_.outChannels) + ")\n";
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%u Hz, %u samples (%.1f ms)", host_.rate, host_.buffer,
                      host_.rate ? 1000.0 * host_.buffer / host_.rate : 0.0);
        return text + buf;
    }

    // ----------------------------------------------------------------------------------------------------------------

    void drawSettings(const float s, const float width, const float top, const float bottom)
    {
        ImGui::SetCursorPos(ImVec2(cui::kPad * s, top));
        if (!ImGui::BeginChild("##settings", ImVec2(width - 2.f * cui::kPad * s, bottom - top - 8.f * s)))
        {
            ImGui::EndChild();
            return;
        }

        AudioHost& h = host_;
        Settings& st = h.settings;

        if (ImGui::BeginTable("##audio", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 118.f * s);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

            if (row("Audio system", h.apis.size() > 1))
            {
                if (ImGui::BeginCombo("##api", RtAudio::getApiDisplayName(h.api).c_str()))
                {
                    for (RtAudio::Api a : h.apis)
                        if (ImGui::Selectable(RtAudio::getApiDisplayName(a).c_str(), a == h.api) && a != h.api)
                        {
                            st.api = RtAudio::getApiName(a);
                            st.input.clear(); // device names differ between systems: start from the defaults
                            st.output.clear();
                            applyChange();
                        }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
            }

            if (Meta::kNumInputs > 0)
            {
                if (row("Input device", h.inDevice >= 0))
                {
                    deviceCombo("##in", h.inDevice, true, st.input);
                    ImGui::EndDisabled();
                }
                if (row("Input channel(s)", h.inDevice >= 0))
                {
                    const unsigned total = h.inDevice >= 0 ? h.devices[h.inDevice].ins : 0;
                    channelCombo("##inch", total, Meta::kNumInputs, st.inFirst, h.inChannels);
                    ImGui::EndDisabled();
                }
            }

            if (row("Output device", h.outDevice >= 0))
            {
                deviceCombo("##out", h.outDevice, false, st.output);
                ImGui::EndDisabled();
            }
            if (row("Output channels", h.outDevice >= 0))
            {
                const unsigned total = h.outDevice >= 0 ? h.devices[h.outDevice].outs : 0;
                channelCombo("##outch", total, 2, st.outFirst, h.outChannels);
                ImGui::EndDisabled();
            }

            if (row("Sample rate", !h.rates.empty()))
            {
                char preview[32];
                std::snprintf(preview, sizeof(preview), "%u Hz", h.rate);
                if (ImGui::BeginCombo("##sr", preview))
                {
                    for (unsigned r : h.rates)
                    {
                        char item[32];
                        std::snprintf(item, sizeof(item), "%u Hz", r);
                        if (ImGui::Selectable(item, r == h.rate) && r != h.rate)
                        {
                            st.sampleRate = r;
                            applyChange();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
            }

            if (row("Buffer size", h.outDevice >= 0))
            {
                char preview[48];
                std::snprintf(preview, sizeof(preview), "%u samples", h.buffer ? h.buffer : st.bufferSize);
                if (ImGui::BeginCombo("##bs", preview))
                {
                    for (unsigned b : kBufferSizes)
                    {
                        char item[48];
                        if (h.rate)
                            std::snprintf(item, sizeof(item), "%u samples (%.1f ms)", b, 1000.0 * b / h.rate);
                        else
                            std::snprintf(item, sizeof(item), "%u samples", b);
                        if (ImGui::Selectable(item, b == h.buffer) && b != h.buffer)
                        {
                            st.bufferSize = b;
                            applyChange();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::Button("Rescan devices"))
            applyChange();

        ImGui::Spacing();
        if (h.running)
            ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "Running: %u Hz, %u samples (%.1f ms)",
                               h.rate, h.buffer, 1000.0 * h.buffer / h.rate);
        else
            ImGui::TextColored(cui::rgb(0xe5534b), "%s", h.status.empty() ? "Audio is stopped." : h.status.c_str());

        const uint32_t xr = h.xruns.load();
        ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "Dropouts: %u", xr);
        if (xr > 0)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset"))
                h.xruns.store(0);
        }

#ifdef __APPLE__
        if (Meta::kNumInputs > 0 && h.running && h.inDevice >= 0)
        {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "%s",
                               "No input signal? Allow microphone access in System Settings > Privacy & Security > "
                               "Microphone, then restart the app.");
            ImGui::PopTextWrapPos();
        }
#endif

        ImGui::EndChild();
    }

    // Starts a table row with a label; the caller must call ImGui::EndDisabled() after its widget.
    static bool row(const char* label, const bool enabled)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::BeginDisabled(!enabled);
        return true;
    }

    void deviceCombo(const char* id, const int current, const bool input, std::string& setting)
    {
        const char* preview = current >= 0 ? host_.devices[current].name.c_str() : "(none)";
        if (!ImGui::BeginCombo(id, preview))
            return;
        for (size_t i = 0; i < host_.devices.size(); ++i)
        {
            const AudioHost::Device& d = host_.devices[i];
            if ((input ? d.ins : d.outs) == 0)
                continue;
            if (ImGui::Selectable(d.name.c_str(), (int)i == current) && (int)i != current)
            {
                setting = d.name;
                (input ? host_.settings.inFirst : host_.settings.outFirst) = 0;
                applyChange();
            }
        }
        ImGui::EndCombo();
    }

    // One entry per starting channel; each opens up to `wanted` consecutive channels.
    void channelCombo(const char* id, const unsigned total, const unsigned wanted, unsigned& first,
                      const unsigned opened)
    {
        const std::string preview = total ? channelLabel(first, opened) : std::string();
        if (!ImGui::BeginCombo(id, preview.c_str()))
            return;
        for (unsigned f = 0; f < total; ++f)
        {
            const std::string label = channelLabel(f, std::min(wanted, total - f));
            if (ImGui::Selectable(label.c_str(), f == first) && f != first)
            {
                first = f;
                applyChange();
            }
        }
        ImGui::EndCombo();
    }

    AudioHost& host_;
    cui::ControlsView<Meta> controls_;
    bool showSettings_ = false;
    bool reopen_ = false;
    bool dirty_ = false;
    double dirtySince_ = 0.0;
    uint savedSize_[2] = {};
    uint resizeTo_[2] = {};
    float meterIn_ = 0.f, meterOut_ = 0.f;
};

} // namespace

int main()
{
    AudioHost host;
    host.load();

    {
        Application app;
        CookbookStandaloneWindow win(app, host);
        win.show();
        // Open the audio after the window is up so the macOS microphone prompt appears on top of it.
        host.open();
        app.exec();
    }

    host.close();
    host.save();
    return 0;
}
