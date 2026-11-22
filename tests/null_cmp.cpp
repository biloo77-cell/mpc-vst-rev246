// Compares tests/render.cpp's outputs from two builds: for each case, the difference's energy over the reference's
// (dB), over the whole file and in the tail windows the upstream tests use (0-50, 50-150, 150-300, 300-600, 600-1000 ms
// after the input stops), plus the largest sample difference in 16-bit steps. Both are int16, as MPC gets them.
//   null_cmp <reference dir> <test dir> <case>...
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static std::vector<int16_t> load(const std::string& p)
{
    std::vector<int16_t> v;
    if (FILE* f = std::fopen(p.c_str(), "rb"))
    {
        int16_t b[4096];
        size_t n;
        while ((n = std::fread(b, 2, 4096, f)) > 0)
            v.insert(v.end(), b, b + n);
        std::fclose(f);
    }
    return v;
}

static double db(double e, double r) { return r > 0 ? 10 * std::log10(e / r + 1e-30) : -999; }

int main(int argc, char** argv)
{
    int bad = 0;
    std::printf("%-10s %8s | tail windows after the input stops (ms): %6s %6s %6s %6s %6s | max diff\n", "case", "whole",
                "0-50", "-150", "-300", "-600", "-1000");
    for (int a = 3; a < argc; ++a)
    {
        const auto r = load(std::string(argv[1]) + "/" + argv[a] + ".s16");
        const auto t = load(std::string(argv[2]) + "/" + argv[a] + ".s16");
        if (r.empty() || r.size() != t.size())
        {
            std::printf("%-10s missing or different length\n", argv[a]);
            ++bad;
            continue;
        }
        auto window = [&](size_t from, size_t to) {
            double e = 0, s = 0;
            for (size_t i = 2 * from; i < 2 * to && i < r.size(); ++i)
            {
                const double d = (double)t[i] - r[i];
                e += d * d;
                s += (double)r[i] * r[i];
            }
            return db(e, s);
        };
        int maxd = 0;
        for (size_t i = 0; i < r.size(); ++i)
            maxd = std::max(maxd, std::abs((int)t[i] - (int)r[i]));
        const size_t stop = 44100, ms = 441 / 10;
        const double whole = window(0, r.size() / 2);
        std::printf("%-10s %8.1f | %41.1f %6.1f %6.1f %6.1f %6.1f | %d\n", argv[a], whole, window(stop, stop + 50 * ms),
                    window(stop + 50 * ms, stop + 150 * ms), window(stop + 150 * ms, stop + 300 * ms),
                    window(stop + 300 * ms, stop + 600 * ms), window(stop + 600 * ms, stop + 1000 * ms), maxd);
        // Level, not waveform: the test's energy over the reference's in each window (a 16-bit tail is chaotic, so two
        // builds that differ by a rounding diverge; what must match is how loud the tail and its grit are).
        auto level = [&](size_t from, size_t to) {
            double et = 0, er = 0;
            for (size_t i = 2 * from; i < 2 * to && i < r.size(); ++i)
            {
                et += (double)t[i] * t[i];
                er += (double)r[i] * r[i];
            }
            return er > 0 ? 10 * std::log10((et + 1e-9) / er) : 0.0;
        };
        std::printf("%-10s %8s   level (test / reference, dB): %12.2f %6.2f %6.2f %6.2f %6.2f\n", "", "",
                    level(stop, stop + 50 * ms), level(stop + 50 * ms, stop + 150 * ms),
                    level(stop + 150 * ms, stop + 300 * ms), level(stop + 300 * ms, stop + 600 * ms),
                    level(stop + 600 * ms, stop + 1000 * ms));
    }
    return bad;
}
