// Lumen MC-64 — shared DSP utilities (double precision, header-only, no JUCE dependency).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #include <immintrin.h>
  #define LUMEN_HAS_SSE 1
#endif

namespace lumen
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kLn10Over20 = 0.11512925464970228420; // ln(10) / 20
constexpr double kMinusInfDb = -300.0;

/** dB -> linear gain. dbToGain(0.0) is exactly 1.0 (exp(0) == 1), which the null tests rely on. */
inline double dbToGain (double db) noexcept { return std::exp (db * kLn10Over20); }

/** linear gain -> dB with a floor far below anything audible. */
inline double gainToDb (double g) noexcept
{
    return g > 1.0e-15 ? std::log (g) / kLn10Over20 : kMinusInfDb;
}

/** Flush values below ~-600 dBFS to zero. Applied to recursive filter state only, so it never
    touches the audible signal (the smallest 24-bit step is ~-144 dBFS). */
inline double flushDenormal (double x) noexcept { return std::abs (x) < 1.0e-30 ? 0.0 : x; }

/** One-pole coefficient for a time constant (time to reach 1 - 1/e of a step). */
inline double timeConstantToCoeff (double ms, double sampleRate) noexcept
{
    if (ms <= 0.0 || sampleRate <= 0.0)
        return 0.0;
    return std::exp (-1.0 / (ms * 0.001 * sampleRate));
}

template <typename T>
inline T clampValue (T v, T lo, T hi) noexcept { return std::min (hi, std::max (lo, v)); }

/** RAII guard enabling flush-to-zero / denormals-are-zero for the current thread (audio callback). */
class ScopedNoDenormals
{
public:
    ScopedNoDenormals() noexcept
    {
#if LUMEN_HAS_SSE
        saved = _mm_getcsr();
        _mm_setcsr (saved | 0x8040u); // FTZ | DAZ
#elif defined(__aarch64__)
        uint64_t fpcr;
        asm volatile ("mrs %0, fpcr" : "=r"(fpcr));
        saved = fpcr;
        fpcr |= (1ull << 24); // FZ
        asm volatile ("msr fpcr, %0" : : "r"(fpcr));
#endif
    }

    ~ScopedNoDenormals() noexcept
    {
#if LUMEN_HAS_SSE
        _mm_setcsr (static_cast<unsigned int> (saved));
#elif defined(__aarch64__)
        uint64_t fpcr = saved;
        asm volatile ("msr fpcr, %0" : : "r"(fpcr));
#endif
    }

    ScopedNoDenormals (const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator= (const ScopedNoDenormals&) = delete;

private:
    uint64_t saved = 0;
};

/** Linear ramp smoother for click-free parameter changes. Lands exactly on the target. */
class LinearSmoother
{
public:
    void reset (double sampleRate, double rampMs) noexcept
    {
        rampSamples = std::max (1, static_cast<int> (std::lround (sampleRate * rampMs * 0.001)));
        countdown = 0;
        current = target;
    }

    void setCurrentAndTarget (double v) noexcept
    {
        current = target = v;
        countdown = 0;
    }

    void setTarget (double v) noexcept
    {
        if (v == target)
            return;
        target = v;
        countdown = rampSamples;
        step = (target - current) / static_cast<double> (rampSamples);
    }

    double next() noexcept
    {
        if (countdown <= 0)
            return current;
        if (--countdown == 0)
            current = target;
        else
            current += step;
        return current;
    }

    bool isSmoothing() const noexcept { return countdown > 0; }
    double getCurrent() const noexcept { return current; }
    double getTarget() const noexcept { return target; }

private:
    double current = 0.0, target = 0.0, step = 0.0;
    int rampSamples = 1, countdown = 0;
};

/** Fixed-capacity integer delay line. Storage is supplied by the owner (no allocation here). */
template <int MaxSamples>
class DelayLine
{
public:
    void clear() noexcept
    {
        std::fill (std::begin (buffer), std::end (buffer), 0.0);
        writePos = 0;
    }

    void setDelay (int samples) noexcept { delay = clampValue (samples, 0, MaxSamples - 1); }
    int getDelay() const noexcept { return delay; }

    double process (double x) noexcept
    {
        if (delay == 0)
            return x;
        buffer[static_cast<size_t> (writePos)] = x;
        int readPos = writePos - delay;
        if (readPos < 0)
            readPos += MaxSamples;
        writePos = (writePos + 1 == MaxSamples) ? 0 : writePos + 1;
        return buffer[static_cast<size_t> (readPos)];
    }

private:
    double buffer[MaxSamples] {};
    int writePos = 0, delay = 0;
};

/** Sliding-window maximum (monotonic deque in a fixed ring). Used to run gain reduction ahead
    of the delayed audio so lookahead catches transients before they arrive. */
template <int MaxWindow>
class SlidingMax
{
public:
    void reset() noexcept
    {
        head = tail = 0;
        index = 0;
    }

    void setWindow (int w) noexcept { window = clampValue (w, 1, MaxWindow); }

    double process (double x) noexcept
    {
        if (window <= 1)
            return x;
        // drop smaller values from the back
        while (tail != head && values[static_cast<size_t> (prev (tail))] <= x)
            tail = prev (tail);
        values[static_cast<size_t> (tail)] = x;
        stamps[static_cast<size_t> (tail)] = index;
        tail = nextIdx (tail);
        // drop expired values from the front
        while (stamps[static_cast<size_t> (head)] <= index - window)
            head = nextIdx (head);
        ++index;
        return values[static_cast<size_t> (head)];
    }

private:
    static constexpr int kSize = MaxWindow + 2;
    static int nextIdx (int i) noexcept { return i + 1 == kSize ? 0 : i + 1; }
    static int prev (int i) noexcept { return i == 0 ? kSize - 1 : i - 1; }

    double values[kSize] {};
    int64_t stamps[kSize] {};
    int head = 0, tail = 0, window = 1;
    int64_t index = 0;
};

} // namespace lumen
