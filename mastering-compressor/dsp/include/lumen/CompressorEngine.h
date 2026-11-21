// Lumen MC-64 — complete mastering compressor engine (64-bit, real-time safe after prepare()).
//
// Signal flow (per channel pair):
//
//   in ─┬─────────────────────────────── dry delay (lookahead + colour latency) ───────┐
//       │                                                                             │
//       ├─ [M/S encode] ─ lookahead delay ─ × gain ─ [M/S decode] ─ colour* ─ makeup ─ mix ─ bypass ─ out
//       │                                     ▲
//   sc ─┴─ [M/S encode] ─ SC HPF/LPF ─ detector ─ gain computer ─ lookahead max ─ ballistics ─ link
//                                                   (FF, or FB from previous output)
//
//   * colour = optional transformer/amplifier stage, oversampled, only in Analog/Hybrid modes.
//
// Real-time rules: prepare() allocates; process() never allocates, locks or does I/O.
#pragma once

#include <array>
#include <vector>

#include "ColorStage.h"
#include "DspCommon.h"
#include "Envelope.h"
#include "FeedbackSolver.h"
#include "Filters.h"
#include "GainComputer.h"
#include "Lookahead.h"

namespace lumen
{
enum class EngineMode { ultraTransparent = 0, digital = 1, analog = 2, hybrid = 3 };
enum class ChannelMode { stereo = 0, dualMono = 1, midSide = 2 };
enum class Topology { feedForward = 0, feedBack = 1 };
enum class LookaheadMode { exact = 0, classic = 1 };

struct CompressorParams
{
    EngineMode mode = EngineMode::ultraTransparent;
    double thresholdDb = -12.0;  // -60..0
    double ratio = 2.0;          // 1..20
    double attackMs = 10.0;      // 0.01..250
    double releaseMs = 200.0;    // 10..5000
    double kneeDb = 6.0;         // 0..24
    double makeupDb = 0.0;       // -24..24
    double mix = 1.0;            // 0..1
    double stereoLink = 1.0;     // 0..1
    bool scHpfOn = true;
    double scHpfHz = 60.0;       // 20..500
    bool scLpfOn = false;
    double scLpfHz = 16000.0;    // 1k..20k
    bool autoRelease = false;
    AutoReleaseShape autoReleaseShape = AutoReleaseShape::program;
    double holdMs = 0.0;         // 0..50
    bool autoGain = false;
    double lookaheadMs = 0.0;    // 0..20
    LookaheadMode lookaheadMode = LookaheadMode::exact; // exact: zero overshoot at the onset
    DetectionMode detection = DetectionMode::peak;
    bool externalSidechain = false;
    ChannelMode channelMode = ChannelMode::stereo;
    Topology topology = Topology::feedForward; // Analog/Hybrid only
    AnalogModel analogModel = AnalogModel::vca; // Analog/Hybrid only
    double hybridBlend = 0.5;    // 0 = digital envelope, 1 = analog envelope
    // analog colour (Analog/Hybrid)
    bool transformer = true;
    bool amplifier = true;
    double driveDb = 6.0;        // 0..24
    double character = 0.35;     // 0..1
    double colorMix = 1.0;       // 0..1
    int oversamplingLog2 = 2;    // 0 = off, 1..4 = 2x..16x (for the colour stage)
    OversamplingQuality oversamplingQuality = OversamplingQuality::linearPhaseHQ;
    // comparison
    bool bypass = false;
    bool loudnessMatchedBypass = true;
};

struct MeterSnapshot
{
    double inputPeak[2] {};    // linear, L/R
    double outputPeak[2] {};   // linear, L/R
    double midPeak = 0.0, sidePeak = 0.0; // output, linear
    double inMidPeak = 0.0, inSidePeak = 0.0;
    double gainReductionDb[2] {}; // positive dB, max in block (processing domain)
    double detectorLevelDb = kMinusInfDb; // max detector level in block (for the transfer curve)
    double autoGainDb = 0.0;
    double loudnessMatchDb = 0.0;
};

class CompressorEngine
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kMaxLookaheadSamples = 7680 + 1; // 20 ms @ 384 kHz
    static constexpr int kMaxColourLatency = 128;
    static constexpr int kMaxDelay = kMaxLookaheadSamples + kMaxColourLatency + 2;

