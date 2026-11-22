// Renders fixed test signals through the MPC engine (float output, before the int16 cut, by calling the vendored engine
// the way src/engine.cpp does is not needed: the int16 path is what ships, so this goes through mpc_engine()), one file
// per case, for tests/null_cmp.cpp to compare two builds (upstream's exact bridge and the port's src/opt bridge).
//   render <out dir>
// Cases: every programme in FLOAT and 16-BIT, TRUE ST, XFMR off, WIDTH 150; input: 1 s of noise bursts at -12 dBFS
// with a 200 Hz / 9 kHz sine pair, then the tail.
extern "C"
{
#include "engine.h"
}

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr int block = 128;
void set(const mpc_engine_t* e, void* inst, const char* key, double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    e->set_param(inst, key, buf);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    const auto* e = mpc_engine();
    struct Case
    {
        const char* name;
        int program, bits, unused, xfmr, width;
    };
    const Case cases[] = {{"S_float", 0, 0, 0, 1, 100}, {"M_float", 1, 0, 0, 1, 100}, {"L_float", 2, 0, 0, 1, 100},
                          {"E_float", 3, 0, 0, 1, 100}, {"N_float", 4, 0, 0, 1, 100}, {"S_16", 0, 1, 0, 1, 100},
                          {"M_16", 1, 1, 0, 1, 100},    {"L_16", 2, 1, 0, 1, 100},    {"E_16", 3, 1, 0, 1, 100},
                          {"N_16", 4, 1, 0, 1, 100},    {"M_noxfmr", 1, 0, 0, 0, 150}};
    for (const auto& c : cases)
    {
        void* inst = e->create(nullptr);
        set(e, inst, "program", c.program);
        set(e, inst, "bits", c.bits);
        set(e, inst, "xfmr", c.xfmr);
        set(e, inst, "width", c.width);
        set(e, inst, "mix", 100);
        const int blocks = (int)(4.0 * 44100 / block);
        std::vector<int16_t> all;
        unsigned seed = 7;
        long t = 0;
        for (int b = 0; b < blocks; ++b)
        {
            int16_t in[2 * block], out[2 * block];
            for (int i = 0; i < block; ++i, ++t)
            {
                double x = 0, y = 0;
                if (t < 44100)
                {
                    seed = seed * 1103515245u + 12345u;
                    const double n = ((int)(seed >> 16 & 0x7fff) - 16384) / 16384.0;
                    const double env = (t / 4410) % 2 == 0 ? 1.0 : 0.0; // 100 ms bursts
                    x = 0.25 * (env * n + 0.5 * std::sin(2 * M_PI * 200 * t / 44100.0)) + 0.1 * std::sin(2 * M_PI * 9000 * t / 44100.0);
                    y = 0.25 * (env * n - 0.5 * std::sin(2 * M_PI * 200 * t / 44100.0));
                }
                in[2 * i] = (int16_t)std::lround(x * 32767);
                in[2 * i + 1] = (int16_t)std::lround(y * 32767);
            }
            e->process(inst, in, out, block);
            all.insert(all.end(), out, out + 2 * block);
        }
        e->destroy(inst);
        if (FILE* f = std::fopen((dir + "/" + c.name + ".s16").c_str(), "wb"))
        {
            std::fwrite(all.data(), sizeof(int16_t), all.size(), f);
            std::fclose(f);
        }
    }
    return 0;
}
