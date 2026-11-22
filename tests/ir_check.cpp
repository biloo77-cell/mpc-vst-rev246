// Offline check of the MPC engine (src/engine.cpp) through its mpc_engine() interface, as the wrapper drives it:
// 44.1 kHz int16 stereo in 128-frame blocks. For each programme and a few RT steps it renders an impulse response
// (fully wet, FLOAT storage so the tail is not cut by 16-bit truncation), and reports
//   - the reverberation time from the Schroeder decay (T20: -5 to -25 dB, extrapolated to 60 dB) against the panel RT,
//   - non-finite or clipped samples (fail),
//   - the CPU time per 128-frame block on this machine.
// Writes tests/out/ir_<prog>_<rt>.f32 (interleaved float L/R) for listening or plotting.
//   g++ -std=gnu++17 -O2 -Isrc/shim -Iupstream/plugin/src -I<mpc-vst-plugins>/wrapper tests/ir_check.cpp src/engine.cpp
extern "C"
{
#include "engine.h"
}

#include "params/Parameters.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
constexpr int block = 128;
constexpr double sr = 44100.0;

void set(const mpc_engine_t* e, void* inst, const char* key, double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    e->set_param(inst, key, buf);
}

// T20 from the Schroeder integral of the mono energy, in seconds; -1 if the decay never reaches -25 dB.
double t20(const std::vector<float>& l, const std::vector<float>& r)
{
    std::vector<double> edc(l.size() + 1, 0.0);
    for (size_t i = l.size(); i-- > 0;)
        edc[i] = edc[i + 1] + (double)l[i] * l[i] + (double)r[i] * r[i];
    if (edc[0] <= 0.0)
        return -1;
    size_t i5 = 0, i25 = 0;
    for (size_t i = 0; i < l.size(); ++i)
    {
        const double db = 10.0 * std::log10(edc[i] / edc[0] + 1e-30);
        if (!i5 && db <= -5.0)
            i5 = i;
        if (!i25 && db <= -25.0)
        {
            i25 = i;
            break;
        }
    }
    if (!i5 || !i25)
        return -1;
    return 3.0 * (double)(i25 - i5) / sr;
}
} // namespace