    CompressorEngine() = default;

    /** Not real-time safe: allocates scratch buffers. */
    void prepare (double newSampleRate, int newMaxBlockSize)
    {
        sampleRate = newSampleRate;
        maxBlockSize = std::max (1, newMaxBlockSize);
        for (auto& b : wet)
            b.assign (static_cast<size_t> (maxBlockSize), 0.0);
        for (auto& b : dry)
            b.assign (static_cast<size_t> (maxBlockSize), 0.0);
        grBuffer.assign (static_cast<size_t> (maxBlockSize), 0.0);

        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            detDigital[ch].prepare (sampleRate);
            detAnalog[ch].prepare (sampleRate);
            envDigital[ch].prepare (sampleRate);
            envAnalog[ch].prepare (sampleRate);
            crest[ch].prepare (sampleRate);
            kIn[ch].setup (sampleRate);
            kOut[ch].setup (sampleRate);
        }
        colour.prepare (sampleRate, maxBlockSize);

        thresholdS.reset (sampleRate, 30.0);
        ratioS.reset (sampleRate, 30.0);
        kneeS.reset (sampleRate, 30.0);
        makeupS.reset (sampleRate, 30.0);
        mixS.reset (sampleRate, 30.0);
        linkS.reset (sampleRate, 30.0);
        blendS.reset (sampleRate, 50.0);
        bypassS.reset (sampleRate, 20.0);

        autoGainCoeff = timeConstantToCoeff (2500.0, sampleRate);
        loudnessCoeff = timeConstantToCoeff (3000.0, sampleRate);
        antiModCoeff = 0.0;
        autoGainS.coeff = timeConstantToCoeff (60.0, sampleRate);
        depthCoeff = timeConstantToCoeff (200.0, sampleRate);

