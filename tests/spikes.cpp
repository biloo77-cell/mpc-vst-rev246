// Cost of the one-off operations behind the sweep's worst blocks, on the device: ReverbEngine::reset(), and
// setSettings() within each programme for every RT / LF / HF / AMPL step. In us and % of a 128-frame block.
#include "dsp/ReverbEngine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
constexpr double budgetUs = 1e6 * 128 / 44100.0;
double now() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
} // namespace

int main()
{
    namespace p = rv::params;
    auto* e = new rv::dsp::ReverbEngine();
    e->prepare(44100.0, 128);
    std::vector<float> in(128, 0.01f), l(128), r(128);
    double worstReset = 0;
    for (int k = 0; k < 20; ++k)
    {
        e->process(in.data(), l.data(), r.data(), 128);
        const double t0 = now();
        e->reset();
        worstReset = std::max(worstReset, now() - t0);
    }
    std::printf("reset(): worst %.0f us (%.0f%%)\n", worstReset, 100 * worstReset / budgetUs);
    const char* names = "SMLEN";
    for (int prog = 0; prog < 5; ++prog)
    {
        rv::dsp::Settings s;
        s.program = (p::Program)prog;
        e->setSettings(s);
        e->reset();
        double worst = 0;
        const char* worstWhat = "";
        int worstStep = 0;
        const p::Joy joys[] = {p::Joy::rt, p::Joy::lf, p::Joy::hf, p::Joy::ampl};
        const char* joyNames[] = {"rt", "lf", "hf", "ampl"};
        for (int j = 0; j < 4; ++j)
            for (int step = 0; step <= p::joyMaxStep(s.program, joys[j]); ++step)
            {
                auto t = s;
                t.steps[(size_t)joys[j]] = step;
                const double v = p::joyNumber(s.program, joys[j], step);
                if (joys[j] == p::Joy::rt) t.rt = v;
                if (joys[j] == p::Joy::lf) t.lfX = v;
                if (joys[j] == p::Joy::hf) t.hfX = v;
                if (joys[j] == p::Joy::ampl) t.amplPercent = v;
                const double t0 = now();
                e->setSettings(t);
                const double dt = now() - t0;
                if (dt > worst)
                {
                    worst = dt;
                    worstWhat = joyNames[j];
                    worstStep = step;
                }
                e->process(in.data(), l.data(), r.data(), 128);
            }
        std::printf("%c setSettings: worst %.0f us (%.0f%%) at %s step %d\n", names[prog], worst, 100 * worst / budgetUs,
                    worstWhat, worstStep);
    }
    delete e;
    return 0;
}
