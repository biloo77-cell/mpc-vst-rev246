// REV-246 for MPC OS: the mpc_engine() glue (wrapper/engine.h) around upstream's reverb DSP, vendored in
// upstream/plugin/src/dsp (a few headers sped up in src/opt, VENDORED.md). SPDX-License-Identifier: AGPL-3.0-or-later
//
// This file is upstream's Rev246Processor (upstream/plugin/src/PluginProcessor.cpp) without JUCE, for MPC's fixed
// stereo in / stereo out at 44.1 kHz in blocks of up to 128 frames. Both of upstream's routings (the MODE switch,
// upstream's ORIGINAL / TRUE ST button): TRUE ST (the port's default, user 2026-11-22; upstream defaults to ORIGINAL)
// runs one engine per input channel and keeps the dry's channels; ORIGINAL sums the input to the unit's mono input,
// one engine, the dry as the panned mono sum. The second engine only runs in TRUE ST (it about doubles the CPU). The same ramps and constants. Left out: the panel (MPC draws its own
// skin), the VU meter's LEDs (OVL is kept), LINK and POWER (MPC has its own insert bypass). INPUT and OUTPUT are upstream's fader
// positions (0..1 along the printed scale, params::gainLinearFromPosition), as the skin's faders draw them.
// Parameters are applied at the start of each block, as upstream applies them at the start of each processBlock.
extern "C"
{
#include "engine.h"
}

#include "dsp/Ducker.h"
#include "dsp/ReverbEngine.h"
#include "dsp/StereoWidth.h"
#include "dsp/Transformer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace
{
namespace params = rv::params;

constexpr double sampleRate = 44100.0; // MPC's rate (wrapper/engine.h)
constexpr int maxBlock = 128;          // MPC's period

// Upstream's constants (PluginProcessor.cpp).
constexpr double rampSeconds = 0.02;
constexpr float monoSumGain = 0.5f;
constexpr float switchOnThreshold = 0.5f;
// The modelled A/D's full scale (a peak in the input domain; the bridge's clip, the cores' 16-bit storage scale, the
// hiss, the transformer cores' drive): 0 dBFS, so the converter clips where the MPC does. Upstream derives it from its
// meter's CAL (adcFullScaleForZeroVu, default -18 dBFS = clip at -9 dBFS, which lit OVL on ordinary MPC levels); the
// port has no meter and no CAL (user, 2026-11-22). The same as CAL at -9.03 dBFS.
constexpr float adcFullScale = 1.0f;
// OVL: an input sample (either side, after INPUT) at 0 dBFS, or the A/D out of range (also 0 dBFS); held 400 ms.
// Upstream also lights it with its VU at +6 over 0 VU (an RMS level): left out, the LED reads as a 0 dBFS clip light
// (user, 2026-11-22).
constexpr float ovlPeak = 0.999f;
constexpr double ovlHoldSeconds = 0.4;

enum Param
{
    pProgram,
    pLowpass, // the six joysticks, in params::Joy order
    pDelay,
    pAmpl,
    pLf,
    pRt,
    pHf,
    pMix,
    pPan,
    pWidth,
    pIn,
    pOut,
    pSixteenBit,
    pTransformer,
    pDuck,
    pDuckRange,
    pDuckThreshold,
    pDuckAttack,
    pDuckRelease,
    pMode, // 0 TRUE ST, 1 ORIGINAL (L+R summed)
    pCount
};
const char* const keys[pCount] = {"program", "lowpass", "delay", "ampl", "lf", "rt", "hf",
                                  "mix", "pan", "width", "in_gain", "out_gain", "bits", "xfmr",
                                  "duck", "duck_range", "duck_thresh", "duck_attack", "duck_release",
                                  "mode"};
// Upstream's initial state (DEVELOPMENT.md, Initial/factory defaults): S, LP 12, DELAY 10 ms, AMPL 80 %, LF x1.0,
// RT 1.0 s, HF x0.9, fully wet, 0 dB, width 100, 16-bit and XFMR on, ducker off at its defaults. MODE: TRUE ST here
// (upstream ORIGINAL; user 2026-11-22).
const double defaults[pCount] = {0, 7, 10, 8, 9, 8, 8, 100, 0, 100, params::defaultGainPosition, params::defaultGainPosition,
                                 1, 1, 0, -10, -20, 10, 200,
#ifdef REV246_BENCH_ORIGINAL // a bench build (tools/bench.sh sets no parameters): ORIGINAL from the start
                                 1};
#else
                                 0};