        firstParams = true;
        setParams (params);
        reset();
    }

    void reset() noexcept
    {
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            detDigital[ch].reset();
            detAnalog[ch].reset();
            envDigital[ch].reset();
            envAnalog[ch].reset();
            crest[ch].reset();
            smoothedReduction[ch] = 0.0;
            hpfD[ch].reset(); lpfD[ch].reset();
            hpfA[ch].reset(); lpfA[ch].reset();
            tilt[ch].reset();
            laDelay[ch].clear();
            dryDelay[ch].clear();
            laShapeD[ch].reset();
            laShapeA[ch].reset();
            kIn[ch].reset();
            kOut[ch].reset();
            antiMod[ch] = 0.0;
        }
        colour.reset();
        avgReduction = 0.0;
        autoGainS.value = autoGainS.target = 0.0;
        msIn = msOut = 0.0;
        applyDelays (true);
    }

    /** Call at the start of each block (audio thread). Cheap unless timing parameters change. */
    void setParams (const CompressorParams& p) noexcept
    {
        const bool structural = firstParams
                                || p.lookaheadMs != params.lookaheadMs
                                || p.lookaheadMode != params.lookaheadMode
                                || p.detection != params.detection
                                || p.oversamplingLog2 != params.oversamplingLog2
                                || p.oversamplingQuality != params.oversamplingQuality
                                || p.mode != params.mode
                                || p.transformer != params.transformer
                                || p.amplifier != params.amplifier;
        const bool timing = firstParams || structural
                            || p.attackMs != params.attackMs || p.releaseMs != params.releaseMs
                            || p.autoRelease != params.autoRelease || p.ratio != params.ratio
                            || p.autoReleaseShape != params.autoReleaseShape || p.holdMs != params.holdMs
                            || p.analogModel != params.analogModel
                            || p.topology != params.topology || p.externalSidechain != params.externalSidechain;
        const bool scFilters = firstParams || p.scHpfHz != params.scHpfHz || p.scLpfHz != params.scLpfHz
                               || p.scHpfOn != params.scHpfOn || p.scLpfOn != params.scLpfOn;

        params = p;
        params.thresholdDb = clampValue (p.thresholdDb, -60.0, 0.0);
        params.ratio = clampValue (p.ratio, 1.0, 20.0);
        params.attackMs = clampValue (p.attackMs, 0.01, 250.0);
        params.releaseMs = clampValue (p.releaseMs, 10.0, 5000.0);
        params.kneeDb = clampValue (p.kneeDb, 0.0, 24.0);
        params.makeupDb = clampValue (p.makeupDb, -24.0, 24.0);
        params.mix = clampValue (p.mix, 0.0, 1.0);
        params.stereoLink = clampValue (p.stereoLink, 0.0, 1.0);
        params.scHpfHz = clampValue (p.scHpfHz, 20.0, 500.0);
        params.scLpfHz = clampValue (p.scLpfHz, 1000.0, 20000.0);
        params.lookaheadMs = clampValue (p.lookaheadMs, 0.0, 20.0);
        params.holdMs = clampValue (p.holdMs, 0.0, 50.0);
        params.hybridBlend = clampValue (p.hybridBlend, 0.0, 1.0);
        params.driveDb = clampValue (p.driveDb, 0.0, 24.0);
        params.character = clampValue (p.character, 0.0, 1.0);
        params.colorMix = clampValue (p.colorMix, 0.0, 1.0);
        params.oversamplingLog2 = clampValue (p.oversamplingLog2, 0, 4);

        if (firstParams)
        {
            thresholdS.setCurrentAndTarget (params.thresholdDb);
            ratioS.setCurrentAndTarget (params.ratio);
            kneeS.setCurrentAndTarget (params.kneeDb);
            makeupS.setCurrentAndTarget (params.makeupDb);
            mixS.setCurrentAndTarget (params.mix);
            linkS.setCurrentAndTarget (effectiveLink());
            blendS.setCurrentAndTarget (params.hybridBlend);
            bypassS.setCurrentAndTarget (params.bypass ? 1.0 : 0.0);
        }
        else
        {
            thresholdS.setTarget (params.thresholdDb);
            ratioS.setTarget (params.ratio);
            kneeS.setTarget (params.kneeDb);
            makeupS.setTarget (params.makeupDb);
            mixS.setTarget (params.mix);
            linkS.setTarget (effectiveLink());
            blendS.setTarget (params.hybridBlend);
            bypassS.setTarget (params.bypass ? 1.0 : 0.0);
        }

        if (structural)
        {
            colour.configureOversampling (params.oversamplingLog2, params.oversamplingQuality);
            applyDelays (firstParams);
        }

        if (timing)
            configureBallistics();

        if (scFilters)
            configureSidechainFilters();

        ColorSettings cs;
        cs.transformer = params.transformer;
        cs.amplifier = params.amplifier;
        cs.driveDb = params.driveDb;
        cs.character = params.character;
        cs.amount = params.colorMix;
        cs.model = params.analogModel;
        colour.setSettings (cs);

        firstParams = false;
    }

    const CompressorParams& getParams() const noexcept { return params; }

    /** Total latency in samples for the current settings (report to the host). */
    int getLatencySamples() const noexcept
    {
        return lookaheadSamples + truePeakDelay() + (colourActive() ? colour.getLatencySamples() : 0);
    }

    /** Effective knee shown in the UI / used by the analog path (each model has a minimum knee). */
    static double analogKnee (double kneeDb, AnalogModel model = AnalogModel::vca) noexcept
    {
        return analogModelKnee (model, kneeDb);
    }

    /** Process numSamples in place. sidechain may be null (or numScChannels 0). */
    void process (double* const* io, int numChannels, const double* const* sidechain, int numScChannels,
                  int numSamples) noexcept
    {
        ScopedNoDenormals noDenormals;
        meters = MeterSnapshot {};
        numChannels = clampValue (numChannels, 1, kMaxChannels);

        for (int offset = 0; offset < numSamples; offset += maxBlockSize)
        {
            const int n = std::min (maxBlockSize, numSamples - offset);
            double* chunk[kMaxChannels] = { io[0] + offset, numChannels > 1 ? io[1] + offset : nullptr };
            const double* sc[kMaxChannels] = { nullptr, nullptr };
            int scCh = 0;
            if (params.externalSidechain && sidechain != nullptr && numScChannels > 0)
            {
                scCh = std::min (numScChannels, kMaxChannels);
                for (int c = 0; c < scCh; ++c)
                    sc[c] = sidechain[c] + offset;
            }
            processChunk (chunk, numChannels, sc, scCh, n);

            // Safety net: inputs are sanitised, so this should never trigger; if any state still
            // went non-finite, recover instead of emitting NaN forever: reset and output silence.
            bool finite = true;
            for (int c = 0; c < numChannels; ++c)
                for (int i = 0; i < n; ++i)
                    finite = finite && std::isfinite (chunk[c][i]);
            if (! finite)
            {
                reset();
                for (int c = 0; c < numChannels; ++c)
                    std::fill (chunk[c], chunk[c] + n, 0.0);
                ++recoveries;
            }
        }
    }

    const MeterSnapshot& getMeters() const noexcept { return meters; }

    /** Number of times the non-finite safety net had to reset the engine (diagnostics). */
    long long getRecoveryCount() const noexcept { return recoveries; }