int main(int argc, char** argv)
{
    const char* outDir = argc > 1 ? argv[1] : "tests/out";
    const auto* e = mpc_engine();
    int failures = 0;
    double worstUs = 0, totalUs = 0;
    long blocks = 0;
    std::printf("%-4s %-6s %-9s %-9s %-8s %s\n", "prog", "step", "panel RT", "T20", "peak", "result");
    const char* names = "SMLEN";
    for (int prog = 0; prog < 5; ++prog)
    {
        for (int rtStep : {2, 8, 13})
        {
            void* inst = e->create(nullptr);
            set(e, inst, "program", prog);
            set(e, inst, "rt", rtStep);
            set(e, inst, "bits", 0); // FLOAT
            set(e, inst, "xfmr", 0);
            set(e, inst, "mix", 100);
            if (prog == 4)
            {
                set(e, inst, "ampl", 6);  // SPAN 0.3 s
                set(e, inst, "hf", 5);    // LATE RT 1.0 s
            }
            const double panelRt = rv::params::joyNumber((rv::params::Program)prog, rv::params::Joy::rt, rtStep);
            const double seconds = prog == 2 ? 3.0 * panelRt + 1.0 : 3.0 * std::max(panelRt, 1.0) + 1.0;
            const int nBlocks = (int)(seconds * sr / block);
            std::vector<float> L, R;
            L.reserve((size_t)nBlocks * block);
            R.reserve((size_t)nBlocks * block);
            int16_t in[2 * block], out[2 * block];
            bool bad = false;
            int clipped = 0;
            float peak = 0;
            for (int b = 0; b < nBlocks; ++b)
            {
                std::memset(in, 0, sizeof in);
                if (b == 4) // after the engine has settled
                    in[0] = in[1] = 16384; // -6 dBFS impulse
                const auto t0 = std::chrono::steady_clock::now();
                e->process(inst, in, out, block);
                const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                worstUs = std::max(worstUs, us);
                totalUs += us;
                ++blocks;
                for (int i = 0; i < block; ++i)
                {
                    const float a = out[2 * i] / 32768.0f, c = out[2 * i + 1] / 32768.0f;
                    if (!std::isfinite(a) || !std::isfinite(c))
                        bad = true;
                    if (std::abs(out[2 * i]) >= 32767 || std::abs(out[2 * i + 1]) >= 32767)
                        ++clipped;
                    peak = std::max(peak, std::max(std::abs(a), std::abs(c)));
                    L.push_back(a);
                    R.push_back(c);
                }
            }
            e->destroy(inst);
            const double t = t20(L, R);
            // N's decay is two stages by design (SPAN, then LATE), so only the finite and silent checks apply there.
            const bool rtOk = prog == 4 ? t > 0 : t > 0 && std::abs(std::log(t / panelRt)) < std::log(1.6);
            const bool ok = !bad && clipped == 0 && peak > 1e-4f && rtOk;
            failures += !ok;
            std::printf("%-4c %-6d %-9.2f %-9.2f %-8.4f %s%s%s\n", names[prog], rtStep, prog == 4 ? 0.0 : panelRt, t, peak,
                        ok ? "ok" : "FAIL", bad ? " (non-finite)" : "", clipped ? " (clipped)" : "");
            const std::string path = std::string(outDir) + "/ir_" + names[prog] + "_" + std::to_string(rtStep) + ".f32";
            if (FILE* f = std::fopen(path.c_str(), "wb"))
            {
                for (size_t i = 0; i < L.size(); ++i)
                {
                    std::fwrite(&L[i], 4, 1, f);
                    std::fwrite(&R[i], 4, 1, f);
                }
                std::fclose(f);
            }
        }
    }
    // OVL: a 0 dBFS clip light (the port's rule, 2026-11-22): lit by a full-scale input, off just below it (a hot
    // signal at -0.5 dBFS peak, -3.5 dBFS rms: upstream's +6 VU rule would have lit it), held 400 ms.
    {
        auto ovlAfter = [e](double amplitude, double seconds) {
            void* inst = e->create(nullptr);
            int16_t in[2 * block], out[2 * block];
            long t = 0;
            const int n = (int)(seconds * sr / block);
            for (int b = 0; b < n; ++b)
            {
                for (int i = 0; i < block; ++i, ++t)
                    in[2 * i] = in[2 * i + 1] = (int16_t)std::lround(amplitude * 32767.0 * std::sin(2.0 * 3.14159265 * 1000.0 * t / sr));
                e->process(inst, in, out, block);
            }
            char buf[16] = {0};
            e->get_param(inst, "ovl", buf, sizeof buf);
            e->destroy(inst);
            return std::string(buf) == "1";
        };
        const bool quiet = !ovlAfter(0.1, 1.0);           // -23 dBFS rms: off
        const bool hot = !ovlAfter(0.944, 1.0);           // -0.5 dBFS peak: still off
        const bool full = ovlAfter(1.0, 0.1);             // full scale: lit
        void* inst = e->create(nullptr);
        int16_t in[2 * block] = {}, out[2 * block];
        in[0] = in[1] = 32767;
        e->process(inst, in, out, block);
        char buf[16] = {0};
        e->get_param(inst, "ovl", buf, sizeof buf);
        const bool lit = std::string(buf) == "1";
        std::memset(in, 0, sizeof in);
        for (int b = 0; b < (int)(0.38 * sr / block); ++b)
            e->process(inst, in, out, block);
        e->get_param(inst, "ovl", buf, sizeof buf);
        const bool held = std::string(buf) == "1";
        for (int b = 0; b < (int)(0.05 * sr / block); ++b)
            e->process(inst, in, out, block);
        e->get_param(inst, "ovl", buf, sizeof buf);
        const bool released = std::string(buf) == "0";
        e->destroy(inst);
        const bool ok = quiet && hot && full && lit && held && released;
        failures += !ok;
        std::printf("OVL: quiet off %d, -0.5 dBFS off %d, full scale on %d, one-sample peak on %d, held at 380 ms %d, off at 430 ms %d: %s\n",
                    quiet, hot, full, lit, held, released, ok ? "ok" : "FAIL");
    }
    std::printf("CPU per 128-frame block (this machine): mean %.1f us, worst %.1f us (real time: 2902 us)\n",
                totalUs / (double)blocks, worstUs);
    std::printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures != 0;
}
