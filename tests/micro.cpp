// Micro-benchmarks of every stage of the engine on the device, each in a tight loop, scaled to its calls per 128-frame
// host block at 44.1 kHz (76.55 device samples), as % of the block's 2902 us. Test-only: opens the classes' privates
// (the standard headers first, so the macro reaches only the engine's own classes).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <juce_core/juce_core.h>
#define private public
#include "dsp/ReverbEngine.h"
#include "dsp/Transformer.h"
#include "dsp/StereoWidth.h"
#include "dsp/Ducker.h"
#undef private

#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
constexpr double budgetUs = 1e6 * 128 / 44100.0;
constexpr double devPerBlock = 128.0 * 26373.626373 / 44100.0;
double now() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
volatile double sink;

template <typename F>
double perCallUs(F&& f, int reps = 200000)
{
    for (int i = 0; i < reps / 10; ++i)
        f(i);
    const double t0 = now();
    for (int i = 0; i < reps; ++i)
        f(i);
    return (now() - t0) / reps;
}
void line(const char* what, double us, double callsPerBlock)
{
    std::printf("%-40s %8.3f us/call x %6.1f = %5.2f%%\n", what, us, callsPerBlock, 100 * us * callsPerBlock / budgetUs);
}
} // namespace

int main()
{
    using namespace rv::dsp;
    DeviceBridge br;
    br.prepare(44100.0, 128);
    std::printf("bridge: wi %d, wo %d, in modes %d, out modes %d\n", br.wi, br.wo, br.inFilter.numModes, br.outFilter.numModes);
    std::vector<double> x(64);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = 0.001 * (double)(i % 7);
    BridgeFilter::State st;
    BridgeFilter::Weights w;
    double acc = 0;
    line("in head", perCallUs([&](int i) { acc += br.inFilter.head(x.data() + (i & 7), (i & 1023) / 1024.0); }), devPerBlock);
    line("in tail(u) (weights + sum)", perCallUs([&](int i) { acc += br.inFilter.tail(st, (i & 1023) / 1024.0); }), devPerBlock);
    line("in push", perCallUs([&](int i) { br.inFilter.push(st, x[(size_t)(i & 31)]); }), 128);
    line("out tailWeights", perCallUs([&](int i) { br.outFilter.tailWeights((i & 1023) / 1024.0, w); acc += w.re[0]; }), 128);
    line("out head", perCallUs([&](int i) { acc += br.outFilter.head(x.data() + (i & 7), (i & 1023) / 1024.0); }), 256);
    line("out tail(w)", perCallUs([&](int) { acc += br.outFilter.tail(st, w); }), 256);
    BridgeFilter::Weights w2;
    line("out shifted", perCallUs([&](int) { br.outFilter.shifted(w, br.shiftL, w2); acc += w2.re[1]; }), 128);
    line("out push", perCallUs([&](int i) { br.outFilter.push(st, x[(size_t)(i & 31)]); }), 2 * devPerBlock);
    line("correction (HP + 65-tap FIR)", perCallUs([&](int i) { acc += br.correct(i & 1, x[(size_t)(i & 31)]); }), 2 * devPerBlock);
    {
        int16_t dummy = 0;
        (void)dummy;
        std::vector<float> in(128), l(128), r(128);
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.01f * (float)((i * 7) % 13);
        line("whole bridge, identity core", perCallUs([&](int) {
                 br.process(in.data(), l.data(), r.data(), 128, [](const float* di, float* dl, float* dr, int n) {
                     for (int k = 0; k < n; ++k)
                         dl[k] = dr[k] = di[k];
                 });
             }, 3000), 1);
    }
    // Cores at the device rate, 16-bit and float, 77 samples a call.
    {
        std::vector<float> in(77), l(77), r(77);
        unsigned seed = 3;
        for (auto& v : in)
        {
            seed = seed * 1103515245u + 12345u;
            v = ((int)(seed >> 16 & 0x7fff) - 16384) / 65536.0f;
        }
        SCore s;
        MLCore m{rv::params::Program::m}, lc{rv::params::Program::l};
        ECore e;
        NCore n;
        ProgramCore* cores[] = {&s, &m, &lc, &e, &n};
        const char* names[] = {"core S", "core M", "core L", "core E", "core N"};
        for (int bits = 0; bits < 2; ++bits)
            for (int c = 0; c < 5; ++c)
            {
                cores[c]->prepare();
                Settings set;
                set.program = (rv::params::Program)c;
                set.sixteenBit = bits;
                cores[c]->setSettings(set);
                cores[c]->reset();
                char label[48];
                std::snprintf(label, sizeof label, "%s %s", names[c], bits ? "16-bit" : "float");
                line(label, perCallUs([&](int) { cores[c]->process(in.data(), l.data(), r.data(), 77); }, 4000), devPerBlock / 77);
            }
    }
    {
        OutputDelay d;
        d.prepare(26400);
        d.setDelay(264);
        std::vector<float> l(77, 0.1f), r(77, 0.1f);
        line("output DELAY", perCallUs([&](int) { d.process(l.data(), r.data(), 77); }, 20000), devPerBlock / 77);
        DeviceLowPass lp;
        lp.setCutoff(8.0);
        line("LOW PASS at 8 kHz", perCallUs([&](int) { lp.process(l.data(), r.data(), 77); }, 20000), devPerBlock / 77);
        AnalogueHiss h;
        h.prepare(44100.0);
        std::vector<float> a(128), b(128);
        line("hiss (16-bit)", perCallUs([&](int) { h.add(a.data(), b.data(), 128, true); }, 20000), 1);
    }
    {
        Transformer t;
        t.prepare(44100.0);
        t.setFullScale(1.0f);
        line("transformer in", perCallUs([&](int i) { acc += t.processInput(0.01f * (float)(i & 15)); }), 128);
        line("transformer out (per channel)", perCallUs([&](int i) { acc += t.processOutput(i & 1, 0.01f * (float)(i & 15)); }), 256);
    }
    sink = acc;
    return 0;
}
