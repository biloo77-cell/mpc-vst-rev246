// The MPC port's replacement for upstream/plugin/src/params/Parameters.{h,cpp}. The vendored DSP includes
// "params/Parameters.h" for Program and the counts; upstream's file also declares the JUCE parameter layout, which the
// port does not have (its parameters are in vst/params.json). The step tables, the fader scale and the value laws
// below are upstream's, copied unchanged from Parameters.cpp; only the text is formatted with snprintf instead of
// juce::String. If upstream changes a table, change it here (VENDORED.md).
#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace rv::params
{
enum class Program
{
    s,
    m,
    l,
    e,
    n,
    count
};

// The joystick columns, left to right.
enum class Joy
{
    lowpass,
    delay,
    ampl,
    lf,
    rt,
    hf,
    count
};

inline constexpr int numPrograms = (int)Program::count;
inline constexpr int numJoys = (int)Joy::count;

inline constexpr float duckMinRangeDb = -60.0f, duckMaxRangeDb = 0.0f, duckDefaultRangeDb = -10.0f;
inline constexpr float duckMinThresholdDb = -80.0f, duckMaxThresholdDb = 0.0f, duckDefaultThresholdDb = -20.0f;
inline constexpr float duckMaxTimeMs = 1000.0f;
inline constexpr float duckDefaultAttackMs = 10.0f, duckDefaultReleaseMs = 200.0f;

namespace detail
{
// Tick positions measured on upstream's panel artwork (design pixels): the cap centre at each printed value.
struct Tick
{
    float db, y;
};
inline constexpr std::array<Tick, 11> gainTicks{{{12.f, 218.f},
                                                 {6.f, 350.f},
                                                 {0.f, 480.f},
                                                 {-5.f, 593.f},
                                                 {-10.f, 688.f},
                                                 {-15.f, 782.f},
                                                 {-20.f, 872.f},
                                                 {-30.f, 982.f},
                                                 {-40.f, 1076.f},
                                                 {-50.f, 1160.f},
                                                 {-100.f, 1238.f}}}; // the bottom is -infinity
inline constexpr float faderTopY = 218.f, faderBottomY = 1238.f;

inline constexpr std::array<double, 8> lowpassStepsKHz{2, 3, 4, 5, 6, 8, 10, 12};
inline constexpr std::array<double, 16> lfStepsX{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8,
                                                 0.9, 1.0, 1.1, 1.2, 1.3, 1.5, 1.8, 2.0};
inline constexpr int hfStepCount = 10;     // HF x 0.1 ... 1.0 in S / M / L / E
inline constexpr int delayMaxStepMs = 999; // DELAY: 0 ... 999 ms in 1 ms steps
inline constexpr int amplMaxStep = 10;     // AMPL: 0 ... 100 % (N: span 0 ... 0.5 s) in 11 steps

using RtTable = std::array<double, 16>;
inline constexpr RtTable rtStepsS{0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0, 2.2, 2.5};
inline constexpr RtTable rtStepsM{0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.2, 1.3, 1.5, 1.8, 2.2, 2.6, 3.0, 3.5, 4.0, 5.0};
inline constexpr RtTable rtStepsL{2.0, 2.2, 2.6, 3.1, 3.6, 4.0, 4.5, 5.2, 6.0, 6.9, 8.0, 10.0, 12.0, 15.0, 17.0, 20.0};
inline constexpr RtTable rtStepsE{0.4, 0.6, 0.8, 1.0, 1.2, 1.5, 1.8, 2.0, 2.2, 2.5, 2.8, 3.0, 3.3, 3.6, 4.0, 4.5};
inline constexpr int rtDefaultStep = 8; // 1.0 s in S

// N: EARLY RT in the LF slot (10 = flat), LATE RT in the HF slot (0 ... 2.0 s by 0.2), SPAN in the AMPL slot.
inline constexpr std::array<double, 16> nEarlySteps{-3.0, -2.0, -1.0, -0.7, -0.5, -0.2, 0.1, 0.2,
                                                    0.3,  0.5,  0.7,  1.0,  1.5,  2.0,  3.0, 10.0};
inline constexpr int nLateSteps = 11;
inline constexpr double nLateStepS = 0.2;

inline const RtTable& rtTable(Program p)
{
    switch (p)
    {
    case Program::m: return rtStepsM;
    case Program::l: return rtStepsL;
    case Program::e: return rtStepsE;
    case Program::s:
    case Program::n:
    case Program::count:
    default: return rtStepsS;
    }
}
} // namespace detail

// The parameter's highest step index (all start at 0), over every program.
inline int joyMaxStep(Joy j)
{
    static constexpr int maxima[] = {(int)detail::lowpassStepsKHz.size() - 1, detail::delayMaxStepMs,
                                     detail::amplMaxStep, (int)detail::lfStepsX.size() - 1,
                                     (int)detail::RtTable{}.size() - 1, detail::nLateSteps - 1};
    return maxima[(int)j];
}

// This program's highest step (HF: 9 in S / M / L / E, 10 in N); a higher one reads as it.
inline int joyMaxStep(Program p, Joy j)
{
    if (j == Joy::hf && p != Program::n)
        return detail::hfStepCount - 1;
    return joyMaxStep(j);
}

// The reference setting (S): LP 12, DELAY 10 ms, AMPL 80 %, LF x1.0, RT 1.0 s, HF x0.9.
inline int joyDefaultStep(Joy j)
{
    static constexpr int defaults[] = {7, 10, 8, 9, detail::rtDefaultStep, 8};
    return defaults[(int)j];
}

inline double joyNumber(Program p, Joy j, int step) noexcept
{
    step = juce::jlimit(0, joyMaxStep(p, j), step);
    const bool n = p == Program::n;
    switch (j)
    {
    case Joy::lowpass: return detail::lowpassStepsKHz[(size_t)step];
    case Joy::delay: return step;
    case Joy::ampl: return n ? step * 0.05 : step * 10.0;                                     // N: SPAN 0 to 0.5 s
    case Joy::lf: return n ? detail::nEarlySteps[(size_t)step] : detail::lfStepsX[(size_t)step]; // N: EARLY RT
    case Joy::rt: return n ? 0.0 : detail::rtTable(p)[(size_t)step];                          // N has no RT
    case Joy::hf: return n ? step * detail::nLateStepS : (step + 1) * 0.1;                    // N: LATE RT
    case Joy::count:
    default: return 0.0;
    }
}

// What the column is called in this program (N relabels three of them and has no RT).
inline const char* joyName(Program p, Joy j)
{
    const bool n = p == Program::n;
    switch (j)
    {
    case Joy::lowpass: return "Lowpass";
    case Joy::delay: return "Delay";
    case Joy::ampl: return n ? "Span" : "Ampl";
    case Joy::lf: return n ? "Early RT" : "LF x";
    case Joy::rt: return n ? "(RT)" : "RT";
    case Joy::hf: return n ? "Late RT" : "HF x";
    case Joy::count:
    default: return "";
    }
}

// The value as the unit's display prints it (one decimal below 10, whole numbers from 10 up), with its unit.
inline int joyText(Program p, Joy j, int step, char* buf, int len)
{
    step = juce::jlimit(0, joyMaxStep(p, j), step);
    const auto v = joyNumber(p, j, step);
    const bool n = p == Program::n;
    char num[24];
    const auto rounded = std::round(std::abs(v) * 10.0) / 10.0;
    if (rounded < 9.95)
        std::snprintf(num, sizeof num, "%s%.1f", v < 0.0 ? "-" : "", rounded);
    else
        std::snprintf(num, sizeof num, "%s%d", v < 0.0 ? "-" : "", (int)std::lround(rounded));
    switch (j)
    {
    case Joy::lowpass: return std::snprintf(buf, (size_t)len, "%d kHz", (int)v);
    case Joy::delay: return std::snprintf(buf, (size_t)len, "%d ms", step);
    case Joy::ampl: return n ? std::snprintf(buf, (size_t)len, "%.2f s", v) : std::snprintf(buf, (size_t)len, "%d %%", (int)v);
    case Joy::lf: return std::snprintf(buf, (size_t)len, n ? "%s s" : "x %s", num);
    case Joy::rt: return n ? std::snprintf(buf, (size_t)len, "--") : std::snprintf(buf, (size_t)len, "%s s", num);
    case Joy::hf: return std::snprintf(buf, (size_t)len, n ? (step == 0 ? "off" : "%s s") : "x %s", num);
    case Joy::count:
    default: return 0;
    }
}

//==============================================================================
// Fader mapping: the parameter value 0..1 is the position along the fader's travel; the scale is the printed one.
inline constexpr float gainOffDb = -99.f;                 // at or below this the fader reads -infinity (linear gain 0)
inline constexpr float defaultGainPosition = 0.74313725f; // 0 dB

inline float gainDbFromPosition(float position)
{
    using namespace detail;
    const float y = faderBottomY - juce::jlimit(0.f, 1.f, position) * (faderBottomY - faderTopY);
    for (size_t i = 1; i < gainTicks.size(); ++i)
        if (y <= gainTicks[i].y)
        {
            const auto& a = gainTicks[i - 1];
            const auto& b = gainTicks[i];
            return a.db + (b.db - a.db) * (y - a.y) / (b.y - a.y);
        }
    return gainTicks.back().db;
}

inline float gainLinearFromPosition(float position)
{
    const auto db = gainDbFromPosition(position);
    return db <= gainOffDb ? 0.f : std::pow(10.0f, db * 0.05f);
}

inline int gainText(float position, char* buf, int len)
{
    const auto db = gainDbFromPosition(position);
    return db <= gainOffDb ? std::snprintf(buf, (size_t)len, "-inf dB") : std::snprintf(buf, (size_t)len, "%.1f dB", db);
}
} // namespace rv::params
