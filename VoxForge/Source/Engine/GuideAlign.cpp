#include "GuideAlign.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace vf
{

std::vector<float> warpUnitsOntoGuide (const float* guide, int tg, const float* speech, int ts, int dim, float* meanCost)
{
    std::vector<float> out ((size_t) std::max (0, tg) * (size_t) std::max (0, dim), 0.0f);
    if (tg <= 0 || dim <= 0) return out;
    if (ts <= 0)
    {
        std::copy (guide, guide + (size_t) tg * (size_t) dim, out.begin());
        return out;
    }

    // unit-normalised copies for cosine distance
    auto normalised = [dim] (const float* x, int n)
    {
        std::vector<float> y (x, x + (size_t) n * (size_t) dim);
        for (int i = 0; i < n; ++i)
        {
            double s = 0.0;
            for (int k = 0; k < dim; ++k) s += (double) y[(size_t) i * dim + k] * y[(size_t) i * dim + k];
            const float inv = s > 1.0e-12 ? (float) (1.0 / std::sqrt (s)) : 0.0f;
            for (int k = 0; k < dim; ++k) y[(size_t) i * dim + k] *= inv;
        }
        return y;
    };
    const auto g = normalised (guide, tg);
    const auto s = normalised (speech, ts);

    // accumulated cost with steps (1,1) (1,0) (0,1); the diagonal is not penalised so a
    // straight match is preferred, repeated frames cost a little extra to avoid "stuck" paths
    const float inf = std::numeric_limits<float>::infinity();
    const size_t W = (size_t) ts;
    std::vector<float> D ((size_t) tg * W, inf);
    std::vector<unsigned char> step ((size_t) tg * W, 0);
    auto cost = [&] (int i, int j)
    {
        const float* a = &g[(size_t) i * dim];
        const float* b = &s[(size_t) j * dim];
        float dot = 0.0f;
        for (int k = 0; k < dim; ++k) dot += a[k] * b[k];
        return 1.0f - dot;
    };
    constexpr float repeatPenalty = 0.02f;
    for (int i = 0; i < tg; ++i)
        for (int j = 0; j < ts; ++j)
        {
            const float c = cost (i, j);
            if (i == 0 && j == 0) { D[0] = c; continue; }
            float best = inf; unsigned char st = 0;
            if (i > 0 && j > 0 && D[(size_t) (i - 1) * W + (size_t) (j - 1)] < best) { best = D[(size_t) (i - 1) * W + (size_t) (j - 1)]; st = 1; }
            if (i > 0 && D[(size_t) (i - 1) * W + (size_t) j] + repeatPenalty < best) { best = D[(size_t) (i - 1) * W + (size_t) j] + repeatPenalty; st = 2; }
            if (j > 0 && D[(size_t) i * W + (size_t) (j - 1)] + repeatPenalty < best) { best = D[(size_t) i * W + (size_t) (j - 1)] + repeatPenalty; st = 3; }
            D[(size_t) i * W + (size_t) j] = best + c;
            step[(size_t) i * W + (size_t) j] = st;
        }

    // backtrack and average the speech frames matched to each guide frame
    std::vector<int> count ((size_t) tg, 0);
    int i = tg - 1, j = ts - 1, pathLen = 0;
    for (;;)
    {
        float* o = &out[(size_t) i * dim];
        const float* sp = speech + (size_t) j * dim;
        for (int k = 0; k < dim; ++k) o[k] += sp[k];
        ++count[(size_t) i];
        ++pathLen;
        const auto st = step[(size_t) i * W + (size_t) j];
        if (st == 0) break;
        if (st == 1) { --i; --j; }
        else if (st == 2) --i;
        else --j;
    }
    for (int f = 0; f < tg; ++f)
        if (count[(size_t) f] > 1)
            for (int k = 0; k < dim; ++k) out[(size_t) f * dim + k] /= (float) count[(size_t) f];
    if (meanCost != nullptr) *meanCost = D[(size_t) (tg - 1) * W + (size_t) (ts - 1)] / (float) std::max (1, pathLen);
    return out;
}

std::pair<int, int> trimSilence (const float* x, int n, float thresholdDb, int pad)
{
    if (n <= 0) return { 0, 0 };
    // 10 ms RMS envelope
    const int hop = 160;
    float peak = 0.0f;
    std::vector<float> env;
    for (int i = 0; i < n; i += hop)
    {
        double e = 0.0;
        const int m = std::min (hop, n - i);
        for (int k = 0; k < m; ++k) e += (double) x[i + k] * x[i + k];
        env.push_back ((float) std::sqrt (e / std::max (1, m)));
        peak = std::max (peak, env.back());
    }
    if (peak <= 1.0e-5f) return { 0, 0 };
    const float thr = peak * std::pow (10.0f, thresholdDb / 20.0f);
    int a = 0, b = (int) env.size() - 1;
    while (a < b && env[(size_t) a] < thr) ++a;
    while (b > a && env[(size_t) b] < thr) --b;
    return { std::max (0, a * hop - pad), std::min (n, (b + 1) * hop + pad) };
}

} // namespace vf
