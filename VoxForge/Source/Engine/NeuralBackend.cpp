#include "NeuralBackend.h"

#if VOXFORGE_WITH_ONNX
 #define ORT_API_MANUAL_INIT 1          // we initialise the API after pre-loading the DLL ourselves
 #include <onnxruntime_cxx_api.h>
 #if JUCE_WINDOWS
  #ifndef NOMINMAX
   #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
   #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <dxgi.h>
 #endif
 #include <array>
#endif

namespace vf
{

#if VOXFORGE_WITH_ONNX

namespace
{
    /** Windows ships an old onnxruntime.dll in System32 and hosts do not search the
        plug-in folder. Load our copy (next to the .vst3 binary) by full path before
        the delay-loaded import is resolved, then initialise the C++ API. */
    bool ensureOrtLoaded (juce::String& error)
    {
        static bool tried = false, ok = false;
        static juce::String err;
        if (tried) { error = err; return ok; }
        tried = true;

       #if JUCE_WINDOWS
        const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        // DirectML build: onnxruntime.dll needs a recent DirectML.dll; System32 has an older one
        const auto dml = self.getSiblingFile ("DirectML.dll");
        if (dml.existsAsFile())
            LoadLibraryExW (dml.getFullPathName().toWideCharPointer(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        const auto dll = self.getSiblingFile ("onnxruntime.dll");
        if (dll.existsAsFile())
        {
            if (LoadLibraryExW (dll.getFullPathName().toWideCharPointer(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr)
            {
                err = "Could not load " + dll.getFullPathName();
                error = err;
                return false;
            }
        }
       #endif

        try
        {
            const OrtApiBase* base = OrtGetApiBase();
            const OrtApi* api = base != nullptr ? base->GetApi (ORT_API_VERSION) : nullptr;
            if (api == nullptr)
            {
                err = "onnxruntime.dll is too old (needs API version " + juce::String (ORT_API_VERSION) + ").";
                error = err;
                return false;
            }
            Ort::InitApi (api);
            ok = true;
        }
        catch (...)
        {
            err = "ONNX Runtime could not be initialised.";
        }
        error = err;
        return ok;
    }

#if JUCE_WINDOWS
    /** GPU for DirectML: NVIDIA first (RTX), then the hardware adapter with the most VRAM.
        Index = position in IDXGIFactory1::EnumAdapters1 order, which is what the DML provider uses. */
    struct GpuChoice { int index = -1; juce::String name; juce::int64 vramMB = 0; };

    GpuChoice chooseGpu (bool allowSoftware)
    {
        GpuChoice best;
        HMODULE dxgi = LoadLibraryW (L"dxgi.dll");
        if (dxgi == nullptr) return best;
        using CreateFn = HRESULT (WINAPI*) (REFIID, void**);
        auto create = reinterpret_cast<CreateFn> (GetProcAddress (dxgi, "CreateDXGIFactory1"));
        IDXGIFactory1* factory = nullptr;
        if (create == nullptr || FAILED (create (__uuidof (IDXGIFactory1), reinterpret_cast<void**> (&factory))))
            return best;
        int bestScore = -1;
        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1* a = nullptr;
            if (factory->EnumAdapters1 (i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d {};
            if (SUCCEEDED (a->GetDesc1 (&d)))
            {
                const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || d.VendorId == 0x1414;
                const auto mb = (juce::int64) (d.DedicatedVideoMemory / (1024 * 1024));
                if (! software || allowSoftware)
                {
                    // NVIDIA > other discrete (>= 1 GB VRAM) > integrated > software
                    const int score = (d.VendorId == 0x10DE ? 3000000 : (mb >= 1024 ? 2000000 : (software ? 0 : 1000000)))
                                      + (int) std::min<juce::int64> (mb, 999999);
                    if (score > bestScore)
                    {
                        bestScore = score;
                        best = { (int) i, juce::String (d.Description).trim(), mb };
                    }
                }
            }
            a->Release();
        }
        factory->Release();
        return best;
    }
#endif
}

struct NeuralBackend::Impl
{
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> content, voice;
    std::unique_ptr<Ort::SessionOptions> options;      // CPU; created only after the ORT API is initialised
    std::unique_ptr<Ort::SessionOptions> gpuOptions;   // DirectML on the chosen GPU (Windows, DirectML build)
    std::string contentIn, contentOut, voiceUnits, voiceF0, voiceOut;
    bool preferGpu = true, allowSoftware = false, contentOnGpu = false, voiceOnGpu = false;
    juce::String gpuName, gpuNote;
    juce::File contentFile;

    bool init (juce::String& error)
    {
        if (env != nullptr) return true;
        if (! ensureOrtLoaded (error)) return false;
        env = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "VoxForge");
        options = std::make_unique<Ort::SessionOptions>();
        options->SetIntraOpNumThreads (juce::jmax (1, juce::SystemStats::getNumPhysicalCpus() - 1));
        options->SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (preferGpu) createGpuOptions();
        return true;
    }

    /** VOXFORGE_GPU=0 disables the GPU, =force also accepts a software adapter (CI). */
    void createGpuOptions()
    {
        gpuOptions.reset();
        gpuName.clear();
        const auto gpuEnv = juce::SystemStats::getEnvironmentVariable ("VOXFORGE_GPU", {}).toLowerCase();
        if (gpuEnv == "0" || gpuEnv == "off") { gpuNote = "GPU disabled (VOXFORGE_GPU=0)"; return; }
       #if JUCE_WINDOWS
        using AppendDml = OrtStatus* (ORT_API_CALL*) (OrtSessionOptions*, int);
        HMODULE ort = GetModuleHandleW (L"onnxruntime.dll");
        auto append = ort != nullptr ? reinterpret_cast<AppendDml> (GetProcAddress (ort, "OrtSessionOptionsAppendExecutionProvider_DML")) : nullptr;
        if (append == nullptr) { gpuNote = "this build of ONNX Runtime has no DirectML (CPU only)"; return; }
        const auto gpu = chooseGpu (allowSoftware || gpuEnv == "force");
        if (gpu.index < 0) { gpuNote = "no DirectX 12 GPU found"; return; }
        try
        {
            auto o = std::make_unique<Ort::SessionOptions>();
            o->SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
            o->DisableMemPattern();                              // required by the DirectML provider
            o->SetExecutionMode (ExecutionMode::ORT_SEQUENTIAL);
            if (OrtStatus* st = append (*o, gpu.index))
            {
                gpuNote = juce::String ("DirectML unavailable: ") + Ort::GetApi().GetErrorMessage (st);
                Ort::GetApi().ReleaseStatus (st);
                return;
            }
            gpuOptions = std::move (o);
            gpuName = gpu.name + (gpu.vramMB > 0 ? " (" + juce::String (gpu.vramMB / 1024.0, 1) + " GB)" : juce::String());
            gpuNote.clear();
        }
        catch (const Ort::Exception& e) { gpuNote = juce::String ("DirectML unavailable: ") + e.what(); }
       #else
        gpuNote = "GPU rendering is available in the Windows build (DirectML)";
       #endif
    }

    std::unique_ptr<Ort::Session> openWith (const juce::File& f, Ort::SessionOptions& o)
    {
       #if JUCE_WINDOWS
        return std::make_unique<Ort::Session> (*env, f.getFullPathName().toWideCharPointer(), o);
       #else
        return std::make_unique<Ort::Session> (*env, f.getFullPathName().toRawUTF8(), o);
       #endif
    }

    /** GPU first; any failure there falls back to the CPU. */
    std::unique_ptr<Ort::Session> open (const juce::File& f, juce::String& error, bool& onGpu)
    {
        onGpu = false;
        if (gpuOptions != nullptr)
        {
            try { auto s = openWith (f, *gpuOptions); onGpu = true; return s; }
            catch (const Ort::Exception& e) { gpuNote = juce::String ("GPU session failed, using CPU: ") + e.what(); }
        }
        try
        {
            return openWith (f, *options);
        }
        catch (const Ort::Exception& e)
        {
            error = juce::String ("ONNX error: ") + e.what();
            return nullptr;
        }
    }

    static std::string name (Ort::Session& s, bool input, size_t i)
    {
        Ort::AllocatorWithDefaultOptions alloc;
        auto p = input ? s.GetInputNameAllocated (i, alloc) : s.GetOutputNameAllocated (i, alloc);
        return std::string (p.get());
    }
};

NeuralBackend::NeuralBackend() : impl (std::make_unique<Impl>()) {}
NeuralBackend::~NeuralBackend() = default;
bool NeuralBackend::isCompiledIn() noexcept { return true; }

bool NeuralBackend::loadContentEncoder (const juce::File& f, juce::String& error)
{
    const juce::ScopedLock sl (lock);
    if (! f.existsAsFile()) { error = "Content encoder not found: " + f.getFullPathName(); lastError = error; return false; }
    if (! impl->init (error)) { lastError = error; return false; }
    bool gpu = false;
    auto s = impl->open (f, error, gpu);
    if (s == nullptr) { lastError = error; return false; }
    impl->contentOnGpu = gpu;
    impl->contentFile = f;
    impl->contentIn = Impl::name (*s, true, 0);
    impl->contentOut = Impl::name (*s, false, 0);
    impl->content = std::move (s);
    return true;
}

bool NeuralBackend::loadVoice (const juce::File& f, juce::String& error, double sampleRate, int hop)
{
    const juce::ScopedLock sl (lock);
    voiceRate = sampleRate > 0 ? sampleRate : kOutputRate;
    voiceHop = hop > 0 ? hop : kHop;
    if (f == voiceFile && impl->voice != nullptr) return true;
    if (! f.existsAsFile()) { error = "Voice model not found: " + f.getFullPathName(); lastError = error; return false; }
    if (! impl->init (error)) { lastError = error; return false; }
    bool gpu = false;
    auto s = impl->open (f, error, gpu);
    if (s == nullptr) { lastError = error; return false; }
    impl->voiceOnGpu = gpu;
    if (s->GetInputCount() < 2) { error = "voice.onnx must have inputs (units, f0)."; lastError = error; return false; }
    impl->voiceUnits = Impl::name (*s, true, 0);
    impl->voiceF0 = Impl::name (*s, true, 1);
    impl->voiceOut = Impl::name (*s, false, 0);
    impl->voice = std::move (s);
    voiceFile = f;
    return true;
}

bool NeuralBackend::hasContentEncoder() const noexcept { return impl->content != nullptr; }
bool NeuralBackend::hasVoice() const noexcept { return impl->voice != nullptr; }

juce::String NeuralBackend::status() const
{
    if (lastError.isNotEmpty() && impl->env == nullptr) return lastError;
    if (! hasContentEncoder()) return "content encoder missing";
    return (hasVoice() ? "ready" : "no neural voice loaded") + juce::String (" - ") + device();
}

juce::String NeuralBackend::device() const
{
    if (impl->env == nullptr) return impl->preferGpu ? "GPU (when available)" : "CPU";
    const bool gpu = impl->contentOnGpu || impl->voiceOnGpu;
    if (gpu) return "GPU: " + impl->gpuName + " (DirectML)";
    return impl->preferGpu && impl->gpuNote.isNotEmpty() ? "CPU (" + impl->gpuNote + ")" : juce::String ("CPU");
}

bool NeuralBackend::usingGpu() const noexcept { return impl->contentOnGpu || impl->voiceOnGpu; }

void NeuralBackend::setPreferGpu (bool shouldUseGpu, bool allowSoftwareAdapter)
{
    const juce::ScopedLock sl (lock);
    if (impl->preferGpu == shouldUseGpu && impl->allowSoftware == allowSoftwareAdapter) return;
    impl->preferGpu = shouldUseGpu;
    impl->allowSoftware = allowSoftwareAdapter;
    if (impl->env == nullptr) return;
    // re-open the models on the new device on next use
    impl->content.reset(); impl->voice.reset();
    impl->contentOnGpu = impl->voiceOnGpu = false;
    voiceFile = juce::File();
    if (shouldUseGpu) impl->createGpuOptions();
    else { impl->gpuOptions.reset(); impl->gpuNote = "GPU off in settings"; }
}

bool NeuralBackend::fallBackToCpu()
{
    // a DirectML inference error (driver reset, out of VRAM ...): re-open both models on the CPU
    if (! usingGpu()) return false;
    impl->gpuNote = "GPU error (" + lastError.substring (0, 120) + "), switched to CPU";
    impl->gpuOptions.reset();
    juce::String err;
    bool gpu = false;
    if (impl->contentOnGpu && impl->contentFile.existsAsFile())
        if (auto s = impl->open (impl->contentFile, err, gpu)) { impl->content = std::move (s); impl->contentOnGpu = false; }
    if (impl->voiceOnGpu && voiceFile.existsAsFile())
        if (auto s = impl->open (voiceFile, err, gpu)) { impl->voice = std::move (s); impl->voiceOnGpu = false; }
    return ! usingGpu();
}

bool NeuralBackend::extractUnits (const float* audio, int numSamples, std::vector<float>& units, int& frames, int& dim,
                                  std::atomic<bool>* cancel)
{
    const juce::ScopedLock sl (lock);
    if (extractUnitsLocked (audio, numSamples, units, frames, dim, cancel)) return true;
    if (cancel != nullptr && cancel->load()) return false;
    return fallBackToCpu() && extractUnitsLocked (audio, numSamples, units, frames, dim, cancel);
}

bool NeuralBackend::extractUnitsLocked (const float* audio, int numSamples, std::vector<float>& units, int& frames, int& dim,
                                        std::atomic<bool>* cancel)
{
    if (impl->content == nullptr) return false;
    units.clear(); frames = 0; dim = 0;

    // 20 s chunks with 1 s context on each side (320 samples per frame)
    constexpr int frameLen = 320;
    constexpr int chunkFrames = 1000, ctxFrames = 50;
    const int totalFrames = numSamples / frameLen;
    auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);

    try
    {
        for (int f0 = 0; f0 < totalFrames; f0 += chunkFrames)
        {
            if (cancel != nullptr && cancel->load()) return false;
            const int a = juce::jmax (0, f0 - ctxFrames);
            const int b = juce::jmin (totalFrames, f0 + chunkFrames + ctxFrames);
            std::vector<float> in ((size_t) (b - a) * frameLen);
            std::memcpy (in.data(), audio + (size_t) a * frameLen, in.size() * sizeof (float));
            std::array<int64_t, 2> shape { 1, (int64_t) in.size() };
            auto tensor = Ort::Value::CreateTensor<float> (mem, in.data(), in.size(), shape.data(), shape.size());
            const char* inNames[] { impl->contentIn.c_str() };
            const char* outNames[] { impl->contentOut.c_str() };
            auto out = impl->content->Run (Ort::RunOptions { nullptr }, inNames, &tensor, 1, outNames, 1);
            auto info = out[0].GetTensorTypeAndShapeInfo();
            auto oshape = info.GetShape();             // [1, T, C]
            const int T = (int) oshape[1];
            const int C = (int) oshape[2];
            dim = C;
            const float* data = out[0].GetTensorData<float>();
            const int keepFrom = f0 - a;
            const int keepTo = juce::jmin (T, keepFrom + juce::jmin (chunkFrames, totalFrames - f0));
            for (int t = keepFrom; t < keepTo; ++t)
                units.insert (units.end(), data + (size_t) t * C, data + (size_t) (t + 1) * C);
        }
    }
    catch (const Ort::Exception& e)
    {
        lastError = e.what();
        return false;
    }
    frames = dim > 0 ? (int) (units.size() / (size_t) dim) : 0;
    return frames > 0;
}

bool NeuralBackend::synthesise (const std::vector<float>& units, int frames, int dim, const std::vector<float>& f0,
                                std::vector<float>& audioOut, std::atomic<bool>* cancel,
                                std::function<void (float)> progress)
{
    const juce::ScopedLock sl (lock);
    if (synthesiseLocked (units, frames, dim, f0, audioOut, cancel, progress)) return true;
    if (cancel != nullptr && cancel->load()) return false;
    return fallBackToCpu() && synthesiseLocked (units, frames, dim, f0, audioOut, cancel, progress);
}

bool NeuralBackend::synthesiseLocked (const std::vector<float>& units, int frames, int dim, const std::vector<float>& f0,
                                      std::vector<float>& audioOut, std::atomic<bool>* cancel,
                                      std::function<void (float)> progress)
{
    if (impl->voice == nullptr || frames <= 0) return false;
    audioOut.assign ((size_t) frames * voiceHop, 0.0f);

    constexpr int chunk = 400, ctx = 24, fade = 8;      // 8 s chunks, 0.48 s context, 160 ms crossfade
    auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);

    try
    {
        for (int s = 0; s < frames; s += chunk)
        {
            if (cancel != nullptr && cancel->load()) return false;
            if (progress) progress ((float) s / (float) frames);

            const int a = juce::jmax (0, s - ctx);
            const int b = juce::jmin (frames, s + chunk + ctx);
            const int T = b - a;
            std::vector<float> u ((size_t) T * dim), p ((size_t) T);
            std::memcpy (u.data(), units.data() + (size_t) a * dim, u.size() * sizeof (float));
            for (int t = 0; t < T; ++t) p[(size_t) t] = (a + t) < (int) f0.size() ? f0[(size_t) (a + t)] : 0.0f;

            std::array<int64_t, 3> us { 1, T, dim };
            std::array<int64_t, 2> ps { 1, T };
            Ort::Value inputs[] = {
                Ort::Value::CreateTensor<float> (mem, u.data(), u.size(), us.data(), us.size()),
                Ort::Value::CreateTensor<float> (mem, p.data(), p.size(), ps.data(), ps.size())
            };
            const char* inNames[] { impl->voiceUnits.c_str(), impl->voiceF0.c_str() };
            const char* outNames[] { impl->voiceOut.c_str() };
            auto out = impl->voice->Run (Ort::RunOptions { nullptr }, inNames, inputs, 2, outNames, 1);
            const float* y = out[0].GetTensorData<float>();
            const size_t ylen = out[0].GetTensorTypeAndShapeInfo().GetElementCount();

            // write [s - fade, s + chunk) with a linear crossfade at the start
            const int w0 = juce::jmax (0, s - (s > 0 ? fade : 0));
            const int w1 = juce::jmin (frames, s + chunk);
            for (int smp = w0 * voiceHop; smp < w1 * voiceHop; ++smp)
            {
                const size_t local = (size_t) (smp - a * voiceHop);
                if (local >= ylen) break;
                float g = 1.0f;
                if (s > 0 && smp < s * voiceHop)
                    g = (float) (smp - w0 * voiceHop) / (float) (fade * voiceHop);
                audioOut[(size_t) smp] = audioOut[(size_t) smp] * (1.0f - g) + y[local] * g;
            }
        }
    }
    catch (const Ort::Exception& e)
    {
        lastError = e.what();
        return false;
    }
    if (progress) progress (1.0f);
    return true;
}

#else   // ---------------------------------------------------------------- no ONNX

struct NeuralBackend::Impl {};
NeuralBackend::NeuralBackend() : impl (std::make_unique<Impl>()) {}
NeuralBackend::~NeuralBackend() = default;
bool NeuralBackend::isCompiledIn() noexcept { return false; }
juce::String NeuralBackend::status() const { return "neural engine not compiled in (DSP engine active)"; }
juce::String NeuralBackend::device() const { return "none"; }
bool NeuralBackend::usingGpu() const noexcept { return false; }
void NeuralBackend::setPreferGpu (bool, bool) {}
bool NeuralBackend::fallBackToCpu() { return false; }
bool NeuralBackend::extractUnitsLocked (const float*, int, std::vector<float>&, int&, int&, std::atomic<bool>*) { return false; }
bool NeuralBackend::synthesiseLocked (const std::vector<float>&, int, int, const std::vector<float>&, std::vector<float>&,
                                      std::atomic<bool>*, std::function<void (float)>) { return false; }
bool NeuralBackend::loadContentEncoder (const juce::File&, juce::String& e) { e = status(); return false; }
bool NeuralBackend::loadVoice (const juce::File&, juce::String& e, double, int) { e = status(); return false; }
bool NeuralBackend::hasContentEncoder() const noexcept { return false; }
bool NeuralBackend::hasVoice() const noexcept { return false; }
bool NeuralBackend::extractUnits (const float*, int, std::vector<float>&, int&, int&, std::atomic<bool>*) { return false; }
bool NeuralBackend::synthesise (const std::vector<float>&, int, int, const std::vector<float>&, std::vector<float>&,
                                std::atomic<bool>*, std::function<void (float)>) { return false; }

#endif

} // namespace vf
