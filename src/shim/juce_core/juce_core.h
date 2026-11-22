// Stand-in for the few JUCE names the vendored REV-246 DSP uses (upstream/plugin/src/dsp), so it builds without JUCE
// and unchanged. Each one follows JUCE's own behaviour; where a result depends on the exact arithmetic (the linear
// smoother, the IIR design) the formulas are JUCE's. Not a general JUCE replacement.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

// JUCE's jassert is a debug-build check that compiles to nothing in a release build (the plugin is built as one).
#define jassert(expression) ((void)0)

namespace juce
{
template <typename T>
struct MathConstants
{
    static constexpr T pi = static_cast<T>(3.141592653589793238L);
    static constexpr T twoPi = static_cast<T>(2 * 3.141592653589793238L);
    static constexpr T halfPi = static_cast<T>(3.141592653589793238L / 2);
    static constexpr T sqrt2 = static_cast<T>(1.4142135623730950488L);
};

template <typename... Types>
void ignoreUnused(Types&&...) noexcept {}

// An exact comparison that says so (JUCE uses it to keep -Wfloat-equal quiet).
template <typename T>
constexpr bool exactlyEqual(T a, T b) { return !(a < b) && !(b < a) && a == b; }

template <typename T>
constexpr T jmin(T a, T b) { return b < a ? b : a; }
template <typename T>
constexpr T jmax(T a, T b) { return a < b ? b : a; }
template <typename T>
constexpr T jlimit(T lo, T hi, T v) { return v < lo ? lo : (hi < v ? hi : v); }

template <typename T>
int roundToInt(T v) { return (int)std::lround(v); }

// The smallest power of two that is >= n (JUCE's nextPowerOfTwo).
inline int nextPowerOfTwo(int n) noexcept
{
    --n;
    n |= (n >> 1);
    n |= (n >> 2);
    n |= (n >> 4);
    n |= (n >> 8);
    n |= (n >> 16);
    return n + 1;
}

// Flush-to-zero for the scope: decaying reverb loops otherwise fall into denormals, which cost 10-100x on the MPC's
// Cortex-A17 (scalar VFP honours them unless FPSCR.FZ is set) as on x86.
class ScopedNoDenormals
{
  public:
    ScopedNoDenormals() noexcept
    {
#if defined(__SSE__)
        saved = _mm_getcsr();
        _mm_setcsr(saved | 0x8040u); // FTZ | DAZ
#elif defined(__aarch64__)
        std::uint64_t v;
        __asm__ volatile("mrs %0, fpcr" : "=r"(v));
        saved = v;
        v |= (1u << 24); // FZ
        __asm__ volatile("msr fpcr, %0" : : "r"(v));
#elif defined(__arm__)
        std::uint32_t v;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
        saved = v;
        v |= (1u << 24); // FZ
        __asm__ volatile("vmsr fpscr, %0" : : "r"(v));
#endif
    }
    ~ScopedNoDenormals() noexcept
    {
#if defined(__SSE__)
        _mm_setcsr((unsigned int)saved);
#elif defined(__aarch64__)
        std::uint64_t v = saved;
        __asm__ volatile("msr fpcr, %0" : : "r"(v));
#elif defined(__arm__)
        std::uint32_t v = (std::uint32_t)saved;
        __asm__ volatile("vmsr fpscr, %0" : : "r"(v));
#endif
    }
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

  private:
    std::uint64_t saved = 0;
};

namespace ValueSmoothingTypes
{
struct Linear {};
} // namespace ValueSmoothingTypes

// juce::SmoothedValue<float, Linear>: a ramp of a fixed number of steps to each new target.
template <typename T, typename Type = ValueSmoothingTypes::Linear>
class SmoothedValue
{
  public:
    SmoothedValue() = default;
    explicit SmoothedValue(T initial) : current(initial), target(initial) {}

    void reset(double sampleRate, double rampSeconds) noexcept
    {
        stepsToTarget = (int)std::floor(rampSeconds * sampleRate);
        setCurrentAndTargetValue(target);
    }

    void setCurrentAndTargetValue(T v) noexcept
    {
        target = current = v;
        countdown = 0;
    }

    void setTargetValue(T v) noexcept
    {
        if (exactlyEqual(v, target))
            return;
        if (stepsToTarget <= 0)
        {
            setCurrentAndTargetValue(v);
            return;
        }
        target = v;
        countdown = stepsToTarget;
        step = (target - current) / (T)countdown;
    }

    bool isSmoothing() const noexcept { return countdown > 0; }
    T getCurrentValue() const noexcept { return current; }
    T getTargetValue() const noexcept { return target; }

    T getNextValue() noexcept
    {
        if (!isSmoothing())
            return target;
        --countdown;
        if (isSmoothing())
            current += step;
        else
            current = target;
        return current;
    }

    T skip(int numSamples) noexcept
    {
        if (numSamples >= countdown)
        {
            setCurrentAndTargetValue(target);
            return target;
        }
        current += step * (T)numSamples;
        countdown -= numSamples;
        return current;
    }

  private:
    T current = 0, target = 0, step = 0;
    int countdown = 0, stepsToTarget = 0;
};

template <typename T>
using LinearSmoothedValue = SmoothedValue<T, ValueSmoothingTypes::Linear>;

namespace dsp
{
namespace IIR
{
// A normalised biquad (a0 = 1) with JUCE's second-order low-pass and high-pass designs.
template <typename T>
struct Coefficients
{
    using Ptr = std::shared_ptr<Coefficients>;
    T b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static Ptr makeLowPass(double sampleRate, T frequency, T Q)
    {
        const auto n = (T)1 / std::tan(MathConstants<T>::pi * frequency / (T)sampleRate);
        const auto nSquared = n * n;
        const auto invQ = (T)1 / Q;
        const auto c1 = (T)1 / ((T)1 + invQ * n + nSquared);
        auto c = std::make_shared<Coefficients>();
        c->b0 = c1;
        c->b1 = c1 * 2;
        c->b2 = c1;
        c->a1 = c1 * 2 * ((T)1 - nSquared);
        c->a2 = c1 * ((T)1 - invQ * n + nSquared);
        return c;
    }

    static Ptr makeHighPass(double sampleRate, T frequency, T Q)
    {
        const auto n = std::tan(MathConstants<T>::pi * frequency / (T)sampleRate);
        const auto nSquared = n * n;
        const auto invQ = (T)1 / Q;
        const auto c1 = (T)1 / ((T)1 + invQ * n + nSquared);
        auto c = std::make_shared<Coefficients>();
        c->b0 = c1;
        c->b1 = c1 * -2;
        c->b2 = c1;
        c->a1 = c1 * 2 * (nSquared - (T)1);
        c->a2 = c1 * ((T)1 - invQ * n + nSquared);
        return c;
    }
};

// Transposed direct form II, as juce::dsp::IIR::Filter::processSample for a second-order section.
template <typename T>
class Filter
{
  public:
    typename Coefficients<T>::Ptr coefficients;

    void reset() noexcept { z1 = z2 = 0; }

    T processSample(T x) noexcept
    {
        const auto& c = *coefficients;
        const auto y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }

  private:
    T z1 = 0, z2 = 0;
};
} // namespace IIR
} // namespace dsp
} // namespace juce