#endif

int keyIndex(const char* k)
{
    for (int i = 0; i < pCount; ++i)
        if (std::strcmp(k, keys[i]) == 0)
            return i;
    return -1;
}

float faderGain(double position) { return params::gainLinearFromPosition((float)position); }

// The engine's unused-side skip (src/opt's ReverbEngine::setOutputs); a no-op on upstream's own engine (the reference
// build of tests/render.cpp, without src/opt), which always makes both sides.
template <typename E>
auto setEngineOutputs(E& e, bool left, bool right, int) -> decltype(e.setOutputs(left, right), void())
{
    e.setOutputs(left, right);
}
template <typename E>
void setEngineOutputs(E&, bool, bool, long)
{
}

// Upstream's dry pan: constant power, unity (not -3 dB) at the centre.
struct PanGains
{
    float left, right;
};
PanGains panGains(float pan)
{
    const auto angle = (juce::jlimit(-1.0f, 1.0f, pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    return {std::cos(angle) * juce::MathConstants<float>::sqrt2, std::sin(angle) * juce::MathConstants<float>::sqrt2};
}

struct EngineKey
{
    int program = -1;
    bool sixteenBit = false;
    std::array<int, params::numJoys> steps{};
    bool operator==(const EngineKey& o) const
    {
        return program == o.program && sixteenBit == o.sixteenBit && steps == o.steps;
    }
};

struct Instance
{
    double v[pCount];

    // engines[1] / transformers[1]: TRUE ST's right side (idle, and not sent settings, in ORIGINAL).
    rv::dsp::ReverbEngine engines[2];
    rv::dsp::Transformer transformers[2];
    rv::dsp::StereoWidener stereoWidener;
    rv::dsp::Ducker ducker;
    juce::LinearSmoothedValue<float> inGain, outGain, mixAmount, panLeft, panRight, transformerOn;
    // MODE switch (upstream's routeGain / dryStereo): the wet dips to silence over 10 ms, the routing changes there and
    // the wet comes back; the dry crossfades between the panned mono sum and its own channels over 20 ms.
    // feed1 (the port's own): the second engine's input fades in over 10 ms after its reset, so it does not start on a
    // signal cut in mid-wave (a click on the right side 27 ms after the switch, measured offline; upstream has none).
    // feed0Left (also the port's own): engine 0's input crossfades between the mono sum and the left side over 10 ms
    // at a switch, for the same reason in both directions.
    juce::LinearSmoothedValue<float> routeGain, dryStereo, feed1, feed0Left;
    bool trueStereo = false; // the routing in use
    bool started = false;    // no block processed yet: a MODE from the restored state applies at once, with no dip
    EngineKey engineKey;
    rv::dsp::Settings engineSettings; // what engine 0 was last sent (engine 1's on a switch to TRUE ST)
    static constexpr int settingsHoldBlocks = 3;
    int settingsHold = 0; // blocks before the engine takes another settings change
    bool transformerWasIdle = false;
    // The dry (input x INPUT gain, each side) delayed by the engine's latency, as upstream delays its dry.
    std::vector<float> dryDelayL, dryDelayR;
    size_t dryPos = 0;
    int ovlLeft = 0;         // samples OVL stays lit
    bool ovlReported = false; // what the host was last told (mpc_engine_changed)

    // One block's buffers (upstream's scratch channels).
    float engineIn0[maxBlock], engineIn1[maxBlock], wetL0[maxBlock], wetR0[maxBlock], wetL1[maxBlock], wetR1[maxBlock];
    float transformerWeight[maxBlock], duckKey[maxBlock], dryL[maxBlock], dryR[maxBlock];

    Instance()
    {
        std::memcpy(v, defaults, sizeof v);
        for (auto& e : engines)
            e.prepare(sampleRate, maxBlock);
        for (auto& t : transformers)
            t.prepare(sampleRate);
        stereoWidener.prepare(sampleRate);
        stereoWidener.setWidth((float)v[pWidth], true);
        ducker.prepare(sampleRate);
        for (auto* s : {&inGain, &outGain, &mixAmount, &panLeft, &panRight, &transformerOn, &dryStereo})
            s->reset(sampleRate, rampSeconds);
        for (auto* s : {&routeGain, &feed1, &feed0Left})
        {
            s->reset(sampleRate, 0.5 * rampSeconds);
            s->setCurrentAndTargetValue(1.0f);
        }
        inGain.setCurrentAndTargetValue(faderGain(v[pIn]));
        outGain.setCurrentAndTargetValue(faderGain(v[pOut]));
        mixAmount.setCurrentAndTargetValue((float)(v[pMix] / 100.0));
        const auto pan = panGains((float)(v[pPan] / 100.0));
        panLeft.setCurrentAndTargetValue(pan.left);
        panRight.setCurrentAndTargetValue(pan.right);
        transformerOn.setCurrentAndTargetValue(on(pTransformer) ? 1.0f : 0.0f);
        trueStereo = step(pMode) == 0;
        dryStereo.setCurrentAndTargetValue(trueStereo ? 1.0f : 0.0f);
        feed0Left.setCurrentAndTargetValue(trueStereo ? 1.0f : 0.0f);
        setEngineOutputs(engines[1], false, true, 0); // engine 1 only ever feeds the right side
        applyOutputs();
        engineKey = {};
        applyParameters();
        settingsHold = 0; // the host's first settings (a restored state, a preset) apply at the first block
        ducker.reset();
        for (auto& e : engines)
            e.reset();
        const auto latency = (size_t)std::max(1, engines[0].latency()); // the same for every program, mode and engine
        dryDelayL.assign(latency, 0.0f);
        dryDelayR.assign(latency, 0.0f);
        dryPos = 0;
    }

    // Upstream's processChunk routing logic: a MODE change waits for the wet's dip to end, then resets the second
    // engine and its transformer (no old tail) and sends the settings again.
    void updateRouting()
    {
        const bool want = step(pMode) == 0;
        if (!started)
        {
            if (want != trueStereo)
                engineKey = {}; // engine 1 (reset at creation) gets the settings in this block
            trueStereo = want;
            dryStereo.setCurrentAndTargetValue(want ? 1.0f : 0.0f);
            feed0Left.setCurrentAndTargetValue(want ? 1.0f : 0.0f);
            applyOutputs();
            return;
        }
        dryStereo.setTargetValue(want ? 1.0f : 0.0f);
        if (want == trueStereo)
            routeGain.setTargetValue(1.0f); // (also: switched back during the dip)
        else if (routeGain.getTargetValue() > 0.0f)
            routeGain.setTargetValue(0.0f);
        else if (!routeGain.isSmoothing())
        {
            // Only engine 1 needs anything, and only on the way to TRUE ST (in ORIGINAL it idles): a reset (no old
            // tail) and engine 0's settings. Upstream resets it both ways and re-sends both engines their settings.
            if (want)
            {
                engines[1].reset();
                transformers[1].reset();
                engines[1].setSettings(engineSettings);
            }
            trueStereo = want;
            applyOutputs(); // on the way to ORIGINAL, engine 0's right side restarts from silence here, under the dip
            routeGain.setTargetValue(1.0f);
            feed1.setCurrentAndTargetValue(0.0f);
            feed1.setTargetValue(1.0f);
            feed0Left.setTargetValue(want ? 1.0f : 0.0f);
        }
    }

    // TRUE ST reads engine 0's left side only; ORIGINAL both.
    void applyOutputs() { setEngineOutputs(engines[0], true, !trueStereo, 0); }

    bool on(int p) const { return v[p] >= switchOnThreshold; }
    int step(int p) const { return (int)std::lround(v[p]); }
    params::Program program() const { return (params::Program)juce::jlimit(0, params::numPrograms - 1, step(pProgram)); }

    // Panel settings -> the engine's settings and the ramps' targets (upstream's applyParameters).
    void applyParameters()
    {
        EngineKey key;
        key.program = (int)program();
        key.sixteenBit = on(pSixteenBit);
        for (int i = 0; i < params::numJoys; ++i) // a step past the program's last (N's longer HF column) reads as the last
            key.steps[(size_t)i] = juce::jlimit(0, params::joyMaxStep((params::Program)key.program, (params::Joy)i),
                                                step(pLowpass + i));
        // The cores' coefficient laws cost up to ~3 % of a block on a Gen1 MPC (setSettings): a change applies at once,
        // but further ones within settingsHoldBlocks blocks (8.7 ms) wait for the next free block, so a fast Q-Link turn
        // or automation sweep costs one recomputation per 3 blocks, not one per block. The unit's own held lever
        // stepped every ~5 ms (ENGINE.md); the last value always lands.
        if (settingsHold > 0)
            --settingsHold;
        else if (!(key == engineKey))
        {
            settingsHold = settingsHoldBlocks - 1;
            engineKey = key;
            const auto p = (params::Program)key.program;
            auto num = [&](params::Joy j) { return params::joyNumber(p, j, key.steps[(size_t)j]); };
            rv::dsp::Settings s;
            s.program = p;
            s.steps = key.steps;
            s.lowpassKHz = num(params::Joy::lowpass);
            s.delayMs = num(params::Joy::delay);
            s.amplPercent = num(params::Joy::ampl);
            s.lfX = num(params::Joy::lf);
            s.rt = num(params::Joy::rt);
            s.hfX = num(params::Joy::hf);
            s.sixteenBit = key.sixteenBit;
            engineSettings = s;
            engines[0].setSettings(s);
            if (trueStereo) // engine 1 idles otherwise; a switch to TRUE ST resets it and sends it these
                engines[1].setSettings(s);
        }

        inGain.setTargetValue(faderGain(v[pIn]));
        outGain.setTargetValue(faderGain(v[pOut]));
        mixAmount.setTargetValue((float)(v[pMix] / 100.0));
        transformerOn.setTargetValue(on(pTransformer) ? 1.0f : 0.0f);
        ducker.setEnabled(on(pDuck));
        ducker.setRangeDb((float)v[pDuckRange]);
        ducker.setThresholdDb((float)v[pDuckThreshold]);
        ducker.setAttackMs((float)v[pDuckAttack]);
        ducker.setReleaseMs((float)v[pDuckRelease]);
        for (auto& e : engines) // (returns at once when unchanged)
            e.setFullScale(adcFullScale);
        for (auto& t : transformers)
            t.setFullScale(adcFullScale);
        const auto pan = panGains((float)(v[pPan] / 100.0));
        panLeft.setTargetValue(pan.left);
        panRight.setTargetValue(pan.right);
    }


    // The wet's two sides after processChunk: engine 0's read-outs in ORIGINAL; engine 0's left and engine 1's right
    // in TRUE ST (upstream's wetL0 / wetR1).
    float* wetLeft() { return wetL0; }
    float* wetRight() { return trueStereo ? wetR1 : wetR0; }

    // Upstream's processChunk (stereo in / stereo out); writes the wet (after duck and width) into wetLeft / wetRight
    // and the delayed dry (after INPUT, each side) into dryL / dryR.
    void processChunk(const int16_t* x, int n)
    {
        updateRouting();
        applyParameters();

        // The input stage: INPUT, the level for OVL, the duck key, the input transformer(s); the dry into its latency
        // rings. ORIGINAL feeds engine 0 the mono sum; TRUE ST feeds each engine its own side.
        // XFMR: upstream runs the stage at all times (a warm state for the toggle's crossfade). Here it is skipped while
        // off and settled (its weight is exactly 0, so the output is the same), and reset when it comes back on: the
        // 20 ms crossfade covers its start from rest. Saves ~0.9 % of a Gen1 core with XFMR off.
        const bool transformerIdle = !transformerOn.isSmoothing() && transformerOn.getTargetValue() <= 0.0f;
        if (transformerWasIdle && !transformerIdle)
            for (auto& t : transformers)
                t.reset();
        transformerWasIdle = transformerIdle;
        const bool st = trueStereo;
        float peak = 0.0f;
        const auto ringSize = dryDelayL.size();
        auto ringPos = dryPos;
        for (int i = 0; i < n; ++i)
        {
            const auto g = inGain.getNextValue();
            const auto transformerMix = transformerOn.getNextValue();
            const auto l = x[2 * i] * (1.0f / 32768.0f) * g, r = x[2 * i + 1] * (1.0f / 32768.0f) * g;
            dryL[i] = dryDelayL[ringPos];
            dryR[i] = dryDelayR[ringPos];
            dryDelayL[ringPos] = l;
            dryDelayR[ringPos] = r;
            ringPos = ringPos + 1 == ringSize ? 0 : ringPos + 1;
            duckKey[i] = std::max(std::abs(l), std::abs(r));
            peak = std::max(peak, duckKey[i]);
            transformerWeight[i] = transformerMix;
            // engine 0: the mono sum (ORIGINAL) or the left side (TRUE ST), crossfaded for 10 ms after a switch
            const auto mono = monoSumGain * (l + r);
            const auto fl = feed0Left.getNextValue();
            const auto x0 = fl <= 0.0f ? mono : fl >= 1.0f ? l : mono + fl * (l - mono);
            engineIn0[i] = transformerIdle ? x0 : x0 + transformerMix * (transformers[0].processInput(x0) - x0);
            if (st)
            {
                const auto r1 = r * feed1.getNextValue();
                engineIn1[i] = transformerIdle ? r1 : r1 + transformerMix * (transformers[1].processInput(r1) - r1);
            }
        }
        dryPos = ringPos;

        engines[0].process(engineIn0, wetL0, wetR0, n);
        if (st)
            engines[1].process(engineIn1, wetL1, wetR1, n);

        const bool over0 = engines[0].takeOverload(), over1 = engines[1].takeOverload();
        if (over0 || (st && over1) || peak >= ovlPeak)
            ovlLeft = (int)(ovlHoldSeconds * sampleRate);
        else
            ovlLeft = std::max(0, ovlLeft - n);

        // The output transformers (one per side in TRUE ST; both read-outs on the one in ORIGINAL), duck, then width.
        float *wl = wetLeft(), *wr = wetRight();
        auto& transformerR = st ? transformers[1] : transformers[0];
        for (int i = 0; i < n && !transformerIdle; ++i)
        {
            const auto w = transformerWeight[i];
            const auto xl = transformers[0].processOutput(0, wl[i]);
            const auto xr = transformerR.processOutput(1, wr[i]);
            wl[i] += w * (xl - wl[i]);
            wr[i] += w * (xr - wr[i]);
        }
        ducker.process(duckKey, wl, wr, n);
        stereoWidener.setWidth((float)v[pWidth]);
        stereoWidener.process(wl, wr, n);
    }

    void process(const int16_t* in, int16_t* out, int frames)
    {
        juce::ScopedNoDenormals noDenormals;
        for (int start = 0; start < frames; start += maxBlock)
        {
            const int n = std::min(maxBlock, frames - start);
            processChunk(in + 2 * start, n);
            started = true;
            int16_t* y = out + 2 * start;
            // Dry / wet mix and OUTPUT (upstream's mixOutput, stereo out): TRUE ST keeps the dry's channels, ORIGINAL pans
            // the mono sum; the wet through the routing dip.
            const float *wl = wetLeft(), *wr = wetRight();
            for (int i = 0; i < n; ++i)
            {
                const auto mix = mixAmount.getNextValue();
                const auto outputGain = outGain.getNextValue();
                const auto panL = panLeft.getNextValue(), panR = panRight.getNextValue();
                const auto route = routeGain.getNextValue();
                const auto stw = dryStereo.getNextValue(); // 0 or 1 at rest: exactly one form, no rounding from the blend
                const auto dryMono = monoSumGain * (dryL[i] + dryR[i]);
                auto blend = [stw, dryMono](float own) { return stw >= 1.0f ? own : stw <= 0.0f ? dryMono : dryMono + stw * (own - dryMono); };
                y[2 * i] = toInt16(((1.0f - mix) * blend(dryL[i]) * panL + mix * route * wl[i]) * outputGain);
                y[2 * i + 1] = toInt16(((1.0f - mix) * blend(dryR[i]) * panR + mix * route * wr[i]) * outputGain);
            }
        }
    }

    // One joystick's value as the display shows it: the number alone (one decimal below 10), blank where the
    // program has no such value (N's RT).
    void joyNumberText(params::Joy j, char* out, size_t n) const
    {
        const auto p = program();
        const int st = juce::jlimit(0, params::joyMaxStep(p, j), step(pLowpass + (int)j));
        if (j == params::Joy::rt && p == params::Program::n)
        {
            std::snprintf(out, n, "--");
            return;
        }
        const double x = params::joyNumber(p, j, st);
        if (j == params::Joy::delay || (j == params::Joy::ampl && p != params::Program::n) || j == params::Joy::lowpass)
            std::snprintf(out, n, "%d", (int)std::lround(x));
        else if (j == params::Joy::ampl) // N: SPAN in seconds, two decimals
            std::snprintf(out, n, "%.2f", x);
        else
        {
            const double r = std::round(std::abs(x) * 10.0) / 10.0;
            if (r < 9.95)
                std::snprintf(out, n, "%s%.1f", x < 0.0 ? "-" : "", r);
            else
                std::snprintf(out, n, "%s%d", x < 0.0 ? "-" : "", (int)std::lround(r));
        }
    }

    // 'l': LOWPASS; 'r': DELAY and AMPL (SPAN in N); 'd': LF, RT, HF (EARLY, -, LATE in N).
    int lcdText(char which, char* buf, int len) const
    {
        char a[16], b[16], c[16];
        using J = params::Joy;
        if (which == 'l')
        {
            joyNumberText(J::lowpass, a, sizeof a);
            return std::snprintf(buf, (size_t)len, "%s", a);
        }
        if (which == 'r')
        {
            joyNumberText(J::delay, a, sizeof a);
            joyNumberText(J::ampl, b, sizeof b);
            return std::snprintf(buf, (size_t)len, "%s   %s", a, b);
        }
        joyNumberText(J::lf, a, sizeof a);
        joyNumberText(J::rt, b, sizeof b);
        joyNumberText(J::hf, c, sizeof c);
        return std::snprintf(buf, (size_t)len, "%s   %s   %s", a, b, c);
    }

    static int16_t toInt16(float s)
    {
        if (!(s == s))
            return 0;
        const auto v = std::lround(s * 32768.0f);
        return (int16_t)std::clamp(v, -32768L, 32767L);
    }

    int getState(char* buf, int len) const
    {
        int n = 0;
        for (int i = 0; i < pCount && n < len; ++i)
            n += std::snprintf(buf + n, (size_t)(len - n), "%s=%.9g;", keys[i], v[i]);
        return n < len ? n : 0;
    }

    void setState(const char* val)
    {
        char k[32];
        const char* p = val;
        while (*p)
        {
            const char *eq = std::strchr(p, '='), *end = std::strchr(p, ';');
            if (!eq || !end || eq > end || eq - p >= (long)sizeof k)
                break;
            std::memcpy(k, p, (size_t)(eq - p));
            k[eq - p] = 0;
            const int i = keyIndex(k);
            if (i >= 0)
                v[i] = std::atof(eq + 1);
            p = end + 1;
        }
    }
};

void* create(const char*)
{
    return new (std::nothrow) Instance();
}

void destroy(void* inst) { delete static_cast<Instance*>(inst); }

void midi(void*, const uint8_t*, int) {}

void setParam(void* inst, const char* key, const char* val)
{
    auto* s = static_cast<Instance*>(inst);
    if (std::strcmp(key, "state") == 0)
    {
        s->setState(val);
        return;
    }
    const int i = keyIndex(key);
    if (i >= 0)
        s->v[i] = std::atof(val);
}

int getParam(void* inst, const char* key, char* buf, int len)
{
    auto* s = static_cast<Instance*>(inst);
    if (std::strcmp(key, "state") == 0)
        return s->getState(buf, len);
    if (std::strcmp(key, "ovl") == 0) // a read-only light (an Off / On option); never part of the state
        return std::snprintf(buf, (size_t)len, "%d", s->ovlLeft > 0 ? 1 : 0);
    // The three value displays' text, as upstream's seven-segment fields read (the units are printed under them).
    if (std::strcmp(key, "lcd_lp") == 0 || std::strcmp(key, "lcd_refl") == 0 || std::strcmp(key, "lcd_decay") == 0)
        return s->lcdText(key[4], buf, len);
    // "<joy>_name" / "<joy>_display": what the column is and reads in the current program (N relabels three).
    const char* us = std::strrchr(key, '_');
    if (us)
    {
        char base[32];
        const auto bl = (size_t)(us - key);
        if (bl < sizeof base)
        {
            std::memcpy(base, key, bl);
            base[bl] = 0;
            const int i = keyIndex(base);
            const bool joy = i >= pLowpass && i <= pHf;
            if (joy && std::strcmp(us, "_name") == 0)
                return std::snprintf(buf, (size_t)len, "%s", params::joyName(s->program(), (params::Joy)(i - pLowpass)));
            if (joy && std::strcmp(us, "_display") == 0)
                return params::joyText(s->program(), (params::Joy)(i - pLowpass), s->step(i), buf, len);
            if ((i == pIn || i == pOut) && std::strcmp(us, "_display") == 0)
                return params::gainText((float)s->v[i], buf, len);
        }
    }
    const int i = keyIndex(key);
    if (i < 0)
        return 0;
    return std::snprintf(buf, (size_t)len, "%.9g", s->v[i]);
}

void render(void*, int16_t* out, int frames) { std::memset(out, 0, (size_t)frames * 2 * sizeof *out); }

void process(void* inst, const int16_t* in, int16_t* out, int frames) { static_cast<Instance*>(inst)->process(in, out, frames); }

// OVL is an Off / On parameter the engine changes by itself: report it when it flips, so MPC redraws the LED
// (wrapper/engine.h; asked once per host block under the engine lock).
int changedParam(void* inst, char* key, int len)
{
    auto* s = static_cast<Instance*>(inst);
    const bool lit = s->ovlLeft > 0;
    if (lit == s->ovlReported)
        return 0;
    s->ovlReported = lit;
    std::snprintf(key, (size_t)len, "ovl");
    return 1;
}

const mpc_engine_t engineApi = {create, destroy, midi, setParam, getParam, render, process, nullptr};
} // namespace

extern "C" const mpc_engine_t* mpc_engine(void) { return &engineApi; }
extern "C" int mpc_engine_changed(void* inst, char* key, int len) { return changedParam(inst, key, len); }
