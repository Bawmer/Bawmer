// Lumen MC-64 — DSP verification suite (no JUCE required).
// Build: cmake -B build -DLUMEN_BUILD_PLUGIN=OFF && cmake --build build && ctest --test-dir build

#include <lumen/CompressorEngine.h>

#include <complex>
#include <cstdio>
#include <functional>
#include <limits>
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
    // Closed-loop fixed point of the feedback law must equal the feed-forward curve at *every*
    // level, including inside the soft knee (where a naive -(R-1)·overshoot law is wrong).
    for (double ratio : { 1.0, 1.5, 2.0, 4.0, 8.0, 20.0 })
        for (double kneeDb : { 0.0, 1.0, 6.0, 24.0 })
            for (double in = -60.0; in <= 0.0; in += 0.25)
            {
                // solve y = in + G_fb(y) by bisection (y - in - G_fb(y) is increasing in y)
                double lo = -300.0, hi = in;
                for (int it = 0; it < 200; ++it)
                {
                    const double mid = 0.5 * (lo + hi);
                    (mid - in - GainComputer::feedBack (mid, -20.0, ratio, kneeDb) > 0.0 ? hi : lo) = mid;
                }
                CHECK_NEAR (0.5 * (lo + hi), GainComputer::staticOutput (in, -20.0, ratio, kneeDb), 1e-9);
            }
}