private:
    double effectiveLink() const noexcept
    {
        return params.channelMode == ChannelMode::dualMono ? 0.0 : params.stereoLink;
    }

    bool usesDigitalPath() const noexcept
    {
        return params.mode == EngineMode::ultraTransparent || params.mode == EngineMode::digital
               || params.mode == EngineMode::hybrid;
    }

    bool usesAnalogPath() const noexcept
    {
        return params.mode == EngineMode::analog || params.mode == EngineMode::hybrid;
    }

    bool colourActive() const noexcept
    {
        return usesAnalogPath() && (params.transformer || params.amplifier);
    }

    /** True-peak detection reports the interval that started kTruePeakDelay samples ago; the audio
        is delayed by the same amount so detection stays sample-aligned (and exact lookahead keeps
        its guarantee). */
    int truePeakDelay() const noexcept
    {
        return params.detection == DetectionMode::truePeak ? LevelDetector::kTruePeakDelay : 0;
    }

    bool feedback() const noexcept
    {
        return usesAnalogPath() && params.topology == Topology::feedBack && ! params.externalSidechain;
    }

    /** immediate = true on prepare/reset; otherwise delay changes crossfade over 20 ms. */
    void applyDelays (bool immediate = false) noexcept
    {
        lookaheadSamples = clampValue (static_cast<int> (std::lround (params.lookaheadMs * 0.001 * sampleRate)),
                                       0, kMaxLookaheadSamples - 1);
        const int total = getLatencySamples();
        const int fade = std::max (1, static_cast<int> (std::lround (0.02 * sampleRate)));
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            laDelay[ch].setFadeLength (fade);
            dryDelay[ch].setFadeLength (fade);
            laDelay[ch].setDelay (lookaheadSamples + truePeakDelay(), immediate);
            dryDelay[ch].setDelay (total, immediate);
        }
        // only reconfigures (and resets) the shapers if lookahead / kernel / mode changed, so
        // switching mode, oversampling, colour or detector keeps the exact-lookahead state
        configureLookaheadShapers (immediate);
    }

    /** Exact mode: kernel length from the attack time. The triangular kernel's step response
        reaches 63 % at ~0.59·W, so W = 1.7·attack keeps the attack control meaningful; it is
        capped at L + 1, the longest kernel for which the zero-overshoot guarantee holds. */
    void configureLookaheadShapers (bool force) noexcept
    {
        const bool exact = params.lookaheadMode == LookaheadMode::exact;
        const int kernel = clampValue (static_cast<int> (std::lround (1.7 * params.attackMs * 0.001 * sampleRate)),
                                       1, lookaheadSamples + 1);
        if (! force && kernel == shaperKernel && exact == shaperExact && lookaheadSamples == shaperLookahead)
            return;
        shaperKernel = kernel;
        shaperExact = exact;
        shaperLookahead = lookaheadSamples;
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            laShapeD[ch].configure (lookaheadSamples, kernel, exact);
            laShapeA[ch].configure (lookaheadSamples, kernel, exact);
        }
    }

    void configureBallistics() noexcept
    {
        BallisticsSettings b;
        b.attackMs = params.attackMs;
        b.releaseMs = params.releaseMs;
        b.autoRelease = params.autoRelease;
        b.shape = params.autoReleaseShape;
        b.holdMs = params.holdMs;
        b.model = params.analogModel;
        releaseShape = ReleaseShapeTable::get (params.autoReleaseShape);

        // Feedback runs as a zero-delay loop (FeedbackSolver.h), which is stable for any
        // coefficient; the analog envelope pre-warps its coefficients by the loop gain k = R - 1
        // so attack/release times mean the same in FB as in FF.
        const double loopGain = feedback() ? params.ratio - 1.0 : 0.0;
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            envDigital[ch].configure (b);
            envAnalog[ch].configure (b, loopGain);
        }
        configureLookaheadShapers (false);
        // Ultra Transparent: an extra pole on the gain trajectory rounds off corners of the
        // one-pole envelope, which lowers gain-modulation sidebands (less audible distortion).
        antiModCoeff = timeConstantToCoeff (clampValue (params.attackMs * 0.25, 0.02, 0.5), sampleRate);
    }

    void configureSidechainFilters() noexcept
    {
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            hpfD[ch].setup (Svf::Type::highpass, params.scHpfHz, 0.7071067811865476, sampleRate);
            hpfA[ch].setup (Svf::Type::highpass, params.scHpfHz, 0.7071067811865476, sampleRate);
            lpfD[ch].setup (Svf::Type::lowpass, params.scLpfHz, 0.7071067811865476, sampleRate);
            lpfA[ch].setup (Svf::Type::lowpass, params.scLpfHz, 0.7071067811865476, sampleRate);
            // analog detector: ~3 dB less sensitive below 90 Hz, +1.5 dB above 5 kHz
            tilt[ch].setup (90.0, -3.0, 5000.0, 1.5, sampleRate);
        }
    }

    void processChunk (double* const* io, int numChannels, const double* const* sc, int numScChannels, int n) noexcept
    {
        const bool stereo = numChannels == 2;
        const bool ms = stereo && params.channelMode == ChannelMode::midSide;
        const bool useD = usesDigitalPath();
        const bool useA = usesAnalogPath();
        const bool fb = feedback();
        const bool ultra = params.mode == EngineMode::ultraTransparent;
        const EngineMode mode = params.mode;
        const bool hpOn = params.scHpfOn, lpOn = params.scLpfOn;
        const DetectionMode det = params.detection;
        const bool exactLA = params.lookaheadMode == LookaheadMode::exact && lookaheadSamples > 0;

        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            detDigital[ch].setMode (det);
            // true peak needs future samples, so it cannot sit inside the zero-delay loop
            detAnalog[ch].setMode (fb && det == DetectionMode::truePeak ? DetectionMode::peak : det);
        }
        const bool adaptRelease = params.autoRelease;

        double* wetL = wet[0].data();
        double* wetR = wet[1].data();
        double* dryL = dry[0].data();
        double* dryR = dry[1].data();

        for (int i = 0; i < n; ++i)
        {
            const double thr = thresholdS.next();
            const double ratio = ratioS.next();
            const double knee = kneeS.next();
            const double kneeA = analogKnee (knee, params.analogModel);
            const double link = linkS.next();
            const double blend = blendS.next();

            double x[2], s[2];
            x[0] = sanitise (io[0][i]);
            x[1] = stereo ? sanitise (io[1][i]) : x[0];

            if (numScChannels > 0)
            {
                s[0] = sanitise (sc[0][i]);
                s[1] = numScChannels > 1 ? sanitise (sc[1][i]) : s[0];
            }
            else
            {
                s[0] = x[0];
                s[1] = x[1];
            }

            // input metering (L/R and M/S)
            meters.inputPeak[0] = std::max (meters.inputPeak[0], std::abs (x[0]));
            meters.inputPeak[1] = std::max (meters.inputPeak[1], std::abs (x[1]));
            meters.inMidPeak = std::max (meters.inMidPeak, std::abs (0.5 * (x[0] + x[1])));
            meters.inSidePeak = std::max (meters.inSidePeak, std::abs (0.5 * (x[0] - x[1])));

            // dry path stays in L/R and is delayed by the full latency
            dryL[i] = dryDelay[0].process (x[0]);
            dryR[i] = dryDelay[1].process (x[1]);

            double p[2] = { x[0], x[1] };
            if (ms)
            {
                p[0] = 0.5 * (x[0] + x[1]);
                p[1] = 0.5 * (x[0] - x[1]);
                const double sm = 0.5 * (s[0] + s[1]);
                const double ss = 0.5 * (s[0] - s[1]);
                s[0] = sm;
                s[1] = ss;
            }

            double r[2] = { 0.0, 0.0 };
            double delayed[2] = { 0.0, 0.0 };
            const int procCh = stereo ? 2 : 1;
            if (adaptRelease && --adaptCountdown <= 0)
            {
                adaptCountdown = kAdaptInterval;
                for (int ch = 0; ch < procCh; ++ch)
                {
                    const double m = releaseShape.multiplier (smoothedReduction[ch], crest[ch].getCrestDb());
                    envDigital[ch].setReleaseScale (m);
                    envAnalog[ch].setReleaseScale (m);
                }
            }

            for (int ch = 0; ch < procCh; ++ch)
            {
                delayed[ch] = laDelay[ch].process (p[ch]);
                if (adaptRelease)
                    crest[ch].process (s[ch]);
                double rD = 0.0, rA = 0.0, floorD = 0.0;
                if (useD)
                {
                    double d = s[ch];
                    if (hpOn) d = hpfD[ch].process (d);
                    if (lpOn) d = lpfD[ch].process (d);
                    const double level = detDigital[ch].process (d);
                    meters.detectorLevelDb = std::max (meters.detectorLevelDb, level);
                    const double target = -GainComputer::feedForward (level, thr, ratio, knee);
                    const double shaped = laShapeD[ch].process (target);
                    rD = envDigital[ch].process (shaped);
                    if (ultra)
                    {
                        antiMod[ch] = flushDenormal (antiModCoeff * antiMod[ch] + (1.0 - antiModCoeff) * rD);
                        rD = antiMod[ch];
                    }
                    if (exactLA)
                        rD = std::max (rD, shaped); // ballistics may only add reduction
                    floorD = shaped;
                }
                if (useA && fb)
                {
                    rA = solveFeedback (ch, delayed[ch], mode == EngineMode::hybrid ? rD : 0.0,
                                        mode == EngineMode::hybrid ? blend : 1.0, thr, ratio, kneeA, hpOn, lpOn);
                }
                else if (useA)
                {
                    double d = s[ch];
                    if (hpOn) d = hpfA[ch].process (d);
                    if (lpOn) d = lpfA[ch].process (d);
                    d = tilt[ch].process (d);
                    const double level = detAnalog[ch].process (d);
                    const double shaped = laShapeA[ch].process (-GainComputer::feedForward (level, thr, ratio, kneeA));
                    if (! useD)
                        meters.detectorLevelDb = std::max (meters.detectorLevelDb, level);
                    rA = envAnalog[ch].process (shaped);
                    if (exactLA)
                        rA = std::max (rA, shaped);
                }

                switch (mode)
                {
                    case EngineMode::ultraTransparent:
                    case EngineMode::digital: r[ch] = rD; break;
                    case EngineMode::analog:  r[ch] = rA; break;
                    case EngineMode::hybrid:  r[ch] = rD + blend * (rA - rD); break;
                }
                // Hybrid + feedback: the FB half cannot look ahead, so re-apply the digital path's
                // exact-lookahead floor after blending (FF hybrid already satisfies it: a convex
                // blend of two values that are each >= their own floor)
                if (exactLA && fb && mode == EngineMode::hybrid)
                    r[ch] = std::max (r[ch], floorD);
            }

            if (stereo && link > 0.0)
            {
                const double linked = std::max (r[0], r[1]);
                r[0] += link * (linked - r[0]);
                r[1] += link * (linked - r[1]);
            }

            for (int ch = 0; ch < procCh; ++ch)
            {
                const double g = r[ch] == 0.0 ? 1.0 : std::exp (-r[ch] * kLn10Over20);
                p[ch] = delayed[ch] * g;
                meters.gainReductionDb[ch] = std::max (meters.gainReductionDb[ch], r[ch]);
                smoothedReduction[ch] = depthCoeff * smoothedReduction[ch] + (1.0 - depthCoeff) * r[ch];
                avgReductionAccum += r[ch];
            }
            grBuffer[static_cast<size_t> (i)] = stereo ? std::max (r[0], r[1]) : r[0];
            avgReduction = autoGainCoeff * avgReduction + (1.0 - autoGainCoeff) * (avgReductionAccum / procCh);
            avgReductionAccum = 0.0;

            if (ms)
            {
                wetL[i] = p[0] + p[1];
                wetR[i] = p[0] - p[1];
            }
            else
            {
                wetL[i] = p[0];
                wetR[i] = stereo ? p[1] : p[0];
            }
        }

        if (colourActive())
        {
            double* chans[2] = { wetL, wetR };
            colour.process (chans, numChannels, n, grBuffer.data());
        }

        // makeup, auto gain, mix, loudness-matched bypass
        const double autoGainDb = params.autoGain ? avgReduction : 0.0;
        autoGainS.setTarget (autoGainDb);
        meters.autoGainDb = autoGainDb;

        for (int i = 0; i < n; ++i)
        {
            const double makeupDb = makeupS.next() + autoGainS.next();
            const double makeup = makeupDb == 0.0 ? 1.0 : dbToGain (makeupDb);
            const double mix = mixS.next();
            const double byp = bypassS.next();

            double out[2], dr[2] = { dryL[i], dryR[i] };
            double w[2] = { wetL[i] * makeup, wetR[i] * makeup };

            for (int ch = 0; ch < 2; ++ch)
            {
                if (mix >= 1.0)      out[ch] = w[ch];
                else if (mix <= 0.0) out[ch] = dr[ch];
                else                 out[ch] = dr[ch] * (1.0 - mix) + w[ch] * mix;
            }

            // loudness tracking (K-weighted mean square, ~3 s)
            {
                const double a = kIn[0].process (dr[0]);
                const double b = stereo ? kIn[1].process (dr[1]) : a;
                const double c = kOut[0].process (out[0]);
                const double d = stereo ? kOut[1].process (out[1]) : c;
                msIn = loudnessCoeff * msIn + (1.0 - loudnessCoeff) * 0.5 * (a * a + b * b);
                msOut = loudnessCoeff * msOut + (1.0 - loudnessCoeff) * 0.5 * (c * c + d * d);
            }

            if (byp > 0.0)
            {
                const double match = params.loudnessMatchedBypass ? loudnessMatchGain() : 1.0;
                for (int ch = 0; ch < 2; ++ch)
                {
                    const double bypassed = dr[ch] * match;
                    out[ch] = byp >= 1.0 ? bypassed : out[ch] + byp * (bypassed - out[ch]);
                }
            }

            io[0][i] = out[0];
            if (stereo)
                io[1][i] = out[1];

            meters.outputPeak[0] = std::max (meters.outputPeak[0], std::abs (out[0]));
            meters.outputPeak[1] = std::max (meters.outputPeak[1], std::abs (out[1]));
            meters.midPeak = std::max (meters.midPeak, std::abs (0.5 * (out[0] + out[1])));
            meters.sidePeak = std::max (meters.sidePeak, std::abs (0.5 * (out[0] - out[1])));
        }
        meters.loudnessMatchDb = gainToDb (loudnessMatchGain());
        if (! stereo)
        {
            meters.inputPeak[1] = meters.inputPeak[0];
            meters.outputPeak[1] = meters.outputPeak[0];
            meters.gainReductionDb[1] = meters.gainReductionDb[0];
        }
    }

    /** Input guard. A NaN/Inf sample from upstream would otherwise enter every recursive state
        (filters, detectors, envelopes, oversampler) and keep the output non-finite forever.
        Non-finite samples become 0; finite ones are clamped to +/-1e6 (+120 dBFS), far above any
        real signal, so squaring in the detectors cannot overflow. Normal samples pass unchanged
        (bit-exact), so the transparent-path null tests are unaffected. */
    static inline double sanitise (double v) noexcept
    {
        if (! std::isfinite (v))
            return 0.0;
        return v > 1.0e6 ? 1.0e6 : (v < -1.0e6 ? -1.0e6 : v);
    }

    /** Zero-delay feedback: solve r = H(r) for this sample (see FeedbackSolver.h), then commit
        the detector chain and envelope with the converged output. The loop is closed around the
        envelope before its RC rounding stage (see AnalogEnvelope::peekBeforeRc); the returned,
        applied reduction includes the RC stage.
        u: delayed input sample; the output is u·10^(-(r0 + w·r)/20). */
    double solveFeedback (int ch, double u, double rD, double w, double thr, double ratio, double kneeDb, bool hpOn,
                          bool lpOn) noexcept
    {
        const double r0 = rD * (1.0 - w);
        auto chainPeek = [&] (double y) {
            double d = y;
            if (hpOn) d = hpfA[ch].peek (d);
            if (lpOn) d = lpfA[ch].peek (d);
            return tilt[ch].peek (d);
        };
        // The sidechain chain is linear: d = A·y + B
        const double B = chainPeek (0.0);
        const double A = chainPeek (1.0) - B;
        auto output = [&] (double r) { return u * std::exp (-(r0 + w * r) * kLn10Over20); };
        auto H = [&] (double r) {
            const double level = detAnalog[ch].peek (A * output (r) + B);
            return envAnalog[ch].peekBeforeRc (-GainComputer::feedBack (level, thr, ratio, kneeDb));
        };
        const auto res = FeedbackSolver::solve (H, envAnalog[ch].getOutputBeforeRc());
        zdfSolves += 1;
        zdfEvaluations += res.evaluations;

        // commit state with the solution
        double d = output (res.r);
        if (hpOn) d = hpfA[ch].process (d);
        if (lpOn) d = lpfA[ch].process (d);
        d = tilt[ch].process (d);
        const double level = detAnalog[ch].process (d);
        return envAnalog[ch].process (-GainComputer::feedBack (level, thr, ratio, kneeDb));
    }

