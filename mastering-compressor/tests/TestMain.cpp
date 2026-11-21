// Lumen MC-64 — DSP verification suite (no JUCE required).
// Build: cmake -B build -DLUMEN_BUILD_PLUGIN=OFF && cmake --build build && ctest --test-dir build

#include <lumen/CompressorEngine.h>

#include <complex>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace lumen;

// ---------------------------------------------------------------------------------------------
// tiny test harness
namespace
{
struct TestCase
{
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}

int failures = 0;
const char* currentTest = "";

struct Registrar
{
    Registrar (const char* n, std::function<void()> f) { registry().push_back ({ n, std::move (f) }); }
};

#define TEST(name)                                     \
    static void name();                                \
    static Registrar registrar_##name (#name, name);   \
    static void name()

#define CHECK(cond, ...)                                                               \
    do {                                                                               \
        if (! (cond)) {                                                                \
            ++failures;                                                                \
            std::printf ("  FAIL [%s] %s:%d: %s — ", currentTest, __FILE__, __LINE__, #cond); \
            std::printf (__VA_ARGS__);                                                 \
            std::printf ("\n");                                                        \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, tol) CHECK (std::abs ((a) - (b)) <= (tol), "%s = %.9g, expected %.9g (tol %.3g)", #a, (double) (a), (double) (b), (double) (tol))

// ---------------------------------------------------------------------------------------------
// helpers
struct Stereo
{
    std::vector<double> l, r;
    explicit Stereo (size_t n = 0) : l (n, 0.0), r (n, 0.0) {}
    size_t size() const { return l.size(); }
};

std::unique_ptr<CompressorEngine> makeEngine (double fs, const CompressorParams& p, int block = 512)
{
    auto e = std::make_unique<CompressorEngine>();
    e->setParams (p);
    e->prepare (fs, block);
    e->setParams (p);
    return e;
}

void run (CompressorEngine& e, Stereo& s, int block = 512, const Stereo* sc = nullptr)
{
    for (size_t pos = 0; pos < s.size(); pos += static_cast<size_t> (block))
    {
        const int n = static_cast<int> (std::min (s.size() - pos, static_cast<size_t> (block)));
        double* io[2] = { s.l.data() + pos, s.r.data() + pos };
        if (sc != nullptr)
        {
            const double* scp[2] = { sc->l.data() + pos, sc->r.data() + pos };
            e.process (io, 2, scp, 2, n);
        }
        else
        {
            e.process (io, 2, nullptr, 0, n);
        }
    }
}

Stereo sine (double fs, double freq, double amp, size_t n, double phaseR = 0.0)
{
    Stereo s (n);
    for (size_t i = 0; i < n; ++i)
    {
        s.l[i] = amp * std::sin (2.0 * kPi * freq * static_cast<double> (i) / fs);
        s.r[i] = amp * std::sin (2.0 * kPi * freq * static_cast<double> (i) / fs + phaseR);
    }
    return s;
}

Stereo noise (size_t n, double amp, unsigned seed = 1)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> d (0.0, amp);
    Stereo s (n);
    for (size_t i = 0; i < n; ++i)
    {
        s.l[i] = d (rng);
        s.r[i] = d (rng);
    }
    return s;
}

double rmsDb (const std::vector<double>& v, size_t from, size_t to)
{
    double acc = 0.0;
    for (size_t i = from; i < to; ++i)
        acc += v[i] * v[i];
    return 10.0 * std::log10 (acc / static_cast<double> (to - from) + 1e-300);
}

double peakAbs (const std::vector<double>& v, size_t from, size_t to)
{
    double p = 0.0;
    for (size_t i = from; i < to; ++i)
        p = std::max (p, std::abs (v[i]));
    return p;
}

/** Magnitude (dB re full-scale sine) of one frequency via a Hann-windowed DFT bin. */
double toneLevelDb (const std::vector<double>& v, size_t from, size_t len, double fs, double freq)
{
    std::complex<double> acc = 0.0;
    double wsum = 0.0;
    for (size_t i = 0; i < len; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * static_cast<double> (i) / static_cast<double> (len));
        const double ph = -2.0 * kPi * freq * static_cast<double> (i) / fs;
        acc += w * v[from + i] * std::complex<double> (std::cos (ph), std::sin (ph));
        wsum += w;
    }
    return 20.0 * std::log10 (2.0 * std::abs (acc) / wsum + 1e-300);
}

CompressorParams transparentDefaults()
{
    CompressorParams p;
    p.mode = EngineMode::ultraTransparent;
    p.scHpfOn = false;
    p.lookaheadMs = 0.0;
    p.kneeDb = 0.0;
    return p;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// 1. static transfer curve
TEST (transferCurveHardKnee)
{
    for (double ratio : { 1.0, 2.0, 4.0, 10.0, 20.0 })
        for (double thr : { -40.0, -20.0, -6.0 })
            for (double in = -60.0; in <= 0.0; in += 1.0)
            {
                const double out = GainComputer::staticOutput (in, thr, ratio, 0.0);
                const double expected = in <= thr ? in : thr + (in - thr) / ratio;
                CHECK_NEAR (out, expected, 1e-12);
            }
}

TEST (transferCurveSoftKneeContinuity)
{
    const double thr = -20.0, ratio = 4.0;
    for (double kneeDb : { 1.0, 6.0, 12.0, 24.0 })
    {
        // value continuity at knee edges and monotonic slope between 1 and 1/R
        const double lo = thr - kneeDb / 2, hi = thr + kneeDb / 2;
        CHECK_NEAR (GainComputer::staticOutput (lo, thr, ratio, kneeDb), lo, 1e-12);
        CHECK_NEAR (GainComputer::staticOutput (hi, thr, ratio, kneeDb), thr + (hi - thr) / ratio, 1e-12);
        double prevSlope = 1.0;
        for (double x = lo; x < hi; x += 0.01)
        {
            const double slope = (GainComputer::staticOutput (x + 1e-6, thr, ratio, kneeDb)
                                  - GainComputer::staticOutput (x, thr, ratio, kneeDb)) / 1e-6;
            CHECK (slope <= prevSlope + 1e-6 && slope >= 1.0 / ratio - 1e-6, "slope %.6f at %.2f", slope, x);
            prevSlope = slope;
        }
    }
    CHECK (GainComputer::feedForward (-3.0, -20.0, 1.0, 6.0) == 0.0, "ratio 1 must be exactly 0 dB");
}

TEST (feedbackCurveMatchesFeedForward)
{
    // Closed-loop fixed point of the feedback computer must equal the feed-forward curve.
    for (double ratio : { 2.0, 4.0, 8.0 })
        for (double kneeDb : { 0.0, 6.0 })
            for (double in = -40.0; in <= 0.0; in += 2.0)
            {
                // solve y = in + G_fb(y) by bisection
                double lo = -200.0, hi = in;
                for (int it = 0; it < 200; ++it)
                {
                    const double mid = 0.5 * (lo + hi);
                    (mid - in - GainComputer::feedBack (mid, -20.0, ratio, kneeDb) > 0.0 ? hi : lo) = mid;
                }
                const double y = 0.5 * (lo + hi);
                if (kneeDb == 0.0)
                    CHECK_NEAR (y, GainComputer::staticOutput (in, -20.0, ratio, 0.0), 1e-9);
                else // soft knee: identical outside the knee region
                    if (in < -20.0 - 3.0 * ratio || in > -20.0 + 3.0 * ratio)
                        CHECK_NEAR (y, GainComputer::staticOutput (in, -20.0, ratio, 0.0), 1e-9);
            }
}

TEST (engineSteadyStateMatchesCurve)
{
    // Steady sine through the full engine: output level must sit on the static curve.
    const double fs = 48000.0;
    for (auto det : { DetectionMode::peak, DetectionMode::rms })
        for (double ratio : { 2.0, 4.0, 10.0 })
        {
            CompressorParams p = transparentDefaults();
            p.thresholdDb = -20.0;
            p.ratio = ratio;
            // peak detection: a near-instant attack with long release holds the cycle peak, so the
            // steady state sits on the curve; RMS reads the AES-17 level and is timing independent
            p.attackMs = det == DetectionMode::peak ? 0.05 : 5.0;
            p.releaseMs = 2000.0;
            p.detection = det;
            p.mode = EngineMode::digital;
            auto e = makeEngine (fs, p);
            const double inDb = -6.0;
            Stereo s = sine (fs, 1000.0, dbToGain (inDb), static_cast<size_t> (fs * 3));
            run (*e, s);
            const double outPeakDb = 20.0 * std::log10 (peakAbs (s.l, s.size() - 4800, s.size()));
            const double expected = GainComputer::staticOutput (inDb, -20.0, ratio, 0.0);
            CHECK_NEAR (outPeakDb, expected, 0.1);
        }
}

// ---------------------------------------------------------------------------------------------
// 2. attack / release timing
TEST (attackAndReleaseTimeConstants)
{
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.mode = EngineMode::digital;
    p.thresholdDb = -30.0;
    p.ratio = 20.0;
    p.attackMs = 10.0;
    p.releaseMs = 100.0;
    p.detection = DetectionMode::peak;
    auto e = makeEngine (fs, p);

    // DC step (peak detection on DC == exact level): 0 dBFS for 1 s, then -60 dBFS
    const size_t n = static_cast<size_t> (fs * 2);
    Stereo s (n);
    for (size_t i = 0; i < n; ++i)
        s.l[i] = s.r[i] = i < n / 2 ? 1.0 : 0.001;
    run (*e, s);

    // final GR (dB) during the step = 30 * (1 - 1/20) = 28.5 dB
    const double grFinal = 28.5;
    auto grAt = [&] (size_t i) { return -20.0 * std::log10 (std::abs (s.l[i]) / (i < n / 2 ? 1.0 : 0.001)); };
    const size_t tauA = static_cast<size_t> (0.010 * fs);
    CHECK_NEAR (grAt (tauA), grFinal * (1.0 - std::exp (-1.0)), 0.3);
    CHECK_NEAR (grAt (n / 2 - 1), grFinal, 0.01);

    const size_t tauR = static_cast<size_t> (0.100 * fs);
    CHECK_NEAR (grAt (n / 2 + tauR), grFinal * std::exp (-1.0), 0.3);
}

TEST (autoReleaseIsProgramDependent)
{
    // A short transient must recover faster than sustained compression.
    const double fs = 48000.0;
    BallisticsSettings b;
    b.attackMs = 1.0;
    b.releaseMs = 500.0;
    b.autoRelease = true;

    auto recoveryTime = [&] (double burstMs) {
        DigitalEnvelope env;
        env.prepare (fs);
        env.configure (b);
        const int burst = static_cast<int> (burstMs * 0.001 * fs);
        double v = 0.0;
        for (int i = 0; i < burst; ++i)
            v = env.process (10.0);
        const double start = v;
        int t = 0;
        while (v > start * 0.1 && t < fs * 10)
        {
            v = env.process (0.0);
            ++t;
        }
        return t / fs;
    };
    const double shortRec = recoveryTime (5.0);
    const double longRec = recoveryTime (3000.0);
    CHECK (shortRec < longRec * 0.4, "short %.3f s vs long %.3f s", shortRec, longRec);
}

// ---------------------------------------------------------------------------------------------
// 3. gain-reduction smoothness
TEST (gainReductionSmoothness)
{
    // A drum-like burst train: the gain trajectory must have no jumps larger than what a
    // one-pole attack can produce in one sample; Ultra mode must be smoother than Digital.
    const double fs = 48000.0;
    const size_t n = static_cast<size_t> (fs * 2);
    Stereo src (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double env = std::exp (-static_cast<double> (i % 12000) / 1200.0);
        src.l[i] = src.r[i] = 0.9 * env * std::sin (2.0 * kPi * 80.0 * i / fs);
    }

    auto maxSecondDiff = [&] (EngineMode m) {
        CompressorParams p = transparentDefaults();
        p.mode = m;
        p.thresholdDb = -24.0;
        p.ratio = 4.0;
        p.attackMs = 2.0;
        p.releaseMs = 150.0;
        p.detection = DetectionMode::rms;
        p.channelMode = ChannelMode::stereo;
        auto e = makeEngine (fs, p);
        // feed DC-offset carrier to read gain directly: use a constant probe on R? Instead measure
        // the gain trajectory via the meter-free method: process src on L and constant on R with
        // 100 % link so R reveals the linked gain.
        Stereo s = src;
        for (size_t i = 0; i < n; ++i)
            s.r[i] = 1.0e-3; // probe (far below threshold, so it never drives the detector)
        run (*e, s);
        double worst = 0.0, worstStep = 0.0;
        for (size_t i = 2; i < n; ++i)
        {
            const double g0 = s.r[i - 2], g1 = s.r[i - 1], g2 = s.r[i];
            worst = std::max (worst, std::abs (g2 - 2 * g1 + g0) / 1.0e-3);
            worstStep = std::max (worstStep, std::abs (g2 - g1) / 1.0e-3);
        }
        return std::make_pair (worst, worstStep);
    };
    const auto digital = maxSecondDiff (EngineMode::digital);
    const auto ultra = maxSecondDiff (EngineMode::ultraTransparent);
    CHECK (digital.second < 0.02, "max per-sample gain step %.5f", digital.second);
    CHECK (ultra.first < digital.first, "ultra curvature %.3g should be < digital %.3g", ultra.first, digital.first);
}

// ---------------------------------------------------------------------------------------------
// 4. stereo link
TEST (stereoLinkBehaviour)
{
    const double fs = 48000.0;
    const size_t n = static_cast<size_t> (fs * 2);
    auto measure = [&] (double link, ChannelMode cm) {
        CompressorParams p = transparentDefaults();
        p.mode = EngineMode::digital;
        p.thresholdDb = -20.0;
        p.ratio = 4.0;
        p.attackMs = 5.0;
        p.releaseMs = 1000.0;
        p.stereoLink = link;
        p.channelMode = cm;
        p.detection = DetectionMode::rms;
        auto e = makeEngine (fs, p);
        Stereo s (n);
        for (size_t i = 0; i < n; ++i)
        {
            s.l[i] = 0.5 * std::sin (2.0 * kPi * 1000.0 * i / fs); // -6 dBFS -> 10.5 dB GR
            s.r[i] = 0.01 * std::sin (2.0 * kPi * 1000.0 * i / fs); // -40 dBFS -> no GR
        }
        run (*e, s);
        const double grL = -6.0206 - 20 * std::log10 (peakAbs (s.l, n - 4800, n));
        const double grR = -40.0 - 20 * std::log10 (peakAbs (s.r, n - 4800, n));
        return std::make_pair (grL, grR);
    };
    const auto linked = measure (1.0, ChannelMode::stereo);
    CHECK_NEAR (linked.first, linked.second, 0.02); // identical gain -> stable image
    CHECK_NEAR (linked.first, 10.5, 0.1);
    const auto unlinked = measure (0.0, ChannelMode::stereo);
    CHECK_NEAR (unlinked.second, 0.0, 0.01);
    CHECK_NEAR (unlinked.first, 10.5, 0.1);
    const auto half = measure (0.5, ChannelMode::stereo);
    CHECK_NEAR (half.second, 5.25, 0.1);
    const auto dual = measure (1.0, ChannelMode::dualMono); // dual mono ignores the link
    CHECK_NEAR (dual.second, 0.0, 0.01);
}

// ---------------------------------------------------------------------------------------------
// 5. mid/side
TEST (midSideProcessing)
{
    const double fs = 48000.0;
    const size_t n = static_cast<size_t> (fs * 2);
    CompressorParams p = transparentDefaults();
    p.mode = EngineMode::digital;
    p.channelMode = ChannelMode::midSide;
    p.stereoLink = 0.0;
    p.detection = DetectionMode::rms;
    p.thresholdDb = -20.0;
    p.ratio = 4.0;
    p.attackMs = 5.0;
    p.releaseMs = 1000.0;
    auto e = makeEngine (fs, p);

    // Mid: loud 1 kHz (-6 dB); Side: quiet 3 kHz (-40 dB)
    Stereo s (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double m = 0.5 * std::sin (2.0 * kPi * 1000.0 * i / fs);
        const double sd = 0.01 * std::sin (2.0 * kPi * 3000.0 * i / fs);
        s.l[i] = m + sd;
        s.r[i] = m - sd;
    }
    run (*e, s);
    std::vector<double> mid (n), side (n);
    for (size_t i = 0; i < n; ++i)
    {
        mid[i] = 0.5 * (s.l[i] + s.r[i]);
        side[i] = 0.5 * (s.l[i] - s.r[i]);
    }
    const size_t len = 16384, from = n - len;
    const double midDb = toneLevelDb (mid, from, len, fs, 1000.0);
    const double sideDb = toneLevelDb (side, from, len, fs, 3000.0);
    CHECK_NEAR (midDb, GainComputer::staticOutput (-6.0206, -20.0, 4.0, 0.0), 0.15);
    CHECK_NEAR (sideDb, -40.0, 0.05);           // side untouched
    CHECK (toneLevelDb (side, from, len, fs, 1000.0) < -120.0, "mid leaked into side"); // no crosstalk

    // M/S at unity gain reconstructs L/R to double precision
    CompressorParams q = transparentDefaults();
    q.channelMode = ChannelMode::midSide;
    q.thresholdDb = 0.0;
    auto e2 = makeEngine (fs, q);
    Stereo a = noise (48000, 0.1, 7), b = a;
    run (*e2, b);
    double err = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        err = std::max ({ err, std::abs (a.l[i] - b.l[i]), std::abs (a.r[i] - b.r[i]) });
    CHECK (err < 1e-15, "M/S reconstruction error %.3g", err);
}

// ---------------------------------------------------------------------------------------------
// 6. harmonic distortion and aliasing
TEST (transparentModeDistortion)
{
    // With gain reduction active (slow release), THD of a 1 kHz tone must be very low.
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.thresholdDb = -20.0;
    p.ratio = 3.0;
    p.attackMs = 20.0;
    p.releaseMs = 800.0;
    p.detection = DetectionMode::rms;
    p.kneeDb = 6.0;
    auto e = makeEngine (fs, p);
    Stereo s = sine (fs, 1000.0, dbToGain (-6.0), static_cast<size_t> (fs * 3));
    run (*e, s);
    const size_t len = 32768, from = s.size() - len;
    const double f0 = toneLevelDb (s.l, from, len, fs, 1000.0);
    const double h2 = toneLevelDb (s.l, from, len, fs, 2000.0) - f0;
    const double h3 = toneLevelDb (s.l, from, len, fs, 3000.0) - f0;
    CHECK (h2 < -80.0 && h3 < -70.0, "H2 %.1f dB, H3 %.1f dB", h2, h3);
}

TEST (analogColourHarmonicsAndCharacter)
{
    const double fs = 48000.0;
    auto harmonics = [&] (double character) {
        CompressorParams p = transparentDefaults();
        p.mode = EngineMode::analog;
        p.thresholdDb = 0.0; // isolate the colour stage
        p.transformer = false;
        p.amplifier = true;
        p.driveDb = 12.0;
        p.character = character;
        p.oversamplingLog2 = 2;
        auto e = makeEngine (fs, p);
        Stereo s = sine (fs, 1000.0, dbToGain (-12.0), static_cast<size_t> (fs * 1.5));
        run (*e, s);
        const size_t len = 32768, from = s.size() - len;
        const double f0 = toneLevelDb (s.l, from, len, fs, 1000.0);
        return std::make_pair (toneLevelDb (s.l, from, len, fs, 2000.0) - f0,
                               toneLevelDb (s.l, from, len, fs, 3000.0) - f0);
    };
    const auto lowChar = harmonics (0.0);
    const auto highChar = harmonics (1.0);
    CHECK (highChar.first > lowChar.first + 6.0, "2nd harmonic should grow with character (%.1f vs %.1f)",
           highChar.first, lowChar.first);
    CHECK (lowChar.second > -70.0 && lowChar.second < -20.0, "3rd harmonic subtle but present: %.1f dB", lowChar.second);
    CHECK (highChar.first < -20.0, "colour must stay subtle: H2 %.1f dB", highChar.first);
}

TEST (oversamplingReducesAliasing)
{
    // 15 kHz tone hard into the colour stage at 44.1 kHz: the 3rd harmonic (45 kHz) folds to
    // 0.9 kHz. Aliased energy must fall sharply as the oversampling factor rises.
    const double fs = 44100.0;
    auto aliasDb = [&] (int osLog2) {
        CompressorParams p = transparentDefaults();
        p.mode = EngineMode::analog;
        p.thresholdDb = 0.0;
        p.transformer = false;
        p.amplifier = true;
        p.driveDb = 18.0;
        p.character = 0.0;
        p.oversamplingLog2 = osLog2;
        auto e = makeEngine (fs, p);
        Stereo s = sine (fs, 15000.0, dbToGain (-6.0), static_cast<size_t> (fs * 1.0));
        run (*e, s);
        const size_t len = 32768, from = s.size() - len;
        return toneLevelDb (s.l, from, len, fs, 45000.0 - fs) - toneLevelDb (s.l, from, len, fs, 15000.0);
    };
    const double off = aliasDb (0), x4 = aliasDb (2), x16 = aliasDb (4);
    CHECK (x4 < off - 30.0, "4x alias %.1f dB vs off %.1f dB", x4, off);
    CHECK (x4 < -100.0, "4x alias %.1f dB", x4);
    CHECK (x16 < -100.0, "16x alias %.1f dB", x16);
}

TEST (oversamplerPassbandAndLatency)
{
    for (int q = 0; q < 2; ++q)
        for (int f = 1; f <= 4; ++f)
        {
            Oversampler<1> os;
            os.prepare (512);
            os.configure (f, static_cast<OversamplingQuality> (q));
            const double fs = 48000.0;
            const int lat = os.getLatencySamples();
            std::vector<double> x (16384), y;
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = 0.5 * std::sin (2.0 * kPi * 1000.0 * i / fs);
            y = x;
            for (size_t pos = 0; pos < y.size(); pos += 512)
            {
                double* ch[1] = { y.data() + pos };
                os.process (ch, 1, 512, [] (int, double v) { return v; });
            }
            // linear-phase & integer latency: y[n] == x[n - lat] (within passband ripple)
            double err = 0.0;
            for (size_t i = 4096; i < y.size(); ++i)
                err = std::max (err, std::abs (y[i] - x[i - static_cast<size_t> (lat)]));
            CHECK (err < (q == 0 ? 1e-5 : 1e-3), "q%d %dx: alignment error %.3g (latency %d)", q, 1 << f, err, lat);
        }
}

// ---------------------------------------------------------------------------------------------
// 7. noise floor and numerical stability
TEST (numericalStabilityAndDenormals)
{
    // Tiny signals, silence, huge signals and very low SC HPF at 384 kHz: no NaN/Inf, no blow-up.
    for (double fs : { 44100.0, 384000.0 })
    {
        CompressorParams p;
        p.mode = EngineMode::hybrid;
        p.scHpfOn = true;
        p.scHpfHz = 20.0;
        p.scLpfOn = true;
        p.topology = Topology::feedBack;
        p.ratio = 20.0;
        p.attackMs = 0.01;
        p.oversamplingLog2 = 4;
        auto e = makeEngine (fs, p);
        const size_t n = static_cast<size_t> (fs * 0.5);
        Stereo s (n);
        for (size_t i = 0; i < n; ++i)
        {
            const double t = static_cast<double> (i) / fs;
            s.l[i] = (i < n / 4) ? 1e-300 : (i < n / 2 ? 0.0 : 8.0 * std::sin (2 * kPi * 100 * t));
            s.r[i] = (i < n / 2) ? 1e-200 * std::sin (2 * kPi * 50 * t) : -s.l[i];
        }
        run (*e, s);
        bool finite = true;
        for (size_t i = 0; i < n; ++i)
            finite = finite && std::isfinite (s.l[i]) && std::isfinite (s.r[i]);
        CHECK (finite, "non-finite output at fs=%.0f", fs);
        CHECK (peakAbs (s.l, n / 2, n) < 16.0, "output exploded at fs=%.0f", fs);
    }
}

TEST (noiseFloorTransparentPath)
{
    // A -140 dBFS signal must pass through the (compressing) transparent path unchanged in level:
    // no added noise or truncation in the 64-bit path.
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.thresholdDb = -30.0;
    p.ratio = 4.0;
    auto e = makeEngine (fs, p);
    Stereo s = sine (fs, 997.0, dbToGain (-140.0), 48000);
    const Stereo ref = s;
    run (*e, s);
    double err = 0.0;
    for (size_t i = 0; i < s.size(); ++i)
        err = std::max (err, std::abs (s.l[i] - ref.l[i]));
    CHECK (err == 0.0, "low-level signal altered by %.3g", err);
}

TEST (feedbackTopologyStable)
{
    const double fs = 48000.0;
    for (double ratio : { 2.0, 8.0, 20.0 })
    {
        CompressorParams p = transparentDefaults();
        p.mode = EngineMode::analog;
        p.topology = Topology::feedBack;
        p.transformer = p.amplifier = false;
        p.ratio = ratio;
        p.thresholdDb = -20.0;
        p.attackMs = 0.01;
        p.releaseMs = 10.0;
        auto e = makeEngine (fs, p);
        Stereo s = sine (fs, 200.0, 0.9, static_cast<size_t> (fs * 2));
        run (*e, s);
        // steady state: successive cycle peaks must agree (no limit cycle / oscillation)
        const size_t cyc = 240;
        double pmin = 1e9, pmax = 0.0;
        for (size_t c = 0; c < 20; ++c)
        {
            const size_t a = s.size() - (c + 1) * cyc;
            const double pk = peakAbs (s.l, a, a + cyc);
            pmin = std::min (pmin, pk);
            pmax = std::max (pmax, pk);
        }
        CHECK (20 * std::log10 (pmax / pmin) < 0.5, "ratio %.0f: cycle-to-cycle variation %.2f dB", ratio,
               20 * std::log10 (pmax / pmin));
        CHECK (std::isfinite (pmax) && pmax < 1.0, "ratio %.0f unstable", ratio);
    }
}

// ---------------------------------------------------------------------------------------------
// 8. automation discontinuities
TEST (automationIsClickFree)
{
    // Jump threshold, makeup and mix every block on a steady sine; the second difference of the
    // output must stay close to that of the input (no steps).
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.mode = EngineMode::digital;
    p.thresholdDb = -10.0;
    p.ratio = 4.0;
    p.attackMs = 1.0;
    p.releaseMs = 50.0;
    auto e = makeEngine (fs, p, 64);
    Stereo s = sine (fs, 100.0, 0.5, static_cast<size_t> (fs));
    const double inputCurv = 0.5 * std::pow (2 * kPi * 100.0 / fs, 2.0);
    double worst = 0.0;
    std::vector<double> prev (2, 0.0);
    for (size_t pos = 0; pos < s.size(); pos += 64)
    {
        const bool flip = (pos / 64) % 50 < 25;
        p.thresholdDb = flip ? -30.0 : -5.0;
        p.makeupDb = flip ? 6.0 : -6.0;
        p.mix = flip ? 1.0 : 0.3;
        e->setParams (p);
        double* io[2] = { s.l.data() + pos, s.r.data() + pos };
        e->process (io, 2, nullptr, 0, 64);
    }
    for (size_t i = 2; i < s.size(); ++i)
        worst = std::max (worst, std::abs (s.l[i] - 2 * s.l[i - 1] + s.l[i - 2]));
    CHECK (worst < inputCurv * 4.0, "max curvature %.3g vs sine %.3g", worst, inputCurv);
}

// ---------------------------------------------------------------------------------------------
// 9. latency and bypass alignment
TEST (latencyReportingAndBypassAlignment)
{
    const double fs = 48000.0;
    for (auto mode : { EngineMode::ultraTransparent, EngineMode::analog, EngineMode::hybrid })
        for (double la : { 0.0, 1.5, 20.0 })
            for (int os : { 0, 1, 4 })
            {
                CompressorParams p = transparentDefaults();
                p.mode = mode;
                p.lookaheadMs = la;
                p.oversamplingLog2 = os;
                p.thresholdDb = 0.0;
                p.transformer = p.amplifier = true;
                p.driveDb = 0.0;
                p.colorMix = 0.0; // colour path present (latency) but contributes nothing
                auto e = makeEngine (fs, p);
                const int lat = e->getLatencySamples();
                const int expectedLa = static_cast<int> (std::lround (la * 0.001 * fs));
                const bool colour = mode != EngineMode::ultraTransparent;
                if (! colour)
                    CHECK (lat == expectedLa, "latency %d expected %d", lat, expectedLa);

                Stereo s (8192);
                s.l[1000] = s.r[1000] = 1.0; // impulse
                Stereo bypassed = s;
                run (*e, s);
                // processed impulse peak lands exactly at 1000 + latency
                size_t peakIdx = 0;
                for (size_t i = 0; i < s.size(); ++i)
                    if (std::abs (s.l[i]) > std::abs (s.l[peakIdx]))
                        peakIdx = i;
                CHECK (peakIdx == 1000u + static_cast<size_t> (lat), "mode %d la %.1f os %d: peak at %zu, latency %d",
                       (int) mode, la, os, peakIdx, lat);

                // bypass path is delayed by the same latency
                p.bypass = true;
                p.loudnessMatchedBypass = false;
                auto eb = makeEngine (fs, p);
                run (*eb, bypassed);
                CHECK (bypassed.l[1000u + static_cast<size_t> (lat)] == 1.0, "bypass misaligned");
            }
}

TEST (loudnessMatchedBypass)
{
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.mode = EngineMode::digital;
    p.thresholdDb = -30.0;
    p.ratio = 4.0;
    p.makeupDb = 6.0;
    p.attackMs = 5.0;
    p.releaseMs = 200.0;
    p.detection = DetectionMode::rms;
    auto e = makeEngine (fs, p);
    Stereo warm = noise (static_cast<size_t> (fs * 8), 0.1, 3);
    run (*e, warm);
    p.bypass = true;
    e->setParams (p);
    Stereo active = noise (static_cast<size_t> (fs * 2), 0.1, 4);
    Stereo src = active;
    run (*e, active);
    // bypassed output level == processed level (within 0.5 dB), not the raw input level
    const double matched = rmsDb (active.l, 9600, active.size());
    const double raw = rmsDb (src.l, 9600, src.size());
    const double processedDb = raw + e->getMeters().loudnessMatchDb;
    CHECK_NEAR (matched, processedDb, 0.3);
    CHECK (std::abs (matched - raw) > 1.0, "match gain not applied (%.2f vs %.2f)", matched, raw);
}

// ---------------------------------------------------------------------------------------------
// 10. null tests in transparent mode
TEST (nullTestTransparentMode)
{
    // Below threshold (or ratio 1:1), Ultra Transparent output must be bit-identical to the input
    // delayed by the reported latency — in stereo and dual-mono, any lookahead.
    const double fs = 96000.0;
    for (auto cm : { ChannelMode::stereo, ChannelMode::dualMono })
        for (double la : { 0.0, 5.0 })
            for (double ratio : { 1.0, 4.0 })
            {
                CompressorParams p = transparentDefaults();
                p.channelMode = cm;
                p.lookaheadMs = la;
                p.ratio = ratio;
                p.thresholdDb = ratio == 1.0 ? -60.0 : 0.0;
                p.scHpfOn = true;
                auto e = makeEngine (fs, p);
                Stereo s = noise (96000, 0.05, 11);
                const Stereo ref = s;
                run (*e, s, 333); // odd block size on purpose
                const size_t lat = static_cast<size_t> (e->getLatencySamples());
                double maxErr = 0.0;
                for (size_t i = lat; i < s.size(); ++i)
                    maxErr = std::max ({ maxErr, std::abs (s.l[i] - ref.l[i - lat]), std::abs (s.r[i] - ref.r[i - lat]) });
                CHECK (maxErr == 0.0, "cm %d la %.0f ratio %.0f: null residual %.3g", (int) cm, la, ratio, maxErr);
            }
}

TEST (externalSidechain)
{
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.mode = EngineMode::digital;
    p.externalSidechain = true;
    p.detection = DetectionMode::rms;
    p.thresholdDb = -20.0;
    p.ratio = 4.0;
    p.releaseMs = 1000.0;
    auto e = makeEngine (fs, p);
    Stereo s = sine (fs, 1000.0, 0.05, static_cast<size_t> (fs)); // -26 dB: would not compress
    Stereo sc = sine (fs, 500.0, 0.5, s.size());                 // -6 dB key
    run (*e, s, 512, &sc);
    const double out = 20 * std::log10 (peakAbs (s.l, s.size() - 4800, s.size()));
    CHECK_NEAR (out, -26.02 - 10.5, 0.2);
}

// ---------------------------------------------------------------------------------------------
// 11. sample rates
TEST (stabilityAcrossSampleRates)
{
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0 })
        for (auto mode : { EngineMode::ultraTransparent, EngineMode::digital, EngineMode::analog, EngineMode::hybrid })
        {
            CompressorParams p;
            p.mode = mode;
            p.thresholdDb = -20.0;
            p.ratio = 4.0;
            p.attackMs = 5.0;
            p.releaseMs = 1500.0;
            p.kneeDb = 0.0;
            p.scHpfOn = false;
            p.transformer = p.amplifier = false; // compare compression behaviour only
            p.lookaheadMs = 2.0;
            auto e = makeEngine (fs, p, 1024);
            Stereo s = sine (fs, 1000.0, dbToGain (-6.0), static_cast<size_t> (fs * 2.5));
            run (*e, s, 1024);
            const size_t tail = static_cast<size_t> (fs * 0.1);
            const double outDb = 20 * std::log10 (peakAbs (s.l, s.size() - tail, s.size()));
            // analog path: tilt filter (+/-) and soft knee shift the curve slightly
            const double tol = (mode == EngineMode::analog || mode == EngineMode::hybrid) ? 1.0 : 0.15;
            CHECK_NEAR (outDb, -6.0206 - 10.5 + 0.0, tol);
        }
}

// ---------------------------------------------------------------------------------------------
int main()
{
    std::printf ("Lumen MC-64 DSP tests\n");
    for (auto& t : registry())
    {
        currentTest = t.name;
        const int before = failures;
        t.fn();
        std::printf ("%s %s\n", failures == before ? "[ OK ]" : "[FAIL]", t.name);
    }
    std::printf ("%s: %d failure(s)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