TEST (inverseStaticCurveRoundTrip)
{
    for (double ratio : { 1.0, 1.01, 3.0, 20.0 })
        for (double kneeDb : { 0.0, 0.5, 12.0, 24.0 })
            for (double in = -60.0; in <= 0.0; in += 0.1)
            {
                const double out = GainComputer::staticOutput (in, -24.0, ratio, kneeDb);
                CHECK_NEAR (GainComputer::inverseStaticOutput (out, -24.0, ratio, kneeDb), in, 1e-9);
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
// 12. exact-onset lookahead
TEST (lookaheadShaperGuarantee)
{
    // Property test of the shaper alone: s[n] >= t[n - L] for random targets, lookaheads and
    // kernel lengths (the proof in Lookahead.h).
    std::mt19937 rng (5);
    std::uniform_real_distribution<double> u (0.0, 1.0);
    for (int trial = 0; trial < 40; ++trial)
    {
        const int L = 1 + static_cast<int> (u (rng) * 900);
        const int W = 1 + static_cast<int> (u (rng) * (L + 1));
        auto shaper = std::make_unique<LookaheadShaper<1024>>();
        shaper->configure (L, W, true);
        std::vector<double> t (20000);
        for (auto& v : t)
            v = u (rng) < 0.01 ? 40.0 * u (rng) : (u (rng) < 0.5 ? 0.0 : 3.0 * u (rng));
        double worst = 0.0;
        for (size_t n = 0; n < t.size(); ++n)
        {
            const double sOut = shaper->process (t[n]);
            if (n >= static_cast<size_t> (L))
                worst = std::max (worst, t[n - static_cast<size_t> (L)] - sOut);
        }
        CHECK (worst <= 1e-12, "L %d W %d: under-shoot %.3g dB", L, W, worst);
    }
}

TEST (exactLookaheadZeroOvershoot)
{
    // 0.5 ms bursts at 0 dBFS over a quiet bed, 20:1, peak detection: in Exact mode no output
    // sample may exceed the static curve of the input sample it came from — in every engine mode,
    // linked stereo, with attack shorter or longer than the lookahead.
    const double fs = 48000.0;
    for (auto mode : { EngineMode::ultraTransparent, EngineMode::digital, EngineMode::analog, EngineMode::hybrid })
        for (double att : { 1.0, 10.0 })
            for (double la : { 1.0, 5.0, 20.0 })
            {
                CompressorParams p = transparentDefaults();
                p.mode = mode;
                p.transformer = p.amplifier = false;
                p.thresholdDb = -20.0;
                p.ratio = 20.0;
                p.attackMs = att;
                p.releaseMs = 50.0;
                p.detection = DetectionMode::peak;
                p.lookaheadMs = la;
                p.lookaheadMode = LookaheadMode::exact;
                auto e = makeEngine (fs, p);
                const size_t n = static_cast<size_t> (fs);
                Stereo s (n);
                for (size_t i = 0; i < n; ++i)
                {
                    double v = 0.01 * std::sin (2.0 * kPi * 300.0 * i / fs);
                    if (i % 4800 < 24)
                        v += std::sin (2.0 * kPi * 5000.0 * i / fs);
                    s.l[i] = v;
                    s.r[i] = 0.7 * v;
                }
                const Stereo in = s;
                run (*e, s);
                const size_t L = static_cast<size_t> (e->getLatencySamples());
                double worst = -1e9;
                for (size_t i = L + 4800; i < n; ++i)
                {
                    const double x = std::abs (in.l[i - L]);
                    if (x < 1e-4)
                        continue;
                    // Reference: the static curve of the input sample (analog mode uses its
                    // minimum 6 dB knee). The analog detector tilt adds +1.5 dB of sensitivity at
                    // 5 kHz, which only increases reduction, so it cannot hide an overshoot.
                    const double kneeDb = mode == EngineMode::analog ? CompressorEngine::analogKnee (0.0) : 0.0;
                    const double xd = 20.0 * std::log10 (x);
                    const double allowed = GainComputer::staticOutput (xd, -20.0, 20.0, kneeDb);
                    worst = std::max (worst, 20.0 * std::log10 (std::abs (s.l[i]) + 1e-300) - allowed);
                }
                CHECK (worst <= 1e-6, "mode %d att %.0f la %.0f: overshoot %.3f dB", (int) mode, att, la, worst);
            }
}

TEST (lookaheadChangeIsClickFree)
{
    // Changing the lookahead (and therefore the delay) while audio runs must crossfade, not jump.
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.thresholdDb = 0.0; // no gain reduction: isolates the delay change
    auto e = makeEngine (fs, p, 256);
    Stereo s = sine (fs, 200.0, 0.5, static_cast<size_t> (fs * 2));
    const double inputCurv = 0.5 * std::pow (2.0 * kPi * 200.0 / fs, 2.0);
    const double lookaheads[] = { 0.0, 7.3, 20.0, 1.1, 13.0, 0.0, 5.0, 19.0 };
    for (size_t pos = 0, k = 0; pos < s.size(); pos += 256)
    {
        if (pos % 12000 == 0)
        {
            p.lookaheadMs = lookaheads[k++ % 8];
            e->setParams (p);
        }
        double* io[2] = { s.l.data() + pos, s.r.data() + pos };
        e->process (io, 2, nullptr, 0, 256);
    }
    double worst = 0.0;
    for (size_t i = 2; i < s.size(); ++i)
        worst = std::max (worst, std::abs (s.l[i] - 2 * s.l[i - 1] + s.l[i - 2]));
    // a raw delay jump would produce a step of up to the full amplitude (curvature ~1)
    CHECK (worst < inputCurv * 20.0, "max curvature %.3g vs sine %.3g", worst, inputCurv);
}

TEST (movingAverageNoDrift)
{
    // The O(1) running sum must not drift: compare against exact recomputation after 10 M samples.
    auto ma = std::make_unique<MovingAverage<512>>();
    ma->setLength (377);
    std::mt19937 rng (9);
    std::uniform_real_distribution<double> u (0.0, 60.0);
    std::vector<double> hist (377, 0.0);
    double out = 0.0;
    for (int i = 0; i < 10000000; ++i)
    {
        const double v = u (rng);
        hist[static_cast<size_t> (i % 377)] = v;
        out = ma->process (v);
    }
    double exact = 0.0;
    for (double v : hist)
        exact += v;
    CHECK_NEAR (out, exact / 377.0, 1e-9);
}

// ---------------------------------------------------------------------------------------------
// 13. zero-delay feedback topology
namespace
{
CompressorParams feedbackParams (Topology t, double ratio)
{
    CompressorParams p = transparentDefaults();
    p.mode = EngineMode::analog;
    p.transformer = p.amplifier = false;
    p.topology = t;
    p.ratio = ratio;
    return p;
}
} // namespace

TEST (zdfFeedbackStaticMatchesFeedForward)
{
    // Settled output level of FB and FF must agree (exact FB law incl. knee + zero-delay loop).
    const double fs = 48000.0;
    for (double ratio : { 2.0, 4.0, 10.0, 20.0 })
    {
        double lvl[2];
        for (int t = 0; t < 2; ++t)
        {
            CompressorParams p = feedbackParams (t == 0 ? Topology::feedForward : Topology::feedBack, ratio);
            p.thresholdDb = -20.0;
            p.attackMs = 10.0;
            p.releaseMs = 300.0;
            p.detection = DetectionMode::rms;
            auto e = makeEngine (fs, p);
            Stereo s = sine (fs, 1000.0, 0.5, 512 * 560);
            run (*e, s);
            lvl[t] = rmsDb (s.l, s.size() - 48000, s.size());
        }
        CHECK_NEAR (lvl[1], lvl[0], 0.01);
    }
}

TEST (zdfLoopCompensationExact)
{
    // The bare loop (flat detector, analog envelope, exact FB law, ZDF solver) must reproduce the
    // feed-forward step response to within one sample: loop-gain pre-warping is exact.
    const double fs = 48000.0;
    for (double ratio : { 2.0, 4.0, 20.0 })
        for (double att : { 0.05, 1.0, 10.0, 100.0 })
        {
            BallisticsSettings b;
            b.attackMs = att;
            b.releaseMs = 500.0;
            AnalogEnvelope ff, fb;
            ff.prepare (fs);
            fb.prepare (fs);
            ff.configure (b, 0.0);
            fb.configure (b, ratio - 1.0);
            const double levelIn = 0.0, thr = -30.0, kneeDb = 0.0;
            const double target = -GainComputer::feedForward (levelIn, thr, ratio, kneeDb);
            int tFF = -1, tFB = -1;
            for (int n = 0; n < static_cast<int> (fs) && (tFF < 0 || tFB < 0); ++n)
            {
                const double rff = ff.process (target);
                auto H = [&] (double r) { return fb.peekBeforeRc (-GainComputer::feedBack (levelIn - r, thr, ratio, kneeDb)); };
                const double r = FeedbackSolver::solve (H, fb.getOutputBeforeRc()).r;
                const double rfb = fb.process (-GainComputer::feedBack (levelIn - r, thr, ratio, kneeDb));
                const double goal = target * (1.0 - std::exp (-1.0));
                if (tFF < 0 && rff >= goal) tFF = n;
                if (tFB < 0 && rfb >= goal) tFB = n;
            }
            CHECK (std::abs (tFF - tFB) <= 1, "R %.0f att %.2f: FF %d vs FB %d samples", ratio, att, tFF, tFB);
        }
}

TEST (zdfFeedbackTimingMatchesFeedForward)
{
    // Full engine. Attack time constants (63 %) in FB must track FF, and the FB step response must
    // not overshoot more than FF. Tolerance: the analog detector's tilt filter (-3 dB shelf below
    // 90 Hz, ~2 ms settling after a step) sits *inside* the loop in FB but outside it in FF, which
    // measurably lengthens slow FB attacks by up to ~25 % on a DC step (with the tilt flattened the
    // two are identical to the sample — see zdfLoopCompensationExact). That difference is the
    // physics of frequency-dependent feedback detection, not a timing error.
    const double fs = 48000.0;
    for (double ratio : { 4.0, 20.0 })
        for (double att : { 0.05, 1.0, 10.0 })
        {
            double t63[2], peak[2];
            for (int t = 0; t < 2; ++t)
            {
                CompressorParams p = feedbackParams (t == 0 ? Topology::feedForward : Topology::feedBack, ratio);
                p.thresholdDb = -30.0;
                p.attackMs = att;
                p.releaseMs = 500.0;
                p.detection = DetectionMode::peak;
                auto e = makeEngine (fs, p);
                Stereo s (static_cast<size_t> (fs));
                std::fill (s.l.begin(), s.l.end(), 1.0);
                std::fill (s.r.begin(), s.r.end(), 1.0);
                run (*e, s);
                const double fin = -20.0 * std::log10 (s.l.back());
                t63[t] = -1.0;
                peak[t] = 0.0;
                for (size_t i = 0; i < s.size(); ++i)
                {
                    const double gr = -20.0 * std::log10 (s.l[i]);
                    peak[t] = std::max (peak[t], gr);
                    if (t63[t] < 0.0 && gr >= fin * (1.0 - std::exp (-1.0)))
                        t63[t] = 1000.0 * static_cast<double> (i) / fs;
                }
            }
            const double tol = std::max (0.25 * t63[0], 0.05); // 50 µs floor (2.4 samples)
            CHECK (std::abs (t63[1] - t63[0]) <= tol, "R %.0f att %.2f: FB %.3f ms vs FF %.3f ms", ratio, att, t63[1], t63[0]);
            CHECK (peak[1] <= peak[0] + 0.1, "R %.0f att %.2f: FB overshoot %.2f dB vs FF %.2f dB", ratio, att, peak[1], peak[0]);
        }
}

TEST (feedbackSolverStress)
{
    // Hybrid + feedback with extreme settings and wild input: finite output, bounded solver work.
    const double fs = 96000.0;
    CompressorParams p;
    p.mode = EngineMode::hybrid;
    p.topology = Topology::feedBack;
    p.ratio = 20.0;
    p.kneeDb = 24.0;
    p.attackMs = 0.01;
    p.releaseMs = 10.0;
    p.thresholdDb = -60.0;
    p.detection = DetectionMode::hybrid;
    p.scLpfOn = true;
    p.transformer = p.amplifier = false;
    auto e = makeEngine (fs, p);
    std::mt19937 rng (3);
    std::uniform_real_distribution<double> u (-1.0, 1.0);
    Stereo s (static_cast<size_t> (fs * 2));
    for (size_t i = 0; i < s.size(); ++i)
    {
        const double burst = (i / 4000) % 3 == 0 ? 16.0 : ((i / 4000) % 3 == 1 ? 0.0 : 1e-8);
        s.l[i] = burst * u (rng);
        s.r[i] = (i % 1000 < 500) ? burst : -burst; // square wave
    }
    run (*e, s);
    bool finite = true;
    for (size_t i = 0; i < s.size(); ++i)
        finite = finite && std::isfinite (s.l[i]) && std::isfinite (s.r[i]);
    CHECK (finite, "non-finite output");
    CHECK (e->getAverageSolverEvaluations() < 8.0, "solver average %.2f evaluations", e->getAverageSolverEvaluations());
}


// ---------------------------------------------------------------------------------------------
// 14. true-peak detection
TEST (truePeakInterpolatorAccuracy)
{
    // Against an ideal interpolator on the same 4x grid: <= 0.02 dB under-read up to 0.45·fs,
    // never more than 0.02 dB over-read. (The grid itself may miss the crest by up to
    // TruePeakInterpolator::gridBoundDb, the documented limit of 4x true-peak meters.)
    const double fs = 48000.0;
    TruePeakInterpolator tp;
    std::mt19937 rng (1);
    std::uniform_real_distribution<double> u (0.0, 2.0 * kPi);
    double worstUnder = 0.0, worstOver = -1e9;
    for (double frac = 0.002; frac <= 0.45; frac += 0.004)
        for (int t = 0; t < 12; ++t)
        {
            tp.reset();
            const double f = frac * fs, ph = u (rng);
            double pk = 0.0, ideal = 0.0;
            for (int n = 0; n < 300; ++n)
            {
                const double v = tp.process (std::sin (2.0 * kPi * f * n / fs + ph));
                if (n <= 80)
                    continue;
                pk = std::max (pk, v);
                for (int p = 0; p < 4; ++p)
                    ideal = std::max (ideal, std::abs (std::sin (2.0 * kPi * f * (n - TruePeakInterpolator::kDelay + p / 4.0) / fs + ph)));
            }
            worstUnder = std::max (worstUnder, 20.0 * std::log10 (ideal / pk));
            worstOver = std::max (worstOver, 20.0 * std::log10 (pk));
        }
    CHECK (worstUnder <= 0.02, "under-read vs ideal 4x grid %.3f dB", worstUnder);
    CHECK (worstOver <= 0.02, "over-read %.3f dB", worstOver);

    // the classic inter-sample case: fs/4 at 45 degrees — samples read -3.01 dB, true peak 0 dB
    tp.reset();
    double pk = 0.0;
    for (int n = 0; n < 200; ++n)
    {
        const double v = tp.process (std::sin (0.5 * kPi * n + 0.25 * kPi));
        if (n > 80)
            pk = std::max (pk, v);
    }
    CHECK_NEAR (20.0 * std::log10 (pk), 0.0, 0.05);
}

TEST (truePeakEngineIntegration)
{
    const double fs = 48000.0;
    // 1. fs/4 at 45 degrees, 0 dBTP but -3.01 dB sample peak; threshold -1.5 dB, 20:1.
    //    Sample-peak detection misses it entirely; true peak compresses by ~1.4 dB.
    auto grFor = [&] (DetectionMode det) {
        CompressorParams p = transparentDefaults();
        p.mode = EngineMode::digital;
        p.thresholdDb = -1.5;
        p.ratio = 20.0;
        p.attackMs = 0.5;
        p.releaseMs = 500.0;
        p.detection = det;
        auto e = makeEngine (fs, p);
        Stereo s (48000);
        for (size_t i = 0; i < s.size(); ++i)
            s.l[i] = s.r[i] = std::sin (0.5 * kPi * static_cast<double> (i) + 0.25 * kPi);
        run (*e, s);
        return -3.0103 - 20.0 * std::log10 (peakAbs (s.l, s.size() - 4800, s.size()));
    };
    CHECK (std::abs (grFor (DetectionMode::peak)) < 0.01, "sample peak should not compress");
    CHECK_NEAR (grFor (DetectionMode::truePeak), 1.5 * (1.0 - 1.0 / 20.0), 0.1);

    // 2. latency grows by the detector delay; Ultra-mode null stays bit-exact
    CompressorParams p = transparentDefaults();
    p.detection = DetectionMode::truePeak;
    p.thresholdDb = 0.0;
    p.lookaheadMs = 1.0;
    auto e = makeEngine (fs, p);
    CHECK (e->getLatencySamples() == 48 + LevelDetector::kTruePeakDelay, "latency %d", e->getLatencySamples());
    Stereo s = noise (48000, 0.05, 12);
    const Stereo ref = s;
    run (*e, s, 333);
    const size_t lat = static_cast<size_t> (e->getLatencySamples());
    double err = 0.0;
    for (size_t i = lat; i < s.size(); ++i)
        err = std::max (err, std::abs (s.l[i] - ref.l[i - lat]));
    CHECK (err == 0.0, "true-peak null residual %.3g", err);

    // 3. exact lookahead keeps its zero-overshoot guarantee with true-peak detection
    CompressorParams q = transparentDefaults();
    q.mode = EngineMode::digital;
    q.detection = DetectionMode::truePeak;
    q.thresholdDb = -20.0;
    q.ratio = 20.0;
    q.attackMs = 2.0;
    q.releaseMs = 50.0;
    q.lookaheadMs = 3.0;
    auto e2 = makeEngine (fs, q);
    Stereo b (48000);
    for (size_t i = 0; i < b.size(); ++i)
        b.l[i] = b.r[i] = 0.01 * std::sin (2.0 * kPi * 300.0 * i / fs) + ((i % 4800 < 24) ? std::sin (2.0 * kPi * 5000.0 * i / fs) : 0.0);
    const Stereo bin = b;
    run (*e2, b);
    const size_t L = static_cast<size_t> (e2->getLatencySamples());
    double worst = -1e9;
    for (size_t i = L + 4800; i < b.size(); ++i)
    {
        const double x = std::abs (bin.l[i - L]);
        if (x < 1e-4)
            continue;
        const double xd = 20.0 * std::log10 (x);
        worst = std::max (worst, 20.0 * std::log10 (std::abs (b.l[i]) + 1e-300) - GainComputer::staticOutput (xd, -20.0, 20.0, 0.0));
    }
    CHECK (worst <= 1e-6, "true-peak + exact lookahead overshoot %.3f dB", worst);
}

// ---------------------------------------------------------------------------------------------
// 15. hold, adaptive release, smooth hybrid detector
namespace
{
double thdPlusNoise30Hz (EngineMode mode, double holdMs)
{
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.mode = mode;
    p.transformer = p.amplifier = false;
    p.thresholdDb = -20.0;
    p.ratio = 4.0;
    p.attackMs = 0.1;
    p.releaseMs = 30.0;
    p.detection = DetectionMode::peak;
    p.holdMs = holdMs;
    auto e = makeEngine (fs, p, 480);
    Stereo s = sine (fs, 30.0, 0.5, 48000 * 3);
    run (*e, s, 480);
    // last second = 30 whole periods: least-squares fundamental, then residual energy
    const size_t a = s.size() - 48000;
    double cc = 0.0, ss = 0.0;
    for (size_t i = a; i < s.size(); ++i)
    {
        const double ph = 2.0 * kPi * 30.0 * static_cast<double> (i - a) / fs;
        cc += s.l[i] * std::cos (ph);
        ss += s.l[i] * std::sin (ph);
    }
    cc *= 2.0 / 48000.0;
    ss *= 2.0 / 48000.0;
    double res = 0.0, fund = 0.0;
    for (size_t i = a; i < s.size(); ++i)
    {
        const double ph = 2.0 * kPi * 30.0 * static_cast<double> (i - a) / fs;
        const double f = cc * std::cos (ph) + ss * std::sin (ph);
        res += (s.l[i] - f) * (s.l[i] - f);
        fund += f * f;
    }
    return 10.0 * std::log10 (res / fund + 1e-300);
}

/** ms from the end of a 1 kHz burst until gain reduction is below 1 dB. */
double recoveryMs (bool autoRel, AutoReleaseShape shape, double burstMs, double burstDb, bool transient)
{
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.mode = EngineMode::digital;
    p.thresholdDb = -30.0;
    p.ratio = 4.0;
    p.attackMs = 2.0;
    p.releaseMs = 400.0;
    p.detection = DetectionMode::rms;
    p.autoRelease = autoRel;
    p.autoReleaseShape = shape;
    auto e = makeEngine (fs, p, 480);
    const size_t start = 4800, burst = static_cast<size_t> (burstMs * 48.0);
    Stereo s (start + burst + 48000 * 6 + 480 - (start + burst) % 480);
    for (size_t i = 0; i < s.size(); ++i)
    {
        double a = dbToGain (-40.0);
        if (i >= start && i < start + burst)
        {
            a = dbToGain (burstDb);
            if (transient)
                a *= (i % 4800 < 240) ? 2.5 : 0.35; // 5 ms hits every 100 ms: high crest factor
        }
        s.l[i] = s.r[i] = a * std::sin (2.0 * kPi * 1000.0 * i / fs);
    }
    const Stereo in = s;
    run (*e, s, 480);
    for (size_t i = start + burst; i + 48 < s.size(); i += 48)
    {
        double ei = 0.0, eo = 0.0;
        for (size_t k = 0; k < 48; ++k)
        {
            ei += in.l[i + k] * in.l[i + k];
            eo += s.l[i + k] * s.l[i + k];
        }
        if (10.0 * std::log10 (ei / (eo + 1e-300)) < 1.0)
            return static_cast<double> (i - start - burst) / 48.0;
    }
    return 1e9;
}
} // namespace

TEST (holdRemovesLowFrequencyRipple)
{
    // Peak detection on a 30 Hz sine: the rectified target peaks every 1/(2f) = 16.7 ms. With
    // no hold, a 30 ms release lets the gain ripple between peaks (heavy LF distortion); a hold
    // of at least 1/(2f) freezes the gain between peaks and the steady-state distortion vanishes.
    for (auto mode : { EngineMode::digital, EngineMode::analog })
    {
        const double noHold = thdPlusNoise30Hz (mode, 0.0);
        const double held = thdPlusNoise30Hz (mode, 1000.0 / 60.0 + 1.0);
        CHECK (noHold > -40.0, "mode %d: expected audible ripple without hold, got %.1f dB", (int) mode, noHold);
        CHECK (held < -120.0, "mode %d: THD+N with hold %.1f dB", (int) mode, held);
    }
}

TEST (adaptiveReleaseProgramDependence)
{
    using S = AutoReleaseShape;
    // duration: longer compression -> slower recovery
    const double r10 = recoveryMs (true, S::program, 10.0, -6.0, false);
    const double r300 = recoveryMs (true, S::program, 300.0, -6.0, false);
    const double r3000 = recoveryMs (true, S::program, 3000.0, -6.0, false);
    CHECK (r10 < r300 && r300 < r3000, "duration: %.0f / %.0f / %.0f ms", r10, r300, r3000);
    // depth: deeper sustained reduction -> slower recovery
    const double shallow = recoveryMs (true, S::program, 3000.0, -18.0, false);
    CHECK (shallow < r3000, "depth: shallow %.0f ms vs deep %.0f ms", shallow, r3000);
    // crest: transient material recovers faster than dense material with the same peaks
    const double transient = recoveryMs (true, S::program, 3000.0, -6.0, true);
    CHECK (transient < 0.6 * r3000, "crest: transient %.0f ms vs dense %.0f ms", transient, r3000);
    // shapes after a short burst: Fast < Program < Smooth
    const double f10 = recoveryMs (true, S::fast, 10.0, -6.0, false);
    const double s10 = recoveryMs (true, S::smooth, 10.0, -6.0, false);
    CHECK (f10 < r10 && r10 < s10, "short burst: fast %.0f, program %.0f, smooth %.0f ms", f10, r10, s10);
}

TEST (adaptiveReleaseMultiplierIsSlow)
{
    // The release multiplier is derived from smoothed quantities only: on a strongly dynamic
    // signal it must stay within its bounds and never jump between updates (32 samples).
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.mode = EngineMode::digital;
    p.thresholdDb = -30.0;
    p.ratio = 4.0;
    p.attackMs = 1.0;
    p.releaseMs = 300.0;
    p.autoRelease = true;
    p.autoReleaseShape = AutoReleaseShape::program;
    auto e = makeEngine (fs, p, 32);
    Stereo s = noise (static_cast<size_t> (fs * 4), 0.3, 21);
    for (size_t i = 0; i < s.size(); ++i)
    {
        const double env = (i / 2400) % 2 ? 1.0 : 0.02; // 50 ms on/off gating
        s.l[i] *= env;
        s.r[i] *= env;
    }
    double prev = e->getReleaseMultiplier (0), worstStep = 0.0, lo = 1e9, hi = 0.0;
    for (size_t pos = 0; pos < s.size(); pos += 32)
    {
        double* io[2] = { s.l.data() + pos, s.r.data() + pos };
        e->process (io, 2, nullptr, 0, 32);
        const double m = e->getReleaseMultiplier (0);
        worstStep = std::max (worstStep, std::abs (m - prev) / prev);
        lo = std::min (lo, m);
        hi = std::max (hi, m);
        prev = m;
    }
    CHECK (lo >= 1.0 / 3.0 - 1e-12 && hi <= 3.0 + 1e-12, "multiplier range %.3f..%.3f", lo, hi);
    CHECK (worstStep < 0.02, "multiplier jumped by %.2f %% in one update", 100.0 * worstStep);
}

TEST (hybridDetectorIsSmooth)
{
    // Soft maximum a + softplus(b - a): C-infinity, equals RMS when peaks are well inside the
    // crest allowance, and stays within ln2/k = 0.35 dB of a hard max.
    double prevSlope = 0.0, worstJump = 0.0, worstGap = 0.0;
    for (double z = -20.0; z <= 20.0; z += 0.01)
    {
        const double v = LevelDetector::softplus (z);
        const double slope = (LevelDetector::softplus (z + 1e-5) - v) / 1e-5;
        if (z > -20.0)
            worstJump = std::max (worstJump, std::abs (slope - prevSlope));
        prevSlope = slope;
        worstGap = std::max (worstGap, v - std::max (0.0, z));
        CHECK (slope >= -1e-9 && slope <= 1.0 + 1e-9, "slope %.6f at %.2f", slope, z);
    }
    CHECK (worstJump < 0.011, "slope discontinuity %.4f", worstJump);
    CHECK (worstGap <= std::log (2.0) / 2.0 + 1e-9, "gap to hard max %.4f dB", worstGap);
    CHECK (LevelDetector::softplus (-6.0) < 1e-5, "RMS calibration disturbed: %.3g dB", LevelDetector::softplus (-6.0));
}


// ---------------------------------------------------------------------------------------------
// 16. analog component models
namespace
{
CompressorParams modelParams (AnalogModel m)
{
    CompressorParams p = transparentDefaults();
    p.mode = EngineMode::analog;
    p.analogModel = m;
    p.transformer = p.amplifier = false;
    p.thresholdDb = -30.0;
    p.ratio = 4.0;
    p.attackMs = 10.0;
    p.releaseMs = 300.0;
    p.detection = DetectionMode::rms;
    return p;
}
const AnalogModel kAllModels[] = { AnalogModel::vca, AnalogModel::opto, AnalogModel::variMu, AnalogModel::fet };
} // namespace

TEST (analogModelsStaticCurve)
{
    // Every model's dynamics are its own; once settled, each lands on the static curve with its
    // characteristic knee (1 kHz, where the detector tilt is ~flat), in FF and in FB.
    const double fs = 48000.0;
    for (auto m : kAllModels)
        for (auto topo : { Topology::feedForward, Topology::feedBack })
            for (double in : { -36.0, -30.0, -24.0, -12.0 })
            {
                CompressorParams p = modelParams (m);
                p.topology = topo;
                p.releaseMs = 200.0;
                auto e = makeEngine (fs, p, 480);
                Stereo s = sine (fs, 1000.0, dbToGain (in), 480 * 400);
                run (*e, s, 480);
                const double out = toneLevelDb (s.l, s.size() - 32768, 32768, fs, 1000.0);
                const double expected = GainComputer::staticOutput (in, -30.0, 4.0, analogModelKnee (m, 0.0));
                CHECK (std::abs (out - expected) < 0.06, "model %d topo %d in %.0f: %.3f dB off the curve", (int) m,
                       (int) topo, in, out - expected);
            }
}

TEST (variMuRatioRisesWithLevel)
{
    // Vari-mu character: the local ratio climbs gradually over ~18 dB above threshold.
    const double k = analogModelKnee (AnalogModel::variMu, 0.0);
    auto localRatio = [&] (double x) {
        const double slope = (GainComputer::staticOutput (x + 0.01, -30.0, 4.0, k) - GainComputer::staticOutput (x - 0.01, -30.0, 4.0, k)) / 0.02;
        return 1.0 / slope;
    };
    const double r2 = localRatio (-28.0), r8 = localRatio (-22.0), r16 = localRatio (-14.0);
    CHECK (r2 < 2.0 && r2 < r8 && r8 < r16, "local ratios %.2f / %.2f / %.2f", r2, r8, r16);
    CHECK (k >= 18.0, "vari-mu knee %.1f dB", k);
}

TEST (optoTwoStageReleaseWithMemory)
{
    // Opto character: after a short burst most of the reduction recovers within 100 ms (fast
    // stage); after a long burst the stored "light" holds a slow tail.
    const double fs = 48000.0;
    auto measure = [&] (double burstSec, double& fractionAt100ms, double& timeTo1dB) {
        CompressorParams p = modelParams (AnalogModel::opto);
        auto e = makeEngine (fs, p, 480);
        const size_t nb = static_cast<size_t> (burstSec * fs);
        size_t n = nb + 480 * 1500;
        n -= n % 480;
        Stereo s (n);
        for (size_t i = 0; i < n; ++i)
            s.l[i] = s.r[i] = (i < nb ? 0.5 : 0.005) * std::sin (2.0 * kPi * 1000.0 * i / fs);
        const Stereo in = s;
        run (*e, s, 480);
        auto gr = [&] (size_t i) {
            double a = 0.0, b = 0.0;
            for (size_t k = 0; k < 48; ++k) { a += in.l[i + k] * in.l[i + k]; b += s.l[i + k] * s.l[i + k]; }
            return 10.0 * std::log10 (a / (b + 1e-300));
        };
        const double g0 = gr (nb - 48);
        fractionAt100ms = gr (nb + 4800) / g0;
        timeTo1dB = 1e9;
        for (size_t i = nb; i + 48 < n; i += 48)
            if (gr (i) < 1.0) { timeTo1dB = static_cast<double> (i - nb) / 48.0; break; }
    };
    double fShort, tShort, fLong, tLong;
    measure (0.5, fShort, tShort);
    measure (8.0, fLong, tLong);
    CHECK (fShort < 0.35, "short burst: %.0f %% of GR left after 100 ms", 100.0 * fShort);
    CHECK (fLong > fShort + 0.15, "memory: %.0f %% (long) vs %.0f %% (short) left after 100 ms", 100.0 * fLong, 100.0 * fShort);
    CHECK (tLong > 1.8 * tShort, "memory: recovery %.0f ms (long) vs %.0f ms (short)", tLong, tShort);
}

TEST (analogModelAttackCharacter)
{
    // Same attack control: FET is much faster than VCA; opto and vari-mu are slower.
    const double fs = 48000.0;
    double t63[4];
    for (auto m : kAllModels)
    {
        CompressorParams p = modelParams (m);
        p.detection = DetectionMode::peak;
        p.releaseMs = 1000.0;
        auto e = makeEngine (fs, p);
        Stereo s (48000);
        std::fill (s.l.begin(), s.l.end(), 1.0);
        std::fill (s.r.begin(), s.r.end(), 1.0);
        run (*e, s);
        const double fin = -20.0 * std::log10 (s.l.back());
        size_t i = 0;
        while (i < s.size() && -20.0 * std::log10 (s.l[i]) < fin * (1.0 - std::exp (-1.0)))
            ++i;
        t63[(int) m] = static_cast<double> (i) / 48.0;
    }
    const double vca = t63[0], opto = t63[1], vm = t63[2], fet = t63[3];
    CHECK (fet < 0.25 * vca, "FET %.2f ms vs VCA %.2f ms", fet, vca);
    CHECK (opto > 1.5 * vca && vm > 1.2 * vca, "opto %.2f / vari-mu %.2f vs VCA %.2f ms", opto, vm, vca);
}

TEST (modelDistortionGrowsWithGainReduction)
{
    // Relative to the VCA model with identical settings, vari-mu adds 2nd harmonic and FET adds
    // 3rd harmonic, and the excess grows with gain reduction.
    const double fs = 48000.0;
    auto harmonic = [&] (AnalogModel m, double thr, double harmonicHz) {
        CompressorParams p = modelParams (m);
        p.amplifier = true;
        p.driveDb = 6.0;
        p.character = 0.35;
        p.thresholdDb = thr;
        p.oversamplingLog2 = 2;
        p.attackMs = 20.0;
        p.releaseMs = 500.0;
        auto e = makeEngine (fs, p, 480);
        Stereo s = sine (fs, 1000.0, 0.5, 480 * 300);
        run (*e, s, 480);
        const size_t len = 32768, from = s.size() - len;
        return toneLevelDb (s.l, from, len, fs, harmonicHz) - toneLevelDb (s.l, from, len, fs, 1000.0);
    };
    const double vmLow = harmonic (AnalogModel::variMu, -8.0, 2000.0) - harmonic (AnalogModel::vca, -8.0, 2000.0);
    const double vmHigh = harmonic (AnalogModel::variMu, -26.0, 2000.0) - harmonic (AnalogModel::vca, -26.0, 2000.0);
    CHECK (vmHigh > vmLow + 5.0 && vmHigh > 5.0, "vari-mu excess H2: %.1f dB (low GR) -> %.1f dB (high GR)", vmLow, vmHigh);
    const double fetLow = harmonic (AnalogModel::fet, -8.0, 3000.0) - harmonic (AnalogModel::vca, -8.0, 3000.0);
    const double fetHigh = harmonic (AnalogModel::fet, -26.0, 3000.0) - harmonic (AnalogModel::vca, -26.0, 3000.0);
    CHECK (fetHigh > fetLow + 5.0 && fetHigh > 5.0, "FET excess H3: %.1f dB (low GR) -> %.1f dB (high GR)", fetLow, fetHigh);
    // opto is clean: no model distortion beyond the VCA reference
    const double optoHigh = harmonic (AnalogModel::opto, -26.0, 2000.0) - harmonic (AnalogModel::vca, -26.0, 2000.0);
    CHECK (std::abs (optoHigh) < 1.0, "opto excess H2 %.1f dB", optoHigh);
}

TEST (analogModelsFeedbackStress)
{
    // Each model inside the zero-delay feedback loop, with hold and auto release, extreme
    // settings and wild input: finite output, bounded solver work.
    const double fs = 48000.0;
    for (auto m : kAllModels)
    {
        CompressorParams p = modelParams (m);
        p.topology = Topology::feedBack;
        p.ratio = 20.0;
        p.attackMs = 0.01;
        p.releaseMs = 10.0;
        p.holdMs = 5.0;
        p.autoRelease = true;
        p.detection = DetectionMode::peak;
        auto e = makeEngine (fs, p);
        std::mt19937 rng (static_cast<unsigned> (m) + 40);
        std::uniform_real_distribution<double> u (-1.0, 1.0);
        Stereo s (48000);
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double a = (i / 3000) % 3 == 0 ? 8.0 : ((i / 3000) % 3 == 1 ? 0.0 : 1e-6);
            s.l[i] = a * u (rng);
            s.r[i] = (i % 700 < 350) ? a : -a;
        }
        run (*e, s);
        bool finite = true;
        for (size_t i = 0; i < s.size(); ++i)
            finite = finite && std::isfinite (s.l[i]) && std::isfinite (s.r[i]);
        CHECK (finite, "model %d: non-finite output", (int) m);
        CHECK (e->getAverageSolverEvaluations() < 8.0, "model %d: solver average %.2f", (int) m, e->getAverageSolverEvaluations());
    }
}


// ---------------------------------------------------------------------------------------------
// 17. audit fixes: robustness
TEST (nonFiniteInputCannotPoisonTheEngine)
{
    // NaN / Inf / absurd values on the main or sidechain input must not leave any state
    // non-finite. The engine treats non-finite samples as 0 and clamps to +/-1e6, so its output
    // must be bit-identical to a clean run fed those substitute values.
    const double fs = 48000.0;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (auto mode : { EngineMode::ultraTransparent, EngineMode::digital, EngineMode::analog, EngineMode::hybrid })
        for (auto topo : { Topology::feedForward, Topology::feedBack })
            for (auto det : { DetectionMode::rms, DetectionMode::truePeak })
            {
                CompressorParams p;
                p.mode = mode;
                p.topology = topo;
                p.detection = det;
                p.thresholdDb = -20.0;
                p.oversamplingLog2 = 2;
                p.lookaheadMs = 2.0;
                p.externalSidechain = true;
                auto bad = makeEngine (fs, p);
                auto ref = makeEngine (fs, p);
                Stereo a = sine (fs, 440.0, 0.3, 48000), b = a;
                Stereo scA = sine (fs, 90.0, 0.4, 48000), scB = scA;
                a.l[1000] = nan;          b.l[1000] = 0.0;
                a.r[2000] = -inf;         b.r[2000] = 0.0;
                a.l[3000] = 1.0e300;      b.l[3000] = 1.0e6;
                scA.l[4000] = inf;        scB.l[4000] = 0.0;
                scA.r[5000] = nan;        scB.r[5000] = 0.0;
                run (*bad, a, 512, &scA);
                run (*ref, b, 512, &scB);
                bool finite = true, identical = true;
                for (size_t i = 0; i < a.size(); ++i)
                {
                    finite = finite && std::isfinite (a.l[i]) && std::isfinite (a.r[i]);
                    identical = identical && a.l[i] == b.l[i] && a.r[i] == b.r[i];
                }
                CHECK (finite, "mode %d topo %d det %d: non-finite output", (int) mode, (int) topo, (int) det);
                CHECK (identical, "mode %d topo %d det %d: differs from the sanitised reference", (int) mode, (int) topo, (int) det);
                CHECK (bad->getRecoveryCount() == 0, "safety net triggered %lld times", bad->getRecoveryCount());
            }
}

TEST (lookaheadRapidChangesAreClickFree)
{
    // Lookahead changes arriving faster than the 20 ms crossfade (every 5 ms) are deferred to the
    // end of the running fade, never restarted mid-blend.
    const double fs = 48000.0;
    CompressorParams p = transparentDefaults();
    p.thresholdDb = 0.0;
    auto e = makeEngine (fs, p, 48);
    Stereo s = sine (fs, 200.0, 0.5, static_cast<size_t> (fs));
    const double maxStep = 0.5 * 2.0 * kPi * 200.0 / fs;
    for (size_t pos = 0, k = 0; pos + 48 <= s.size(); pos += 48)
    {
        if (pos % 240 == 0) // every 5 ms
        {
            p.lookaheadMs = 0.5 + 19.0 * static_cast<double> ((k * 7) % 11) / 10.0;
            ++k;
            e->setParams (p);
        }
        double* io[2] = { s.l.data() + pos, s.r.data() + pos };
        e->process (io, 2, nullptr, 0, 48);
    }
    double worst = 0.0;
    for (size_t i = 1; i < s.size(); ++i)
        worst = std::max (worst, std::abs (s.l[i] - s.l[i - 1]));
    // a crossfade between two copies of the sine can at most add their slopes
    CHECK (worst <= 2.0 * maxStep + 1e-9, "largest step %.4f (signal's own max step %.4f)", worst, maxStep);
}

TEST (exactLookaheadSurvivesStructuralChangesAndHybridFeedback)
{
    // Zero overshoot must hold (a) while oversampling and colour switches toggle during playback
    // and (b) in Hybrid mode with the feedback topology.
    const double fs = 48000.0;
    for (int scenario = 0; scenario < 2; ++scenario)
    {
        CompressorParams p = transparentDefaults();
        p.mode = EngineMode::hybrid;
        p.topology = scenario == 1 ? Topology::feedBack : Topology::feedForward;
        p.thresholdDb = -20.0;
        p.ratio = 20.0;
        p.attackMs = 1.0;
        p.releaseMs = 50.0;
        p.detection = DetectionMode::peak;
        p.lookaheadMs = 5.0;
        p.hybridBlend = 0.7;
        p.transformer = p.amplifier = false;
        p.colorMix = 0.0; // colour stage may switch on, but must not change levels for this check
        auto e = makeEngine (fs, p, 480);
        Stereo s (48000);
        for (size_t i = 0; i < s.size(); ++i)
            s.l[i] = s.r[i] = 0.01 * std::sin (2.0 * kPi * 300.0 * i / fs)
                              + ((i % 4800 < 24) ? std::sin (2.0 * kPi * 5000.0 * i / fs) : 0.0);
        const Stereo in = s;
        std::vector<int> latencyAt (s.size(), e->getLatencySamples());
        for (size_t pos = 0, k = 0; pos < s.size(); pos += 480, ++k)
        {
            if (scenario == 0 && k % 10 == 5)
            {
                p.oversamplingLog2 = static_cast<int> (k / 10) % 3; // 0, 1, 2 -> latency changes
                p.amplifier = ! p.amplifier;
                e->setParams (p);
            }
            double* io[2] = { s.l.data() + pos, s.r.data() + pos };
            e->process (io, 2, nullptr, 0, 480);
        }
        // compare only regions away from latency changes (where the delay crossfades)
        const size_t L = static_cast<size_t> (e->getLatencySamples());
        double worst = -1e9;
        for (size_t i = 4800 + L; i < s.size(); ++i)
        {
            if (scenario == 0)
                continue; // latency moves in scenario 0: checked below via the static lookahead
            const double x = std::abs (in.l[i - L]);
            if (x < 1e-4)
                continue;
            const double xd = 20.0 * std::log10 (x);
            worst = std::max (worst, 20.0 * std::log10 (std::abs (s.l[i]) + 1e-300) - GainComputer::staticOutput (xd, -20.0, 20.0, 0.0));
        }
        if (scenario == 1)
            CHECK (worst <= 1e-6, "hybrid + feedback: overshoot %.3f dB", worst);
        else
        {
            // with colour toggling the latency moves, so measure the burst peaks directly: every
            // burst must come out at or below the static-curve level of a 0 dBFS input
            const double allowed = dbToGain (GainComputer::staticOutput (0.0, -20.0, 20.0, 0.0)) * 1.000001;
            double peak = 0.0;
            for (size_t i = 9600; i < s.size(); ++i)
                peak = std::max (peak, std::abs (s.l[i]));
            CHECK (peak <= allowed, "structural toggles: burst peak %.4f > allowed %.4f", peak, allowed);
        }
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
