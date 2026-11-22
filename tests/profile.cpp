// Per-case CPU cost of the MPC engine, to run on the device (static armv7 build, /tmp, normal priority):
// each programme in FLOAT and 16-BIT, and the costly events a Q-Link sweep makes (a setting change every
// block, a programme switch). Prints mean and p99 per 128-frame block as % of the 2902 us budget.
extern "C"
{
#include "engine.h"
}

#include "dsp/ReverbEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
constexpr int block = 128;
constexpr double budgetUs = 1e6 * block / 44100.0;

void set(const mpc_engine_t* e, void* inst, const char* key, double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    e->set_param(inst, key, buf);
}

struct Stats
{
    double mean, p99, max;
};

template <typename Before>
Stats run(const mpc_engine_t* e, void* inst, int blocks, Before before)
{
    int16_t in[2 * block], out[2 * block];
    std::vector<double> us;
    us.reserve((size_t)blocks);
    unsigned seed = 1;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < 2 * block; ++i)
        {
            seed = seed * 1103515245u + 12345u;
            in[i] = (int16_t)((int)(seed >> 16 & 0x7fff) - 16384) / 4; // noise around -18 dBFS
        }
        before(b);
        const auto t0 = std::chrono::steady_clock::now();
        e->process(inst, in, out, block);
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    std::vector<double> s = us;
    std::sort(s.begin(), s.end());
    double sum = 0;
    for (double v : us)
        sum += v;
    return {100.0 * sum / (double)blocks / budgetUs, 100.0 * s[(size_t)(0.99 * (double)(blocks - 1))] / budgetUs,
            100.0 * s.back() / budgetUs};
}

void report(const char* name, Stats s) { std::printf("%-34s mean %5.1f%%  p99 %5.1f%%  max %5.1f%%\n", name, s.mean, s.p99, s.max); }
} // namespace

int main()
{
    const auto* e = mpc_engine();
    const int n = 2000; // ~5.8 s per case
    const char* names[] = {"S", "M", "L", "E", "N"};
    for (int bits = 0; bits < 2; ++bits)
        for (int p = 0; p < 5; ++p)
        {
            void* inst = e->create(nullptr);
            set(e, inst, "program", p);
            set(e, inst, "bits", bits);
            char label[64];
            std::snprintf(label, sizeof label, "%s %s", names[p], bits ? "16-bit" : "float");
            report(label, run(e, inst, n, [](int) {}));
            e->destroy(inst);
        }
    if (std::getenv("QUICK"))
        return 0;
    {
        void* inst = e->create(nullptr);
        set(e, inst, "program", 2);
        report("L, RT changed every block", run(e, inst, n, [&](int b) { set(e, inst, "rt", b % 16); }));
        set(e, inst, "program", 4);
        report("N, LF/HF/AMPL changed every block", run(e, inst, n, [&](int b) {
                   set(e, inst, "lf", b % 16);
                   set(e, inst, "hf", b % 11);
                   set(e, inst, "ampl", b % 11);
               }));
        report("programme switch every 12 blocks", run(e, inst, n, [&](int b) {
                   if (b % 12 == 0)
                       set(e, inst, "program", (b / 12) % 5);
               }));
        report("WIDTH 150 (crossover on)", run(e, inst, n, [&](int b) {
                   if (b == 0)
                       set(e, inst, "width", 150);
               }));
        e->destroy(inst);
    }
    return 0;
}