public:
    /** Current programme-dependent release multiplier of a channel (diagnostics). */
    double getReleaseMultiplier (int ch) const noexcept { return envDigital[clampValue (ch, 0, kMaxChannels - 1)].getReleaseScale(); }

    /** Average number of loop evaluations per zero-delay feedback solve (diagnostics). */
    double getAverageSolverEvaluations() const noexcept
    {
        return zdfSolves > 0 ? static_cast<double> (zdfEvaluations) / static_cast<double> (zdfSolves) : 0.0;
    }

private:
    /** Gain applied to the dry signal while bypassed so it matches the processed loudness. */
    double loudnessMatchGain() const noexcept
    {
        if (msIn < 1.0e-10 || msOut < 1.0e-10)
            return 1.0;
        return clampValue (std::sqrt (msOut / msIn), 0.0631, 15.85); // ±24 dB
    }

    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    CompressorParams params;
    bool firstParams = true;
    int lookaheadSamples = 0;

    std::array<std::vector<double>, kMaxChannels> wet, dry;
    std::vector<double> grBuffer;

    LevelDetector detDigital[kMaxChannels], detAnalog[kMaxChannels];
    DigitalEnvelope envDigital[kMaxChannels];
    AnalogEnvelope envAnalog[kMaxChannels];
    Svf hpfD[kMaxChannels], lpfD[kMaxChannels], hpfA[kMaxChannels], lpfA[kMaxChannels];
    DetectorTilt tilt[kMaxChannels];
    CrossfadingDelay<kMaxLookaheadSamples> laDelay[kMaxChannels];
    CrossfadingDelay<kMaxDelay> dryDelay[kMaxChannels];
    LookaheadShaper<kMaxLookaheadSamples> laShapeD[kMaxChannels], laShapeA[kMaxChannels];
    int shaperKernel = -1, shaperLookahead = -1;
    bool shaperExact = false;
    long long zdfSolves = 0, zdfEvaluations = 0, recoveries = 0;
    KWeighting kIn[kMaxChannels], kOut[kMaxChannels];
    ColorStage<kMaxChannels> colour;

    LinearSmoother thresholdS, ratioS, kneeS, makeupS, mixS, linkS, blendS, bypassS;
    struct OnePoleSmoother
    {
        double coeff = 0.0, value = 0.0, target = 0.0;
        void setTarget (double t) noexcept { target = t; }
        double next() noexcept
        {
            value = coeff * value + (1.0 - coeff) * target;
            if (std::abs (value - target) < 1.0e-9)
                value = target; // lands exactly (keeps the unity-gain path bit-exact)
            return value;
        }
    } autoGainS;

    double antiMod[kMaxChannels] {};
    CrestEstimator crest[kMaxChannels];
    double smoothedReduction[kMaxChannels] {};
    double depthCoeff = 0.0;
    ReleaseShapeTable releaseShape = ReleaseShapeTable::get (AutoReleaseShape::program);
    static constexpr int kAdaptInterval = 32;
    int adaptCountdown = 0;
    double antiModCoeff = 0.0;
    double avgReduction = 0.0, avgReductionAccum = 0.0, autoGainCoeff = 0.0;
    double msIn = 0.0, msOut = 0.0, loudnessCoeff = 0.0;
    MeterSnapshot meters;
};

} // namespace lumen
