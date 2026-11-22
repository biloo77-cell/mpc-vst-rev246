// Per-stage cost of the vendored engine on the device: the whole ReverbEngine (bridge + core), the device-rate path
// alone (core + DELAY + LOW PASS, processDeviceRate), the transformer stage, the widener. % of a 128-frame block.
#include "dsp/ReverbEngine.h"
#include "dsp/StereoWidth.h"
#include "dsp/Transformer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
constexpr int block = 128, blocks = 3000;
constexpr double budgetUs = 1e6 * block / 44100.0;
double now() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
} // namespace

int main()
{
    std::vector<float> in(block), l(block), r(block);
    unsigned seed = 1;
    auto noise = [&] {
        for (auto& x : in)
        {
            seed = seed * 1103515245u + 12345u;
            x = ((int)(seed >> 16 & 0x7fff) - 16384) / 131072.0f;
        }
    };
    const char* names = "SMLEN";
    for (int p = 0; p < 5; ++p)
    {
        auto* e = new rv::dsp::ReverbEngine();
        e->prepare(44100.0, block);
        rv::dsp::Settings s;
        s.program = (rv::params::Program)p;
        s.sixteenBit = true;
        e->setSettings(s);
        e->reset();
        double tAll = 0;
        for (int b = 0; b < blocks; ++b)
        {
            noise();
            const double t0 = now();
            e->process(in.data(), l.data(), r.data(), block);
            tAll += now() - t0;
        }
        // The device-rate path alone, for the same span of time (128 x 26373.6 / 44100 = 76.5 device samples a block).
        const int nDev = 76;
        std::vector<float> dIn(nDev), dl(nDev), dr(nDev);
        double tDev = 0;
        for (int b = 0; b < blocks; ++b)
        {
            noise();
            std::copy(in.begin(), in.begin() + nDev, dIn.begin());
            const double t0 = now();
            e->processDeviceRate(dIn.data(), dl.data(), dr.data(), nDev);
            tDev += now() - t0;
        }
        std::printf("%c 16-bit: engine %5.1f%%  device-rate path %5.1f%%  -> bridge + hiss ~%5.1f%%\n", names[p],
                    100 * tAll / blocks / budgetUs, 100 * tDev / blocks / budgetUs, 100 * (tAll - tDev) / blocks / budgetUs);
        delete e;
    }
    {
        rv::dsp::Transformer t;
        t.prepare(44100.0);
        t.setFullScale(1.0f);
        double tt = 0;
        for (int b = 0; b < blocks; ++b)
        {
            noise();
            const double t0 = now();
            for (int i = 0; i < block; ++i)
            {
                l[(size_t)i] = t.processInput(in[(size_t)i]);
                l[(size_t)i] = t.processOutput(0, l[(size_t)i]);
                r[(size_t)i] = t.processOutput(1, in[(size_t)i]);
            }
            tt += now() - t0;
        }
        std::printf("transformer (in + 2 out): %5.1f%%\n", 100 * tt / blocks / budgetUs);
    }
    {
        rv::dsp::StereoWidener w;
        w.prepare(44100.0);
        w.setWidth(150.0f, true);
        double tw = 0;
        for (int b = 0; b < blocks; ++b)
        {
            noise();
            std::copy(in.begin(), in.end(), l.begin());
            std::copy(in.begin(), in.end(), r.begin());
            const double t0 = now();
            w.process(l.data(), r.data(), block);
            tw += now() - t0;
        }
        std::printf("widener at 150: %5.1f%%\n", 100 * tw / blocks / budgetUs);
    }
    return 0;
}
