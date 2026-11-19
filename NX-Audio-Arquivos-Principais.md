# NX Audio Pro EQ³ — arquivos principais

Arquivos da versão publicada em `0c4b60b7c263532b7bbccee523874c35de5c988b`.

## CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.22)

project(ProQ3Clone VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Caminho para o JUCE 8.0.4 (pode ser passado via -DJUCE_DIR="C:/JUCE" ou colocar a pasta JUCE ao lado/dentro do projeto)
if(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" JUCE_build EXCLUDE_FROM_ALL)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/JUCE" JUCE_build EXCLUDE_FROM_ALL)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" JUCE_build EXCLUDE_FROM_ALL)
elseif(EXISTS "C:/JUCE/CMakeLists.txt")
    add_subdirectory("C:/JUCE" JUCE_build EXCLUDE_FROM_ALL)
else()
    find_package(JUCE CONFIG REQUIRED)
endif()

juce_add_plugin(ProQ3Clone
    COMPANY_NAME "NX Audio"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE ScAu
    PLUGIN_CODE Pq3C
    FORMATS VST3 Standalone
    PLUGIN_NAME "NX Audio"
    PRODUCT_NAME "NX Audio"
)

# Regenerate the JUCE Windows resource when the product metadata changes.
if(TARGET ProQ3Clone_rc_lib)
    get_target_property(nx_juce_generated_dir ProQ3Clone JUCE_GENERATED_SOURCES_DIRECTORY)
    get_target_property(nx_juce_info_file ProQ3Clone JUCE_INFO_FILE)
    add_custom_command(
        OUTPUT "${nx_juce_generated_dir}/ProQ3Clone_resources.rc"
        APPEND DEPENDS "${nx_juce_info_file}"
    )
endif()

target_sources(ProQ3Clone
    PRIVATE
        Source/PluginProcessor.h
        Source/PluginProcessor.cpp
        Source/PluginEditor.h
        Source/PluginEditor.cpp
        Source/SpectrumAnalyzerPanel.cpp
        Source/SpectrumMatchPanel.cpp
)

target_compile_definitions(ProQ3Clone
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        _USE_MATH_DEFINES=1
)

if(MSVC)
    target_compile_options(ProQ3Clone PRIVATE /W4 /MP /permissive- /Zc:__cplusplus /utf-8)
endif()

target_link_libraries(ProQ3Clone
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
```

## Source/PluginProcessor.h

```cpp
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <complex>
#include <vector>

class ProQ3AudioProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int kMaxBands = 24;
    static constexpr int kMaxCascadedSections = 8; // Até 96 dB/oct + Brickwall (8 biquads em cascata)
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr int kFftBins = kFftSize / 2 + 1;
    static constexpr float kMeterFloorDb = -100.0f;

    // 9 Tipos de Filtro conforme manual oficial do FabFilter Pro-Q 3 (p. 11: Band controls -> shape)
    enum FilterShape : int
    {
        kShapeBell      = 0, // 1. Bell
        kShapeLowShelf  = 1, // 2. Low Shelf
        kShapeLowCut    = 2, // 3. Low Cut
        kShapeHighShelf = 3, // 4. High Shelf
        kShapeHighCut   = 4, // 5. High Cut
        kShapeNotch     = 5, // 6. Notch
        kShapeBandPass  = 6, // 7. Band Pass
        kShapeTiltShelf = 7, // 8. Tilt Shelf
        kShapeFlatTilt  = 8  // 9. Flat Tilt
    };

    // 10 Inclinações conforme manual oficial (6 a 96 dB/oct + Brickwall para Low/High Cut)
    enum FilterSlope : int
    {
        kSlope6dB       = 0,
        kSlope12dB      = 1,
        kSlope18dB      = 2,
        kSlope24dB      = 3,
        kSlope30dB      = 4,
        kSlope36dB      = 5,
        kSlope48dB      = 6,
        kSlope72dB      = 7,
        kSlope96dB      = 8,
        kSlopeBrickwall = 9
    };

    struct BiquadCoeffs
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a1 = 0.0, a2 = 0.0;
    };

    struct BiquadState
    {
        double z1L = 0.0, z2L = 0.0;
        double z1R = 0.0, z2R = 0.0;

        void reset() noexcept
        {
            z1L = z2L = z1R = z2R = 0.0;
        }
    };

    struct BandParameters
    {
        bool used       = false;
        bool enabled    = true;
        int shape       = kShapeBell;  // 0..8 (9 tipos do manual)
        double freqHz   = 1000.0;      // 10.0 a 30000.0 Hz
        double gainDb   = 0.0;         // -30.0 a +30.0 dB
        double q        = 1.0;         // 0.025 a 40.0
        int slopeIndex  = kSlope12dB;  // 0..9 (6 dB/oct a 96 dB/oct + Brickwall)
        int stereoMode  = 0;           // 0: Stereo, 1: Left, 2: Right, 3: Mid, 4: Side
        bool gainQInter = false;       // Gain-Q interaction

        // Parâmetros de EQ Dinâmico e Sidechain (Manual Pro-Q 3 p. 13-14)
        double dynRangeDb    = 0.0;    // -30.0 a +30.0 dB (0.0 = estático)
        bool   dynEnabled    = true;   // Bypass dynamics button (true = dinâmica ativa, false = suspensa)
        bool   dynAutoThresh = true;   // Auto Threshold vs Manual Threshold slider
        double dynThresholdDb = -18.0; // -60.0 a 0.0 dB (preservado mesmo quando dynAutoThresh == true)
        bool   dynExtSidechain = false;// false = Main Input, true = External Side Chain
    };

    struct BandDynamicState
    {
        BiquadCoeffs detectorBpCoeffs;
        BiquadState  detectorBpState;
        double fastEnvLin      = 0.0;   // Envelope rápido do sinal detector
        double slowAutoRefLin  = 0.01;  // Referência adaptativa lenta para o modo Auto Threshold
        double smoothedGainModDb = 0.0; // Ganho dinâmico atual (redução < 0 ou expansão > 0) em dB
        double detectorLevelDb = -100.0;

        void reset() noexcept
        {
            detectorBpState.reset();
            fastEnvLin = 0.0;
            slowAutoRefLin = 0.01;
            smoothedGainModDb = 0.0;
            detectorLevelDb = -100.0;
        }
    };

    struct BandDspChain
    {
        int activeSections = 1;
        std::array<BiquadCoeffs, kMaxCascadedSections> coeffs;
        std::array<BiquadState, kMaxCascadedSections> states;
        BandDynamicState dynState;
    };

    ProQ3AudioProcessor();
    ~ProQ3AudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Helpers de IDs do APVTS para as 24 bandas (preservando IDs originais)
    static juce::String getBandParamID (int bandIndex, const juce::String& suffix);
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Regras de formato do manual Pro-Q 3
    static bool doesShapeSupportGain (int shape) noexcept;
    static bool doesShapeSupportDynamics (int shape) noexcept;
    static bool doesShapeSupportQ (int shape, int slopeIndex) noexcept;

    // Leitura de parâmetros de uma banda (0..23)
    BandParameters getBandParameters (int bandIndex) const noexcept;

    // Leituras em tempo real da dinâmica para o anel do GAIN, medidor de Threshold e curva do gráfico
    float getBandDynamicGainDb (int bandIndex) const noexcept
    {
        if (bandIndex < 0 || bandIndex >= kMaxBands) return 0.0f;
        return currentDynamicGainDb[static_cast<size_t> (bandIndex)].load (std::memory_order_relaxed);
    }

    float getBandDetectorLevelDb (int bandIndex) const noexcept
    {
        if (bandIndex < 0 || bandIndex >= kMaxBands) return -100.0f;
        return currentDetectorLevelDb[static_cast<size_t> (bandIndex)].load (std::memory_order_relaxed);
    }

    struct BandResponse
    {
        std::array<BiquadCoeffs, kMaxCascadedSections> coefficients {};
        int sections = 0;
        double sampleRate = 44100.0;

        double magnitudeDbAtFrequency (double freqHz) const noexcept;
    };

    BandResponse makeBandResponse (BandParameters parameters, double dynamicGainDb = 0.0) const noexcept;

    // Cálculo analítico exato da magnitude em dB da função de transferência H(e^(jw))
    // includeDynamicGain = false -> retorna a curva estática; true -> inclui o ganho dinâmico instantâneo aplicado pelo DSP
    double getBandMagnitudeDbAtFrequency (int bandIndex, double freqHz, bool includeDynamicGain = false) const noexcept;
    double getTotalMagnitudeDbAtFrequency (double freqHz, bool includeDynamicGain = false) const noexcept;
    double getChannelMagnitudeDbAtFrequency (double freqHz, int targetChannel, bool includeDynamicGain = false) const noexcept;

    static int getFactoryPresetCount() noexcept;
    static juce::String getFactoryPresetName (int index);
    static juce::String getFactoryPresetCategory (int index);
    int getFactoryPresetIndex() const;
    void loadFactoryPreset (int index);

    // Ativa uma nova banda livre na frequência e ganho clicados
    int activateFirstFreeBand (float freqHz, float gainDb, float qValue = 1.0f, int shape = kShapeBell, float dynRangeDb = 0.0f, bool newUndoTransaction = true);
    void deactivateBand (int bandIndex);

    // Suporte A/B Switch e Copy
    void toggleABState();
    void copyActiveToInactiveState();

    enum class MidiOutputTarget { outputLevel, gainScale };
    void armMidiLearn (MidiOutputTarget target) noexcept;
    void clearMidiLearn() noexcept;
    bool isMidiLearnArmed() const noexcept;
    bool hasMidiOutputMapping() const noexcept;
    bool isStateBActive() const noexcept { return currentIsB; }

    // Analisador de espectro Pre/Post/Sidechain e Spectrum Grab
    static float spectrumTiltDb (double frequency) noexcept;
    bool pullNextPreFftBlock (std::vector<float>& destMagnitudesDb);
    bool pullNextPostFftBlock (std::vector<float>& destMagnitudesDb);
    bool pullNextSidechainFftBlock (std::vector<float>& destMagnitudesDb);

    struct SpectrumPeer
    {
        int id;
        juce::String name;
    };
    juce::String getSpectrumInstanceName() const;
    bool hasSpectrumTrackName() const;
    void setSpectrumInstanceAlias (juce::String);
    std::vector<SpectrumPeer> getSpectrumPeers() const;
    bool readPostSpectrum (juce::uint64& generation, std::vector<float>& magnitudes);
    bool readPeerSpectrum (int peerId, std::vector<float>& magnitudes, double& rate);
    void updateTrackProperties (const TrackProperties&) override;

    float getOutputPeakDbL() const noexcept { return outputPeakDbL.load (std::memory_order_relaxed); }
    float getOutputPeakDbR() const noexcept { return outputPeakDbR.load (std::memory_order_relaxed); }
    float getOutputHeldPeakDb (size_t channel) const noexcept
    {
        return outputHeldPeakDb[channel].load (std::memory_order_relaxed);
    }

    double getCurrentSampleRate() const noexcept { return currentSampleRate.load (std::memory_order_relaxed); }

    int getSelectedBandIndex() const noexcept { return selectedBandIndex.load (std::memory_order_relaxed); }
    void setSelectedBandIndex (int idx) noexcept { selectedBandIndex.store (juce::jlimit (-1, kMaxBands - 1, idx), std::memory_order_relaxed); }

    void setSoloBandIndex (int idx, float soloFreqHz = 1000.0f, float soloQ = 2.0f) noexcept;
    int getSoloBandIndex() const noexcept { return soloBandIndex.load (std::memory_order_relaxed); }

    // Cálculo estático dos coeficientes para qualquer um dos 9 tipos de filtro e inclinações
    static void computeFilterCascadeCoeffs (const BandParameters& params,
                                            double sampleRate,
                                            double gainScale,
                                            double dynamicGainOffsetDb,
                                            std::array<BiquadCoeffs, kMaxCascadedSections>& outCoeffs,
                                            int& outNumSections) noexcept;

    static void computeBandDetectorCoeffs (double freqHz, double q, double sampleRate, BiquadCoeffs& outCoeffs) noexcept;

    // Cálculo de um passo do detector dinâmico por amostra (com Auto e Manual Threshold + Soft Knee)
    static double processDynamicEnvelopeSample (const BandParameters& bp,
                                                BandDynamicState& dynState,
                                                double detectorInputSample,
                                                double sampleRate) noexcept;

    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;

private:
    void processMidiLearn (const juce::MidiBuffer& midiMessages);

    std::atomic<int> armedMidiTarget { -1 };
    std::array<std::atomic<int>, 2> learnedControllers { { -1, -1 } };
    std::array<juce::RangedAudioParameter*, 2> midiOutputParameters {};

    struct BandParameterValues
    {
        const std::atomic<float>* used = nullptr;
        const std::atomic<float>* enabled = nullptr;
        const std::atomic<float>* shape = nullptr;
        const std::atomic<float>* freqHz = nullptr;
        const std::atomic<float>* gainDb = nullptr;
        const std::atomic<float>* q = nullptr;
        const std::atomic<float>* slopeIndex = nullptr;
        const std::atomic<float>* stereoMode = nullptr;
        const std::atomic<float>* gainQInter = nullptr;
        const std::atomic<float>* dynRangeDb = nullptr;
        const std::atomic<float>* dynEnabled = nullptr;
        const std::atomic<float>* dynAutoThresh = nullptr;
        const std::atomic<float>* dynThresholdDb = nullptr;
        const std::atomic<float>* dynExtSidechain = nullptr;
    };

    std::array<BandParameterValues, kMaxBands> bandParameterValues;

    void updateAllFilterCoefficients() noexcept;

    struct SpectrumCapture
    {
        using Channel = std::array<float, 8192>;
        using StereoBuffer = std::array<Channel, 2>;
        std::unique_ptr<StereoBuffer> history = std::make_unique<StereoBuffer>();
        std::unique_ptr<StereoBuffer> frame = std::make_unique<StereoBuffer>();
        size_t writeIndex = 0;
        size_t activeSize = kFftSize;
        size_t samplesUntilFrame = kFftSize;
        size_t publishedPlan = 2;

        void setResolution (size_t fftSize) noexcept
        {
            if (activeSize != fftSize)
            {
                activeSize = fftSize;
                resetHistory();
            }
        }
        enum class FrameState { empty, writing, ready, reading };
        std::atomic<FrameState> frameState { FrameState::empty };

        // The audio thread owns the history. The published frame is read by the editor.
        void push (float left, float right) noexcept
        {
            (*history)[0][writeIndex] = left;
            (*history)[1][writeIndex] = right;
            writeIndex = (writeIndex + 1) & (activeSize - 1);
            if (--samplesUntilFrame != 0)
                return;

            samplesUntilFrame = activeSize / 4;
            auto previous = frameState.load (std::memory_order_relaxed);
            if (previous == FrameState::reading
                || ! frameState.compare_exchange_strong (previous, FrameState::writing, std::memory_order_acquire))
                return;
            for (size_t channel = 0; channel < history->size(); ++channel)
            {
                const auto& samples = (*history)[channel];
                auto dest = std::copy (samples.begin() + static_cast<std::ptrdiff_t> (writeIndex),
                                       samples.begin() + static_cast<std::ptrdiff_t> (activeSize), (*frame)[channel].begin());
                std::copy_n (samples.begin(), writeIndex, dest);
            }
            publishedPlan = activeSize == 1024 ? 0 : activeSize == 2048 ? 1 : activeSize == 4096 ? 2 : 3;
            frameState.store (FrameState::ready, std::memory_order_release);
        }

        void resetHistory() noexcept
        {
            writeIndex = 0;
            samplesUntilFrame = activeSize;
            auto published = FrameState::ready;
            frameState.compare_exchange_strong (published, FrameState::empty, std::memory_order_relaxed);
        }
    };

    bool pullSpectrum (SpectrumCapture&, std::vector<float>&);
    void updateOutputMeter (float leftPeak, float rightPeak, int samples, double sampleRate) noexcept;

    SpectrumCapture preFftFifo;
    SpectrumCapture postFftFifo;
    SpectrumCapture sidechainFftFifo;
    int spectrumPeerId = 0;
    juce::String spectrumTrackName;
    juce::String spectrumInstanceAlias;
    juce::String getSpectrumInstanceNameUnlocked() const;
    std::vector<float> cachedPostSpectrum;
    double lastPostSpectrumMs = 0.0;
    juce::uint64 postSpectrumGeneration = 0;
    void refreshPostSpectrum();
    struct SpectrumTransform
    {
        SpectrumTransform (int order)
            : size (size_t { 1 } << order), fft (order),
              window (size, juce::dsp::WindowingFunction<float>::hann) {}

        size_t size;
        juce::dsp::FFT fft;
        juce::dsp::WindowingFunction<float> window;
    };
    std::array<SpectrumTransform, 4> spectrumTransforms {{ { 10 }, { 11 }, { 12 }, { 13 } }};
    std::array<float, 8192 * 2> fftWorkspace {};

    std::array<BandDspChain, kMaxBands> dspBands;
    std::array<std::atomic<float>, kMaxBands> currentDynamicGainDb {};
    std::array<std::atomic<float>, kMaxBands> currentDetectorLevelDb {};

    BiquadCoeffs soloBandPassCoeffs;
    BiquadState  soloBandPassState;

    std::atomic<double> currentSampleRate { 44100.0 };
    std::atomic<float> outputPeakDbL { kMeterFloorDb };
    std::atomic<float> outputPeakDbR { kMeterFloorDb };
    std::array<std::atomic<float>, 2> outputHeldPeakDb { { kMeterFloorDb, kMeterFloorDb } };
    std::array<int, 2> meterHoldSamples {};
    std::atomic<int> selectedBandIndex { -1 };
    std::atomic<int> soloBandIndex { -1 };
    std::atomic<float> soloFrequencyHz { 1000.0f };
    std::atomic<float> soloBandwidthQ { 2.0f };

    juce::ValueTree stateA;
    juce::ValueTree stateB;
    bool currentIsB = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProQ3AudioProcessor)
};
```

## Source/PluginProcessor.cpp

```cpp
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "FactoryPresets.h"
#include <cmath>

namespace
{
    struct SpectrumPeerRegistry
    {
        juce::CriticalSection mutex;
        std::vector<ProQ3AudioProcessor*> instances;
        int nextId = 1;
    };

    SpectrumPeerRegistry& getSpectrumPeerRegistry()
    {
        static SpectrumPeerRegistry registry;
        return registry;
    }

    const juce::Identifier factoryPresetProperty { "factoryPreset" };

    constexpr double kPi = 3.1415926535897932384626433832795;

    constexpr std::array<int, 10> filterOrders { 1, 2, 3, 4, 5, 6, 8, 12, 16, 16 };

    int getFilterOrder (int slopeIndex) noexcept
    {
        return filterOrders[static_cast<size_t> (juce::jlimit (0, 9, slopeIndex))];
    }

    void designCutFilter (int order, double frequency, double sampleRate,
                          double resonance, bool highPass,
                          std::array<ProQ3AudioProcessor::BiquadCoeffs, ProQ3AudioProcessor::kMaxCascadedSections>& coeffs) noexcept
    {
        const auto w = 2.0 * kPi * frequency / sampleRate;
        const auto cosine = std::cos (w);
        const auto sine = std::sin (w);
        const auto numerator = 0.5 * (highPass ? 1.0 + cosine : 1.0 - cosine);
        const auto sign = highPass ? -1.0 : 1.0;
        const auto firstBiquad = order % 2;
        if (firstBiquad != 0)
        {
            const auto tangent = std::tan (0.5 * w);
            const auto b0 = (highPass ? 1.0 : tangent) / (1.0 + tangent);
            coeffs.front() = { b0, sign * b0, 0.0, (tangent - 1.0) / (tangent + 1.0), 0.0 };
        }

        const auto pairs = order / 2;
        for (int pair = 0; pair < pairs; ++pair)
        {
            // Use the Butterworth pole pairs, ordered from lowest to highest Q.
            const auto angle = kPi * (2 * (pairs - pair) - 1) / (2.0 * order);
            const auto q = 0.5 / std::sin (angle) * (pair == pairs - 1 ? resonance : 1.0);
            const auto alpha = sine / (2.0 * q);
            const auto a0 = 1.0 + alpha;
            coeffs[static_cast<size_t> (firstBiquad + pair)] = {
                numerator / a0, sign * 2.0 * numerator / a0, numerator / a0,
                -2.0 * cosine / a0, (1.0 - alpha) / a0
            };
        }
    }
}

ProQ3AudioProcessor::ProQ3AudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                        .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undoManager, "ProQ3Parameters", createParameterLayout())
{
    for (int i = 0; i < kMaxBands; ++i)
    {
        const auto parameter = [this, i] (const char* suffix)
        {
            return apvts.getRawParameterValue (getBandParamID (i, suffix));
        };
        bandParameterValues[static_cast<size_t> (i)] = {
            parameter ("used"), parameter ("enabled"), parameter ("shape"),
            parameter ("freq"), parameter ("gain"), parameter ("q"),
            parameter ("slope"), parameter ("stereo"), parameter ("gain_q"),
            parameter ("dyn_range"), parameter ("dyn_enabled"), parameter ("dyn_auto"),
            parameter ("dyn_thresh"), parameter ("dyn_ext_sc")
        };
        currentDynamicGainDb[static_cast<size_t> (i)].store (0.0f, std::memory_order_relaxed);
        currentDetectorLevelDb[static_cast<size_t> (i)].store (-100.0f, std::memory_order_relaxed);
    }

    midiOutputParameters = { apvts.getParameter ("global_output_gain"), apvts.getParameter ("global_gain_scale") };
    apvts.state.setProperty (factoryPresetProperty, "Default Setting", nullptr);
    stateA = apvts.copyState();
    stateB = apvts.copyState();
    updateAllFilterCoefficients();
    auto& registry = getSpectrumPeerRegistry();
    const juce::ScopedLock lock (registry.mutex);
    spectrumPeerId = registry.nextId++;
    registry.instances.push_back (this);
}

ProQ3AudioProcessor::~ProQ3AudioProcessor()
{
    auto& registry = getSpectrumPeerRegistry();
    const juce::ScopedLock lock (registry.mutex);
    registry.instances.erase (std::remove (registry.instances.begin(), registry.instances.end(), this),
                              registry.instances.end());
}

juce::String ProQ3AudioProcessor::getBandParamID (int bandIndex, const juce::String& suffix)
{
    return "band" + juce::String (bandIndex + 1).paddedLeft ('0', 2) + "_" + suffix;
}

bool ProQ3AudioProcessor::doesShapeSupportGain (int shape) noexcept
{
    // Manual Pro-Q 3 p. 11: "The gain knob sets the gain in dB... This setting is only used for Bell, Shelving and Tilt filter types."
    return (shape == kShapeBell
         || shape == kShapeLowShelf
         || shape == kShapeHighShelf
         || shape == kShapeTiltShelf
         || shape == kShapeFlatTilt);
}

bool ProQ3AudioProcessor::doesShapeSupportDynamics (int shape) noexcept
{
    // Manual Pro-Q 3 p. 11 & p. 13: "any of Pro-Q's bands (with Bell or Shelf shapes) can be made dynamic, at any slope"
    // "Note that this setting is only available for Bell and Shelving filter types" (Bell, Low Shelf, High Shelf, Tilt Shelf).
    return (shape == kShapeBell
         || shape == kShapeLowShelf
         || shape == kShapeHighShelf
         || shape == kShapeTiltShelf);
}

bool ProQ3AudioProcessor::doesShapeSupportQ (int shape, int slopeIndex) noexcept
{
    // Manual Pro-Q 3 p. 11: "The Q cannot be adjusted when a 6 dB/octave Notch is used" (e Flat Tilt usa inclinação plana em todo o espectro)
    if (shape == kShapeFlatTilt)
        return false;
    if (shape == kShapeNotch && slopeIndex == kSlope6dB)
        return false;
    if ((shape == kShapeLowCut || shape == kShapeHighCut)
        && (slopeIndex == kSlope6dB || slopeIndex == kSlopeBrickwall))
        return false;
    return true;
}

juce::AudioProcessorValueTreeState::ParameterLayout ProQ3AudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.reserve (kMaxBands * 14 + 16);

    juce::NormalisableRange<float> freqRange (10.0f, 30000.0f,
        [] (float start, float end, float norm) -> float
        {
            return start * std::pow (end / start, norm);
        },
        [] (float start, float end, float val) -> float
        {
            return std::log (juce::jlimit (start, end, val) / start) / std::log (end / start);
        },
        [] (float start, float end, float val) -> float
        {
            return juce::jlimit (start, end, val);
        });

    juce::NormalisableRange<float> qRange (0.025f, 40.0f,
        [] (float start, float end, float norm) -> float
        {
            return start * std::pow (end / start, norm);
        },
        [] (float start, float end, float val) -> float
        {
            return std::log (juce::jlimit (start, end, val) / start) / std::log (end / start);
        },
        [] (float start, float end, float val) -> float
        {
            return juce::jlimit (start, end, val);
        });

    juce::NormalisableRange<float> gainRange (-30.0f, 30.0f, 0.01f);
    juce::NormalisableRange<float> dynRange  (-30.0f, 30.0f, 0.01f);
    juce::NormalisableRange<float> threshRange (-60.0f, 0.0f, 0.1f);

    const juce::StringArray shapeChoices {
        "Bell", "Low Shelf", "Low Cut", "High Shelf", "High Cut",
        "Notch", "Band Pass", "Tilt Shelf", "Flat Tilt"
    };

    const juce::StringArray slopeChoices {
        "6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct",
        "30 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct",
        "96 dB/oct", "Brickwall"
    };

    const juce::StringArray stereoChoices {
        "Stereo", "Left", "Right", "Mid", "Side"
    };

    const std::array<float, kMaxBands> defaultFreqs {
        1000.0f, 100.0f, 250.0f, 500.0f, 2000.0f, 4000.0f, 8000.0f, 12000.0f,
        50.0f,   80.0f,  160.0f, 320.0f, 640.0f,  1250.0f, 2500.0f, 5000.0f,
        10000.0f, 16000.0f, 40.0f, 125.0f, 800.0f, 1600.0f, 3150.0f, 6300.0f
    };

    for (int i = 0; i < kMaxBands; ++i)
    {
        const juce::String prefix = "Band " + juce::String (i + 1) + " ";

        // IDs originais preservados 100% intactos
        params.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { getBandParamID (i, "used"), 1 },
            prefix + "Used",
            false));

        params.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { getBandParamID (i, "enabled"), 1 },
            prefix + "Enabled",
            true));

        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { getBandParamID (i, "shape"), 1 },
            prefix + "Shape",
            shapeChoices,
            0)); // Bell padrão

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { getBandParamID (i, "freq"), 1 },
            prefix + "Frequency",
            freqRange,
            defaultFreqs[static_cast<size_t> (i)],
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { getBandParamID (i, "gain"), 1 },
            prefix + "Gain",
            gainRange,
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { getBandParamID (i, "q"), 1 },
            prefix + "Q",
            qRange,
            1.0f));

        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { getBandParamID (i, "slope"), 1 },
            prefix + "Slope",
            slopeChoices,
            1)); // 12 dB/oct padrão

        params.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { getBandParamID (i, "stereo"), 1 },
            prefix + "Stereo Placement",
            stereoChoices,
            0)); // Stereo padrão

        params.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { getBandParamID (i, "gain_q"), 1 },
            prefix + "Gain-Q Interaction",
            false));

        // Novos parâmetros de EQ Dinâmico e Sidechain (Manual Pro-Q 3 p. 13-14)
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { getBandParamID (i, "dyn_range"), 1 },
            prefix + "Dynamic Range",
            dynRange,
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));

        params.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { getBandParamID (i, "dyn_enabled"), 1 },
            prefix + "Dynamics Enabled",
            true));

        params.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { getBandParamID (i, "dyn_auto"), 1 },
            prefix + "Dynamic Auto Threshold",
            true));

        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { getBandParamID (i, "dyn_thresh"), 1 },
            prefix + "Dynamic Threshold",
            threshRange,
            -18.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));

        params.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { getBandParamID (i, "dyn_ext_sc"), 1 },
            prefix + "External Side Chain",
            false));
    }

    // Controles globais da barra inferior
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "global_output_gain", 1 },
        "Output Level",
        juce::NormalisableRange<float> (-36.0f, 36.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "global_gain_scale", 1 },
        "Gain Scale",
        juce::NormalisableRange<float> (0.0f, 200.0f, 1.0f),
        100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "global_bypass", 1 },
        "Global Bypass",
        false));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "global_phase_invert", 1 },
        "Phase Invert",
        false));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "global_auto_gain", 1 },
        "Auto Gain",
        false));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "display_range", 1 },
        "Display Range",
        juce::StringArray { "3 dB", "6 dB", "12 dB", "30 dB" },
        2)); // 12 dB padrão

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "processing_mode", 1 },
        "Processing Mode",
        juce::StringArray { "Zero Latency", "Natural Phase", "Linear Phase" },
        0)); // Zero Latency padrão

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "analyzer_pre", 1 },
        "Analyzer Pre-EQ",
        true));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "analyzer_post", 1 },
        "Analyzer Post-EQ",
        true));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "analyzer_sc", 1 },
        "Analyzer External Sidechain",
        false));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "piano_display", 1 },
        "Piano Display",
        false));

    // Appended so existing host automation indices remain intact.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "global_output_balance", 1 },
        "Output Balance",
        juce::NormalisableRange<float> (-100.0f, 100.0f, 1.0f),
        0.0f));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "global_output_balance_mode", 1 },
        "Output Balance Mode",
        juce::StringArray { "L/R", "M/S" },
        0));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "analyzer_peak_alerts", 1 },
        "Spectrum Peak Alerts",
        true));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "analyzer_speed", 1 }, "Spectrum Speed",
        juce::StringArray { "Slow", "Normal", "Fast" }, 1));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "analyzer_tilt", 1 }, "Spectrum Tilt",
        juce::StringArray { "Flat", "3 dB/oct", "4.5 dB/oct", "6 dB/oct" }, 2));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "analyzer_range", 1 }, "Spectrum Display Range",
        juce::StringArray { "60 dB", "90 dB", "100 dB", "120 dB" }, 2));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "analyzer_detail", 1 }, "Spectrum Trace Detail",
        juce::StringArray { "Smooth", "Balanced", "Detailed" }, 1));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "analyzer_resolution", 1 }, "Spectrum FFT Resolution",
        juce::StringArray { "1024", "2048", "4096", "8192" }, 2));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "analyzer_overlap", 1 }, "Spectrum Overlap", true));

    return { params.begin(), params.end() };
}

const juce::String ProQ3AudioProcessor::getName() const { return JucePlugin_Name; }
bool ProQ3AudioProcessor::acceptsMidi() const { return true; }
bool ProQ3AudioProcessor::producesMidi() const { return false; }
bool ProQ3AudioProcessor::isMidiEffect() const { return false; }
double ProQ3AudioProcessor::getTailLengthSeconds() const { return 0.0; }
int ProQ3AudioProcessor::getNumPrograms() { return 1; }
int ProQ3AudioProcessor::getCurrentProgram() { return 0; }
void ProQ3AudioProcessor::setCurrentProgram (int) {}
const juce::String ProQ3AudioProcessor::getProgramName (int) { return "Default"; }
void ProQ3AudioProcessor::changeProgramName (int, const juce::String&) {}

void ProQ3AudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate.store (sampleRate > 0.0 ? sampleRate : 44100.0, std::memory_order_relaxed);

    for (int i = 0; i < kMaxBands; ++i)
    {
        auto& band = dspBands[static_cast<size_t> (i)];
        for (auto& st : band.states)
            st.reset();
        band.dynState.reset();
        currentDynamicGainDb[static_cast<size_t> (i)].store (0.0f, std::memory_order_relaxed);
        currentDetectorLevelDb[static_cast<size_t> (i)].store (-100.0f, std::memory_order_relaxed);
    }
    preFftFifo.resetHistory();
    postFftFifo.resetHistory();
    sidechainFftFifo.resetHistory();
    outputPeakDbL.store (kMeterFloorDb, std::memory_order_relaxed);
    outputPeakDbR.store (kMeterFloorDb, std::memory_order_relaxed);
    for (auto& peak : outputHeldPeakDb)
        peak.store (kMeterFloorDb, std::memory_order_relaxed);
    meterHoldSamples.fill (0);
    soloBandPassState.reset();
    updateAllFilterCoefficients();
}

void ProQ3AudioProcessor::releaseResources() {}

bool ProQ3AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn  = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainIn != mainOut)
        return false;

    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;

    // Barramento Sidechain opcional (pode estar desabilitado, mono ou estéreo no Studio One)
    if (layouts.inputBuses.size() > 1)
    {
        const auto scIn = layouts.getChannelSet (true, 1);
        if (! scIn.isDisabled()
            && scIn != juce::AudioChannelSet::mono()
            && scIn != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

ProQ3AudioProcessor::BandParameters ProQ3AudioProcessor::getBandParameters (int bandIndex) const noexcept
{
    BandParameters bp;
    if (bandIndex < 0 || bandIndex >= kMaxBands)
        return bp;

    const auto& values = bandParameterValues[static_cast<size_t> (bandIndex)];
    const auto getVal = [] (const std::atomic<float>* value, float fallback)
    {
        return value != nullptr ? value->load (std::memory_order_relaxed) : fallback;
    };

    bp.used           = getVal (values.used, 0.0f) >= 0.5f;
    bp.enabled        = getVal (values.enabled, 1.0f) >= 0.5f;
    bp.shape          = static_cast<int> (std::lround (getVal (values.shape, 0.0f)));
    bp.freqHz         = getVal (values.freqHz, 1000.0f);
    bp.gainDb         = getVal (values.gainDb, 0.0f);
    bp.q              = getVal (values.q, 1.0f);
    bp.slopeIndex     = static_cast<int> (std::lround (getVal (values.slopeIndex, 1.0f)));
    bp.stereoMode     = static_cast<int> (std::lround (getVal (values.stereoMode, 0.0f)));
    bp.gainQInter     = getVal (values.gainQInter, 0.0f) >= 0.5f;
    bp.dynRangeDb     = getVal (values.dynRangeDb, 0.0f);
    bp.dynEnabled     = getVal (values.dynEnabled, 1.0f) >= 0.5f;
    bp.dynAutoThresh  = getVal (values.dynAutoThresh, 1.0f) >= 0.5f;
    bp.dynThresholdDb = getVal (values.dynThresholdDb, -18.0f);
    bp.dynExtSidechain = getVal (values.dynExtSidechain, 0.0f) >= 0.5f;

    return bp;
}

void ProQ3AudioProcessor::computeBandDetectorCoeffs (double freqHz, double q, double sampleRate, BiquadCoeffs& outCoeffs) noexcept
{
    if (sampleRate <= 0.0)
    {
        outCoeffs = BiquadCoeffs {};
        return;
    }

    // Manual Pro-Q 3 p. 13: "Internally, the dynamic EQ process will trigger on a band-limited version of the plug-in's input, according to the frequency range the band works on."
    const double f0 = juce::jlimit (15.0, 0.49 * sampleRate, freqHz);
    const double detQ = juce::jlimit (0.35, 12.0, q * 0.7071067811865475);
    const double w0 = 2.0 * kPi * f0 / sampleRate;
    const double alpha = std::sin (w0) / (2.0 * detQ);
    const double invA0 = 1.0 / (1.0 + alpha);

    outCoeffs.b0 =  alpha * invA0;
    outCoeffs.b1 =  0.0;
    outCoeffs.b2 = -alpha * invA0;
    outCoeffs.a1 = (-2.0 * std::cos (w0)) * invA0;
    outCoeffs.a2 = (1.0 - alpha) * invA0;
}

double ProQ3AudioProcessor::processDynamicEnvelopeSample (const BandParameters& bp,
                                                          BandDynamicState& dynState,
                                                          double detectorInputSample,
                                                          double sampleRate) noexcept
{
    // Filtra o sinal detector pela região de frequência da banda
    const auto& c = dynState.detectorBpCoeffs;
    auto& st = dynState.detectorBpState;
    const double filtered = c.b0 * detectorInputSample + st.z1L;
    st.z1L = c.b1 * detectorInputSample - c.a1 * filtered + st.z2L;
    st.z2L = c.b2 * detectorInputSample - c.a2 * filtered;

    const double absSig = std::abs (filtered);

    // Constantes de tempo dependentes da frequência e do programa (Manual Pro-Q 3 p. 13:
    // "attack, release and knee all depend on the processed audio, the frequency range of the EQ band and the current dynamic range")
    const double freqScale = juce::jlimit (0.35, 2.5, std::sqrt (1000.0 / juce::jmax (20.0, bp.freqHz)));
    const double attackSec  = 0.0035 * freqScale;
    const double releaseSec = 0.0550 * freqScale;

    const double attCoeff = std::exp (-1.0 / juce::jmax (1.0, attackSec * sampleRate));
    const double relCoeff = std::exp (-1.0 / juce::jmax (1.0, releaseSec * sampleRate));

    if (absSig > dynState.fastEnvLin)
        dynState.fastEnvLin = attCoeff * dynState.fastEnvLin + (1.0 - attCoeff) * absSig;
    else
        dynState.fastEnvLin = relCoeff * dynState.fastEnvLin + (1.0 - relCoeff) * absSig;

    // Atualiza referência adaptativa lenta para o modo Auto Threshold (~350 ms)
    const double slowCoeff = std::exp (-1.0 / juce::jmax (1.0, 0.35 * sampleRate));
    if (absSig > 1.0e-5)
        dynState.slowAutoRefLin = slowCoeff * dynState.slowAutoRefLin + (1.0 - slowCoeff) * absSig;

    const double envDb = (dynState.fastEnvLin > 1.0e-6)
                            ? (20.0 * std::log10 (dynState.fastEnvLin))
                            : -100.0;
    dynState.detectorLevelDb = envDb;

    // Se a forma não suporta dinâmica (ex: Flat Tilt, Low/High Cut, Notch, Band Pass),
    // ou se o botão de Bypass Dynamics estiver desligado, ou se a faixa dinâmica for 0 dB:
    // o ganho dinâmico aplicado pelo DSP é exatamente 0.0 dB (banda puramente estática)
    if (! doesShapeSupportDynamics (bp.shape) || ! bp.dynEnabled || std::abs (bp.dynRangeDb) < 1.0e-4)
    {
        dynState.smoothedGainModDb *= 0.90;
        if (std::abs (dynState.smoothedGainModDb) < 1.0e-4)
            dynState.smoothedGainModDb = 0.0;
        return dynState.smoothedGainModDb;
    }

    // Determina o limiar efetivo em dB (Auto vs Manual)
    double effectiveThreshDb = bp.dynThresholdDb;
    if (bp.dynAutoThresh)
    {
        // No modo Auto Threshold (Manual p. 14: "the threshold is constantly adjusting to the level of the current, band-limited trigger signal"),
        // ancora o limiar 4.5 dB abaixo do nível médio recente (com piso em -48 dB)
        const double autoRefDb = (dynState.slowAutoRefLin > 1.0e-5)
                                    ? (20.0 * std::log10 (dynState.slowAutoRefLin))
                                    : -48.0;
        effectiveThreshDb = juce::jlimit (-54.0, -3.0, autoRefDb - 4.5);
    }

    // Soft knee de 6 dB (Manual p. 14: "a soft knee is used internally by the dynamic EQ algorithm, so it can start triggering a little bit below the selected threshold value")
    const double overDb = envDb - effectiveThreshDb;
    const double kneeWidthDb = 6.0;
    const double fullRangeSpanDb = 12.0; // 12 dB acima do limiar atinge ~100% da faixa dinâmica configurada

    double normTrigger = 0.0;
    if (overDb <= -0.5 * kneeWidthDb)
    {
        normTrigger = 0.0;
    }
    else if (overDb < 0.5 * kneeWidthDb)
    {
        const double t = (overDb + 0.5 * kneeWidthDb) / kneeWidthDb;
        const double softDb = 0.5 * kneeWidthDb * t * t;
        normTrigger = softDb / fullRangeSpanDb;
    }
    else
    {
        normTrigger = overDb / fullRangeSpanDb;
    }

    // Saturação suave assintótica em [0.0, 1.0] para preservar naturalidade musical
    normTrigger = juce::jlimit (0.0, 1.0, normTrigger / (1.0 + 0.35 * juce::jmax (0.0, normTrigger - 0.65)));

    const double targetModDb = bp.dynRangeDb * normTrigger;
    const double modSmooth = (std::abs (targetModDb) > std::abs (dynState.smoothedGainModDb)) ? attCoeff : relCoeff;
    dynState.smoothedGainModDb = modSmooth * dynState.smoothedGainModDb + (1.0 - modSmooth) * targetModDb;

    return dynState.smoothedGainModDb;
}

void ProQ3AudioProcessor::computeFilterCascadeCoeffs (const BandParameters& params,
                                                      double sampleRate,
                                                      double gainScale,
                                                      double dynamicGainOffsetDb,
                                                      std::array<BiquadCoeffs, kMaxCascadedSections>& outCoeffs,
                                                      int& outNumSections) noexcept
{
    const auto order = getFilterOrder (params.slopeIndex);
    outNumSections = (order + 1) / 2;
    for (auto& c : outCoeffs)
        c = BiquadCoeffs {};

    if (! params.used || ! params.enabled || sampleRate <= 0.0)
        return;

    const double nyquist = 0.498 * sampleRate;
    const double f0 = juce::jlimit (10.0, nyquist, params.freqHz);
    if (params.shape == kShapeLowCut || params.shape == kShapeHighCut)
    {
        const auto resonance = order == 1 || params.slopeIndex == kSlopeBrickwall
            ? 1.0 : juce::jlimit (0.025, 40.0, params.q);
        designCutFilter (order, f0, sampleRate, resonance, params.shape == kShapeLowCut, outCoeffs);
        return;
    }

    const double w0 = 2.0 * kPi * f0 / sampleRate;
    const double cosW0 = std::cos (w0);
    const double sinW0 = std::sin (w0);

    // Compensação de cramping perto de Nyquist (Pro-Q 3 Zero Latency analog magnitude matching)
    const double nyquistRatio = f0 / (0.5 * sampleRate);
    const double crampComp = 1.0 / std::sqrt (juce::jmax (0.15, 1.0 - 0.65 * nyquistRatio * nyquistRatio));

    // Q=1.0 no Pro-Q 3 corresponde a 1/sqrt(2) = 0.70710678 (Manual p. 11)
    double effectiveQ = juce::jlimit (0.025, 40.0, params.q) * 0.7071067811865475;

    // Soma o ganho estático escalado por Gain Scale ao ganho dinâmico instantâneo (se o tipo suportar dinâmica)
    const double staticGainDb = doesShapeSupportGain (params.shape) ? (params.gainDb * gainScale) : 0.0;
    const double dynGainDb    = (doesShapeSupportDynamics (params.shape) && params.dynEnabled) ? dynamicGainOffsetDb : 0.0;
    const double totalGainDb  = juce::jlimit (-45.0, 45.0, staticGainDb + dynGainDb);

    // Gain-Q interaction (Manual p. 11: afeta Bell)
    if (params.gainQInter && params.shape == kShapeBell)
    {
        const double gainFactor = 1.0 + 0.035 * std::abs (totalGainDb);
        effectiveQ = juce::jlimit (0.018, 40.0, effectiveQ * gainFactor);
    }

    if (params.slopeIndex == kSlope6dB)
        effectiveQ = params.shape == kShapeNotch ? 0.7071067811865475 : effectiveQ * 0.55;
    else if (params.slopeIndex == kSlopeBrickwall)
        effectiveQ = juce::jmax (effectiveQ, 1.25);

    const double qComp = effectiveQ * crampComp;
    BiquadCoeffs section;

    switch (params.shape)
    {
        case kShapeBell:
        {
            if (std::abs (totalGainDb) < 1.0e-5)
                return;
            const double sectionGainDb = totalGainDb / static_cast<double> (outNumSections);
            const double A = std::pow (10.0, sectionGainDb / 40.0);
            const double alpha = sinW0 / (2.0 * qComp);

            const double b0 = 1.0 + alpha * A;
            const double b1 = -2.0 * cosW0;
            const double b2 = 1.0 - alpha * A;
            const double a0 = 1.0 + alpha / A;
            const double a1 = -2.0 * cosW0;
            const double a2 = 1.0 - alpha / A;
            const double invA0 = 1.0 / a0;

            section.b0 = b0 * invA0;
            section.b1 = b1 * invA0;
            section.b2 = b2 * invA0;
            section.a1 = a1 * invA0;
            section.a2 = a2 * invA0;
            break;
        }

        case kShapeLowShelf:
        {
            if (std::abs (totalGainDb) < 1.0e-5)
                return;
            const double sectionGainDb = totalGainDb / static_cast<double> (outNumSections);
            const double A = std::pow (10.0, sectionGainDb / 40.0);
            const double sqrtA = std::sqrt (A);
            const double alpha = sinW0 / (2.0 * qComp);

            const double b0 =      A * ((A + 1.0) - (A - 1.0) * cosW0 + 2.0 * sqrtA * alpha);
            const double b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosW0);
            const double b2 =      A * ((A + 1.0) - (A - 1.0) * cosW0 - 2.0 * sqrtA * alpha);
            const double a0 =           (A + 1.0) + (A - 1.0) * cosW0 + 2.0 * sqrtA * alpha;
            const double a1 = -2.0 *     ((A - 1.0) + (A + 1.0) * cosW0);
            const double a2 =           (A + 1.0) + (A - 1.0) * cosW0 - 2.0 * sqrtA * alpha;
            const double invA0 = 1.0 / a0;

            section.b0 = b0 * invA0;
            section.b1 = b1 * invA0;
            section.b2 = b2 * invA0;
            section.a1 = a1 * invA0;
            section.a2 = a2 * invA0;
            break;
        }

        case kShapeHighShelf:
        {
            if (std::abs (totalGainDb) < 1.0e-5)
                return;
            const double sectionGainDb = totalGainDb / static_cast<double> (outNumSections);
            const double A = std::pow (10.0, sectionGainDb / 40.0);
            const double sqrtA = std::sqrt (A);
            const double alpha = sinW0 / (2.0 * qComp);

            const double b0 =      A * ((A + 1.0) + (A - 1.0) * cosW0 + 2.0 * sqrtA * alpha);
            const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW0);
            const double b2 =      A * ((A + 1.0) + (A - 1.0) * cosW0 - 2.0 * sqrtA * alpha);
            const double a0 =           (A + 1.0) - (A - 1.0) * cosW0 + 2.0 * sqrtA * alpha;
            const double a1 =  2.0 *     ((A - 1.0) - (A + 1.0) * cosW0);
            const double a2 =           (A + 1.0) - (A - 1.0) * cosW0 - 2.0 * sqrtA * alpha;
            const double invA0 = 1.0 / a0;

            section.b0 = b0 * invA0;
            section.b1 = b1 * invA0;
            section.b2 = b2 * invA0;
            section.a1 = a1 * invA0;
            section.a2 = a2 * invA0;
            break;
        }

        case kShapeNotch:
        {
            const double alpha = sinW0 / (2.0 * qComp);
            const double b0 =  1.0;
            const double b1 = -2.0 * cosW0;
            const double b2 =  1.0;
            const double a0 =  1.0 + alpha;
            const double a1 = -2.0 * cosW0;
            const double a2 =  1.0 - alpha;
            const double invA0 = 1.0 / a0;

            section.b0 = b0 * invA0;
            section.b1 = b1 * invA0;
            section.b2 = b2 * invA0;
            section.a1 = a1 * invA0;
            section.a2 = a2 * invA0;
            break;
        }

        case kShapeBandPass:
        {
            const double alpha = sinW0 / (2.0 * qComp);
            const double b0 =  alpha;
            const double b1 =  0.0;
            const double b2 = -alpha;
            const double a0 =  1.0 + alpha;
            const double a1 = -2.0 * cosW0;
            const double a2 =  1.0 - alpha;
            const double invA0 = 1.0 / a0;

            section.b0 = b0 * invA0;
            section.b1 = b1 * invA0;
            section.b2 = b2 * invA0;
            section.a1 = a1 * invA0;
            section.a2 = a2 * invA0;
            break;
        }

        case kShapeTiltShelf:
        case kShapeFlatTilt:
        {
            // Tilt Shelf inclina graves e agudos simetricamente em torno de f0;
            // Flat Tilt usa Q ultralargo fixo (0.28) para uma inclinação linear em todo o espectro audível (Manual p. 1 & p. 11)
            if (std::abs (totalGainDb) < 1.0e-5)
                return;
            const double tiltQ = (params.shape == kShapeFlatTilt) ? 0.28 : qComp;
            const double sectionGainDb = totalGainDb / static_cast<double> (outNumSections);
            const double A = std::pow (10.0, sectionGainDb / 20.0); // Metade para baixo em LF, metade para cima em HF
            const double sqrtA = std::sqrt (A);
            const double alpha = sinW0 / (2.0 * tiltQ);

            const double b0 =      sqrtA * ((A + 1.0) + (A - 1.0) * cosW0 + 2.0 * sqrtA * alpha);
            const double b1 = -2.0 * sqrtA * ((A - 1.0) + (A + 1.0) * cosW0);
            const double b2 =      sqrtA * ((A + 1.0) + (A - 1.0) * cosW0 - 2.0 * sqrtA * alpha);
            const double a0 =     (1.0 / sqrtA) * (A * ((A + 1.0) - (A - 1.0) * cosW0 + 2.0 * sqrtA * alpha));
            const double a1 =  2.0 * (1.0 / sqrtA) * (A * ((A - 1.0) - (A + 1.0) * cosW0));
            const double a2 =     (1.0 / sqrtA) * (A * ((A + 1.0) - (A - 1.0) * cosW0 - 2.0 * sqrtA * alpha));
            const double invA0 = 1.0 / a0;

            section.b0 = b0 * invA0;
            section.b1 = b1 * invA0;
            section.b2 = b2 * invA0;
            section.a1 = a1 * invA0;
            section.a2 = a2 * invA0;
            break;
        }

        default:
            break;
    }

    for (int s = 0; s < outNumSections; ++s)
        outCoeffs[static_cast<size_t> (s)] = section;
}

void ProQ3AudioProcessor::updateAllFilterCoefficients() noexcept
{
    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    const double gainScale = static_cast<double> (apvts.getRawParameterValue ("global_gain_scale")->load (std::memory_order_relaxed)) * 0.01;

    for (int i = 0; i < kMaxBands; ++i)
    {
        const BandParameters bp = getBandParameters (i);
        auto& band = dspBands[static_cast<size_t> (i)];
        computeBandDetectorCoeffs (bp.freqHz, bp.q, sr, band.dynState.detectorBpCoeffs);

        const double dynModDb = static_cast<double> (currentDynamicGainDb[static_cast<size_t> (i)].load (std::memory_order_relaxed));
        computeFilterCascadeCoeffs (bp, sr, gainScale, dynModDb, band.coeffs, band.activeSections);
    }

    const int sIdx = soloBandIndex.load (std::memory_order_relaxed);
    if (sIdx >= 0 && sIdx < kMaxBands)
    {
        const double f0 = juce::jlimit (10.0, 0.495 * sr, static_cast<double> (soloFrequencyHz.load (std::memory_order_relaxed)));
        const double q  = juce::jlimit (0.1, 40.0, static_cast<double> (soloBandwidthQ.load (std::memory_order_relaxed)));
        const double w0 = 2.0 * kPi * f0 / sr;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double invA0 = 1.0 / (1.0 + alpha);

        soloBandPassCoeffs.b0 =  alpha * invA0;
        soloBandPassCoeffs.b1 =  0.0;
        soloBandPassCoeffs.b2 = -alpha * invA0;
        soloBandPassCoeffs.a1 = (-2.0 * std::cos (w0)) * invA0;
        soloBandPassCoeffs.a2 = (1.0 - alpha) * invA0;
    }
}

ProQ3AudioProcessor::BandResponse ProQ3AudioProcessor::makeBandResponse (BandParameters parameters,
                                                                       double dynamicGainDb) const noexcept
{
    BandResponse response;
    if (! parameters.used)
        return response;

    response.sampleRate = currentSampleRate.load (std::memory_order_relaxed);
    const double gainScale = apvts.getRawParameterValue ("global_gain_scale")->load (std::memory_order_relaxed) * 0.01;
    parameters.enabled = true;
    computeFilterCascadeCoeffs (parameters, response.sampleRate, gainScale, dynamicGainDb, response.coefficients, response.sections);
    return response;
}

double ProQ3AudioProcessor::BandResponse::magnitudeDbAtFrequency (double freqHz) const noexcept
{
    if (sections == 0)
        return 0.0;

    const double sr = sampleRate;
    const double clampedFreq = juce::jlimit (10.0, 0.498 * sr, freqHz);
    const double w = 2.0 * kPi * clampedFreq / sr;

    const std::complex<double> z1 (std::cos (-w), std::sin (-w));
    const auto z2 = z1 * z1;

    double magnitude = 1.0;
    for (int section = 0; section < sections; ++section)
    {
        const auto& c = coefficients[static_cast<size_t> (section)];
        const auto numerator = c.b0 + c.b1 * z1 + c.b2 * z2;
        const auto denominator = 1.0 + c.a1 * z1 + c.a2 * z2;
        magnitude *= std::norm (numerator) / std::norm (denominator);
    }
    return 10.0 * std::log10 (juce::jmax (1.0e-20, magnitude));
}

double ProQ3AudioProcessor::getBandMagnitudeDbAtFrequency (int bandIndex, double freqHz, bool includeDynamicGain) const noexcept
{
    if (bandIndex < 0 || bandIndex >= kMaxBands)
        return 0.0;

    return makeBandResponse (getBandParameters (bandIndex),
                             includeDynamicGain ? getBandDynamicGainDb (bandIndex) : 0.0f)
        .magnitudeDbAtFrequency (freqHz);
}

double ProQ3AudioProcessor::getTotalMagnitudeDbAtFrequency (double freqHz, bool includeDynamicGain) const noexcept
{
    double totalDb = 0.0;
    for (int i = 0; i < kMaxBands; ++i)
    {
        const BandParameters bp = getBandParameters (i);
        if (! bp.used || ! bp.enabled)
            continue;
        totalDb += getBandMagnitudeDbAtFrequency (i, freqHz, includeDynamicGain);
    }
    return totalDb;
}

double ProQ3AudioProcessor::getChannelMagnitudeDbAtFrequency (double freqHz, int targetStereoMode, bool includeDynamicGain) const noexcept
{
    double totalDb = 0.0;
    for (int i = 0; i < kMaxBands; ++i)
    {
        const BandParameters bp = getBandParameters (i);
        if (! bp.used || ! bp.enabled)
            continue;
        if (bp.stereoMode == 0 || bp.stereoMode == targetStereoMode)
            totalDb += getBandMagnitudeDbAtFrequency (i, freqHz, includeDynamicGain);
    }
    return totalDb;
}

int ProQ3AudioProcessor::getFactoryPresetCount() noexcept
{
    return static_cast<int> (std::size (FactoryPresets::entries));
}

juce::String ProQ3AudioProcessor::getFactoryPresetName (int index)
{
    return juce::isPositiveAndBelow (index, getFactoryPresetCount())
        ? juce::String (FactoryPresets::entries[index].name) : juce::String();
}

juce::String ProQ3AudioProcessor::getFactoryPresetCategory (int index)
{
    if (! juce::isPositiveAndBelow (index, getFactoryPresetCount()))
        return {};

    const auto* category = FactoryPresets::entries[index].category;
    return category != nullptr ? juce::String (category) : juce::String();
}

int ProQ3AudioProcessor::getFactoryPresetIndex() const
{
    const auto name = apvts.state.getProperty (factoryPresetProperty).toString();
    for (int index = 0; index < getFactoryPresetCount(); ++index)
        if (name == FactoryPresets::entries[index].name)
            return index;
    return -1;
}

void ProQ3AudioProcessor::loadFactoryPreset (int index)
{
    if (! juce::isPositiveAndBelow (index, getFactoryPresetCount()))
        return;

    const auto& preset = FactoryPresets::entries[index];
    jassert (preset.bands.size() <= kMaxBands);

    // APVTS writes undoable parameter changes to its tree asynchronously.
    apvts.copyState();
    undoManager.beginNewTransaction ("Load Preset: " + juce::String (preset.name));

    {
        const juce::ScopedLock callbackLock (getCallbackLock());
        const auto setParameter = [this] (const juce::String& id, float value)
        {
            if (auto* parameter = apvts.getParameter (id))
            {
                const auto normalized = parameter->convertTo0to1 (value);
                if (! juce::approximatelyEqual (parameter->getValue(), normalized))
                    parameter->setValueNotifyingHost (normalized);
            }
        };

        int slot = 0;
        for (const auto& band : preset.bands)
        {
            const auto set = [slot, &setParameter] (const char* suffix, double value)
            {
                setParameter (getBandParamID (slot, suffix), static_cast<float> (value));
            };
            set ("enabled", band.enabled);
            set ("shape", band.shape);
            set ("freq", band.freqHz);
            set ("gain", band.gainDb);
            set ("q", band.q);
            set ("slope", band.slopeIndex);
            set ("stereo", band.stereoMode);
            set ("gain_q", band.gainQInter);
            set ("dyn_range", band.dynRangeDb);
            set ("dyn_enabled", band.dynEnabled);
            set ("dyn_auto", band.dynAutoThresh);
            set ("dyn_thresh", band.dynThresholdDb);
            set ("dyn_ext_sc", band.dynExtSidechain);
            set ("used", true);
            ++slot;
        }
        for (; slot < kMaxBands; ++slot)
            setParameter (getBandParamID (slot, "used"), 0.0f);

        if (preset.displayRange >= 0)
        {
            setParameter ("display_range", static_cast<float> (preset.displayRange));
            setParameter ("processing_mode", static_cast<float> (preset.phase));
            setParameter ("global_output_gain", preset.output);
            setParameter ("global_gain_scale", 100.0f);
            setParameter ("global_bypass", 0.0f);
            setParameter ("global_phase_invert", 0.0f);
            setParameter ("global_auto_gain", 0.0f);
            setParameter ("global_output_balance", 0.0f);
            setParameter ("global_output_balance_mode", 0.0f);
            setParameter ("analyzer_pre", 1.0f);
            setParameter ("analyzer_post", 1.0f);
            setParameter ("analyzer_sc", 0.0f);
            setParameter ("piano_display", 0.0f);
        }

        setSelectedBandIndex (preset.bands.size() == 0 ? -1 : 0);
        setSoloBandIndex (-1);
        soloBandPassState.reset();
        for (size_t band = 0; band < dspBands.size(); ++band)
        {
            for (auto& state : dspBands[band].states)
                state.reset();
            dspBands[band].dynState.reset();
            currentDynamicGainDb[band].store (0.0f, std::memory_order_relaxed);
            currentDetectorLevelDb[band].store (-100.0f, std::memory_order_relaxed);
        }
    }

    apvts.state.setProperty (factoryPresetProperty, preset.name, &undoManager);
    apvts.copyState();
    undoManager.beginNewTransaction();
}

int ProQ3AudioProcessor::activateFirstFreeBand (float freqHz, float gainDb, float qValue, int shape, float dynRangeDb, bool newUndoTransaction)
{
    for (int i = 0; i < kMaxBands; ++i)
    {
        const auto bp = getBandParameters (i);
        if (! bp.used)
        {
            if (newUndoTransaction)
                undoManager.beginNewTransaction ("Create EQ Band " + juce::String (i + 1));

            auto setParam = [this] (const juce::String& id, float plainValue)
            {
                if (auto* p = apvts.getParameter (id))
                    p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
            };

            setParam (getBandParamID (i, "used"), 1.0f);
            setParam (getBandParamID (i, "enabled"), 1.0f);
            setParam (getBandParamID (i, "shape"), static_cast<float> (juce::jlimit (0, 8, shape)));
            setParam (getBandParamID (i, "freq"), juce::jlimit (10.0f, 30000.0f, freqHz));
            setParam (getBandParamID (i, "gain"), juce::jlimit (-30.0f, 30.0f, gainDb));
            setParam (getBandParamID (i, "q"), juce::jlimit (0.025f, 40.0f, qValue));
            setParam (getBandParamID (i, "slope"), 1.0f); // 12 dB/oct
            setParam (getBandParamID (i, "stereo"), 0.0f);
            setParam (getBandParamID (i, "dyn_range"), juce::jlimit (-30.0f, 30.0f, dynRangeDb));
            setParam (getBandParamID (i, "dyn_enabled"), 1.0f);
            setParam (getBandParamID (i, "dyn_auto"), 1.0f);

            setSelectedBandIndex (i);
            return i;
        }
    }
    return -1;
}

void ProQ3AudioProcessor::deactivateBand (int bandIndex)
{
    if (bandIndex < 0 || bandIndex >= kMaxBands)
        return;

    undoManager.beginNewTransaction ("Delete EQ Band " + juce::String (bandIndex + 1));

    auto setParam = [this] (const juce::String& id, float plainValue)
    {
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
    };

    setParam (getBandParamID (bandIndex, "used"), 0.0f);
    setParam (getBandParamID (bandIndex, "gain"), 0.0f);
    setParam (getBandParamID (bandIndex, "dyn_range"), 0.0f);

    for (auto& st : dspBands[static_cast<size_t> (bandIndex)].states)
        st.reset();
    dspBands[static_cast<size_t> (bandIndex)].dynState.reset();
    currentDynamicGainDb[static_cast<size_t> (bandIndex)].store (0.0f, std::memory_order_relaxed);
    currentDetectorLevelDb[static_cast<size_t> (bandIndex)].store (-100.0f, std::memory_order_relaxed);

    int nextSel = -1;
    for (int i = 0; i < kMaxBands; ++i)
    {
        if (i != bandIndex && getBandParameters (i).used)
        {
            nextSel = i;
            break;
        }
    }
    setSelectedBandIndex (nextSel);
}

void ProQ3AudioProcessor::setSoloBandIndex (int idx, float soloFreqHz, float soloQ) noexcept
{
    soloFrequencyHz.store (soloFreqHz, std::memory_order_relaxed);
    soloBandwidthQ.store (soloQ, std::memory_order_relaxed);
    soloBandIndex.store (idx, std::memory_order_relaxed);
}

void ProQ3AudioProcessor::toggleABState()
{
    if (! currentIsB)
    {
        stateA = apvts.copyState();
        apvts.replaceState (stateB.createCopy());
        currentIsB = true;
    }
    else
    {
        stateB = apvts.copyState();
        apvts.replaceState (stateA.createCopy());
        currentIsB = false;
    }
}

void ProQ3AudioProcessor::copyActiveToInactiveState()
{
    if (! currentIsB)
        stateB = apvts.copyState();
    else
        stateA = apvts.copyState();
}

void ProQ3AudioProcessor::armMidiLearn (MidiOutputTarget target) noexcept
{
    armedMidiTarget.store (static_cast<int> (target), std::memory_order_relaxed);
}

void ProQ3AudioProcessor::clearMidiLearn() noexcept
{
    armedMidiTarget.store (-1, std::memory_order_relaxed);
    for (auto& assignment : learnedControllers)
        assignment.store (-1, std::memory_order_relaxed);
}

bool ProQ3AudioProcessor::isMidiLearnArmed() const noexcept
{
    return armedMidiTarget.load (std::memory_order_relaxed) >= 0;
}

bool ProQ3AudioProcessor::hasMidiOutputMapping() const noexcept
{
    return std::any_of (learnedControllers.begin(), learnedControllers.end(), [] (const auto& controller)
    {
        return controller.load (std::memory_order_relaxed) >= 0;
    });
}

void ProQ3AudioProcessor::processMidiLearn (const juce::MidiBuffer& midiMessages)
{
    if (midiMessages.isEmpty() || (! isMidiLearnArmed() && ! hasMidiOutputMapping()))
        return;

    std::array<int, 2> lastValues { -1, -1 };
    for (const auto event : midiMessages)
    {
        if (event.numBytes != 3 || (event.data[0] & 0xf0) != 0xb0)
            continue;

        const int controller = (event.data[0] & 0x0f) * 128 + event.data[1];
        const int learning = armedMidiTarget.exchange (-1, std::memory_order_relaxed);
        if (juce::isPositiveAndBelow (learning, static_cast<int> (learnedControllers.size())))
            learnedControllers[static_cast<size_t> (learning)].store (controller, std::memory_order_relaxed);

        for (size_t target = 0; target < learnedControllers.size(); ++target)
            if (learnedControllers[target].load (std::memory_order_relaxed) == controller)
                lastValues[target] = event.data[2];
    }

    for (size_t target = 0; target < lastValues.size(); ++target)
        if (lastValues[target] >= 0)
            midiOutputParameters[target]->setValueNotifyingHost (static_cast<float> (lastValues[target]) / 127.0f);
}

void ProQ3AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    processMidiLearn (midiMessages);

    auto mainBus = getBusBuffer (buffer, true, 0);
    const int numMainChannels = mainBus.getNumChannels();
    const int numSamples      = buffer.getNumSamples();

    if (numSamples <= 0 || numMainChannels <= 0)
        return;

    const auto resolution = juce::jlimit (0, 3, static_cast<int> (apvts.getRawParameterValue ("analyzer_resolution")->load (std::memory_order_relaxed)));
    const auto fftSize = size_t { 1024 } << resolution;
    preFftFifo.setResolution (fftSize);
    postFftFifo.setResolution (fftSize);
    sidechainFftFifo.setResolution (fftSize);

    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    const double gainScale = static_cast<double> (apvts.getRawParameterValue ("global_gain_scale")->load (std::memory_order_relaxed)) * 0.01;
    const bool globalBypass = apvts.getRawParameterValue ("global_bypass")->load (std::memory_order_relaxed) >= 0.5f;
    const bool phaseInvert  = apvts.getRawParameterValue ("global_phase_invert")->load (std::memory_order_relaxed) >= 0.5f;
    const bool autoGain     = apvts.getRawParameterValue ("global_auto_gain")->load (std::memory_order_relaxed) >= 0.5f;
    const float outGainDb   = apvts.getRawParameterValue ("global_output_gain")->load (std::memory_order_relaxed);

    float* leftChannel  = mainBus.getWritePointer (0);
    float* rightChannel = (numMainChannels > 1) ? mainBus.getWritePointer (1) : nullptr;

    // Barramento auxiliar External Side Chain (Bus 1)
    const float* scLeft  = nullptr;
    const float* scRight = nullptr;
    if (getBusCount (true) > 1)
    {
        auto scBus = getBusBuffer (buffer, true, 1);
        if (scBus.getNumChannels() > 0)
        {
            scLeft  = scBus.getReadPointer (0);
            scRight = (scBus.getNumChannels() > 1) ? scBus.getReadPointer (1) : scLeft;
        }
    }

    for (int s = 0; s < numSamples; ++s)
    {
        preFftFifo.push (leftChannel[s], rightChannel != nullptr ? rightChannel[s] : leftChannel[s]);
        sidechainFftFifo.push (scLeft != nullptr ? scLeft[s] : 0.0f, scRight != nullptr ? scRight[s] : 0.0f);
    }

    if (! globalBypass)
    {
        const int activeSolo = soloBandIndex.load (std::memory_order_relaxed);

        if (activeSolo >= 0 && activeSolo < kMaxBands)
        {
            updateAllFilterCoefficients();
            const auto& c = soloBandPassCoeffs;
            auto& st = soloBandPassState;

            for (int s = 0; s < numSamples; ++s)
            {
                const double xL = static_cast<double> (leftChannel[s]);
                const double yL = c.b0 * xL + st.z1L;
                st.z1L = c.b1 * xL - c.a1 * yL + st.z2L;
                st.z2L = c.b2 * xL - c.a2 * yL;
                leftChannel[s] = static_cast<float> (yL);

                if (rightChannel != nullptr)
                {
                    const double xR = static_cast<double> (rightChannel[s]);
                    const double yR = c.b0 * xR + st.z1R;
                    st.z1R = c.b1 * xR - c.a1 * yR + st.z2R;
                    st.z2R = c.b2 * xR - c.a2 * yR;
                    rightChannel[s] = static_cast<float> (yR);
                }
            }
        }
        else
        {
            // Processamento das 24 bandas (com EQ Dinâmico e Sidechain interno/externo)
            for (int b = 0; b < kMaxBands; ++b)
            {
                const BandParameters bp = getBandParameters (b);
                auto& chain = dspBands[static_cast<size_t> (b)];

                if (! bp.used || ! bp.enabled)
                {
                    currentDynamicGainDb[static_cast<size_t> (b)].store (0.0f, std::memory_order_relaxed);
                    currentDetectorLevelDb[static_cast<size_t> (b)].store (-100.0f, std::memory_order_relaxed);
                    continue;
                }

                computeBandDetectorCoeffs (bp.freqHz, bp.q, sr, chain.dynState.detectorBpCoeffs);

                const bool hasDynamicRange = doesShapeSupportDynamics (bp.shape) && std::abs (bp.dynRangeDb) > 1.0e-4;
                double lastModDb = 0.0;

                // Se a banda tem alcance dinâmico configurado (ou se está no modo manual monitorando o detector),
                // roda o detector sample-a-sample sobre a fonte selecionada (Main Input ou External Sidechain)
                if (hasDynamicRange || ! bp.dynAutoThresh)
                {
                    // Manual Pro-Q 3 p. 14: External side chain só dispara quando em modo manual e dynExtSidechain == true
                    const bool useExtSc = (! bp.dynAutoThresh) && bp.dynExtSidechain && (scLeft != nullptr);

                    for (int s = 0; s < numSamples; ++s)
                    {
                        double detSample = 0.0;
                        if (useExtSc)
                        {
                            detSample = (scRight != nullptr)
                                            ? 0.5 * (static_cast<double> (scLeft[s]) + static_cast<double> (scRight[s]))
                                            : static_cast<double> (scLeft[s]);
                        }
                        else
                        {
                            if (rightChannel == nullptr || bp.stereoMode == 0)
                                detSample = (rightChannel != nullptr)
                                                ? 0.5 * (static_cast<double> (leftChannel[s]) + static_cast<double> (rightChannel[s]))
                                                : static_cast<double> (leftChannel[s]);
                            else if (bp.stereoMode == 1)
                                detSample = static_cast<double> (leftChannel[s]);
                            else if (bp.stereoMode == 2)
                                detSample = static_cast<double> (rightChannel[s]);
                            else if (bp.stereoMode == 3)
                                detSample = 0.5 * (static_cast<double> (leftChannel[s]) + static_cast<double> (rightChannel[s]));
                            else
                                detSample = 0.5 * (static_cast<double> (leftChannel[s]) - static_cast<double> (rightChannel[s]));
                        }

                        lastModDb = processDynamicEnvelopeSample (bp, chain.dynState, detSample, sr);

                        // Atualiza coeficientes do filtro a cada 16 amostras para transições dinâmicas livres de zipper noise
                        if ((s & 15) == 0)
                            computeFilterCascadeCoeffs (bp, sr, gainScale, lastModDb, chain.coeffs, chain.activeSections);

                        const int numSec = chain.activeSections;
                        for (int sec = 0; sec < numSec; ++sec)
                        {
                            const auto& c = chain.coeffs[static_cast<size_t> (sec)];
                            auto& st      = chain.states[static_cast<size_t> (sec)];

                            if (rightChannel == nullptr || bp.stereoMode == 0)
                            {
                                const double xL = static_cast<double> (leftChannel[s]);
                                const double yL = c.b0 * xL + st.z1L;
                                st.z1L = c.b1 * xL - c.a1 * yL + st.z2L;
                                st.z2L = c.b2 * xL - c.a2 * yL;
                                leftChannel[s] = static_cast<float> (yL);

                                if (rightChannel != nullptr)
                                {
                                    const double xR = static_cast<double> (rightChannel[s]);
                                    const double yR = c.b0 * xR + st.z1R;
                                    st.z1R = c.b1 * xR - c.a1 * yR + st.z2R;
                                    st.z2R = c.b2 * xR - c.a2 * yR;
                                    rightChannel[s] = static_cast<float> (yR);
                                }
                            }
                            else if (bp.stereoMode == 1)
                            {
                                const double xL = static_cast<double> (leftChannel[s]);
                                const double yL = c.b0 * xL + st.z1L;
                                st.z1L = c.b1 * xL - c.a1 * yL + st.z2L;
                                st.z2L = c.b2 * xL - c.a2 * yL;
                                leftChannel[s] = static_cast<float> (yL);
                            }
                            else if (bp.stereoMode == 2)
                            {
                                const double xR = static_cast<double> (rightChannel[s]);
                                const double yR = c.b0 * xR + st.z1R;
                                st.z1R = c.b1 * xR - c.a1 * yR + st.z2R;
                                st.z2R = c.b2 * xR - c.a2 * yR;
                                rightChannel[s] = static_cast<float> (yR);
                            }
                            else if (bp.stereoMode == 3)
                            {
                                const double l = static_cast<double> (leftChannel[s]);
                                const double r = static_cast<double> (rightChannel[s]);
                                const double mid  = 0.5 * (l + r);
                                const double side = 0.5 * (l - r);

                                const double yMid = c.b0 * mid + st.z1L;
                                st.z1L = c.b1 * mid - c.a1 * yMid + st.z2L;
                                st.z2L = c.b2 * mid - c.a2 * yMid;

                                leftChannel[s]  = static_cast<float> (yMid + side);
                                rightChannel[s] = static_cast<float> (yMid - side);
                            }
                            else if (bp.stereoMode == 4)
                            {
                                const double l = static_cast<double> (leftChannel[s]);
                                const double r = static_cast<double> (rightChannel[s]);
                                const double mid  = 0.5 * (l + r);
                                const double side = 0.5 * (l - r);

                                const double ySide = c.b0 * side + st.z1R;
                                st.z1R = c.b1 * side - c.a1 * ySide + st.z2R;
                                st.z2R = c.b2 * side - c.a2 * ySide;

                                leftChannel[s]  = static_cast<float> (mid + ySide);
                                rightChannel[s] = static_cast<float> (mid - ySide);
                            }
                        }
                    }

                    currentDynamicGainDb[static_cast<size_t> (b)].store (static_cast<float> (lastModDb), std::memory_order_relaxed);
                    currentDetectorLevelDb[static_cast<size_t> (b)].store (static_cast<float> (chain.dynState.detectorLevelDb), std::memory_order_relaxed);
                }
                else
                {
                    // Banda estática pura
                    currentDynamicGainDb[static_cast<size_t> (b)].store (0.0f, std::memory_order_relaxed);
                    currentDetectorLevelDb[static_cast<size_t> (b)].store (-100.0f, std::memory_order_relaxed);

                    if (doesShapeSupportGain (bp.shape) && std::abs (bp.gainDb) < 1.0e-5)
                        continue;

                    computeFilterCascadeCoeffs (bp, sr, gainScale, 0.0, chain.coeffs, chain.activeSections);
                    const int numSec = chain.activeSections;

                    for (int sec = 0; sec < numSec; ++sec)
                    {
                        const auto& c = chain.coeffs[static_cast<size_t> (sec)];
                        auto& st      = chain.states[static_cast<size_t> (sec)];

                        if (rightChannel == nullptr || bp.stereoMode == 0)
                        {
                            for (int s = 0; s < numSamples; ++s)
                            {
                                const double xL = static_cast<double> (leftChannel[s]);
                                const double yL = c.b0 * xL + st.z1L;
                                st.z1L = c.b1 * xL - c.a1 * yL + st.z2L;
                                st.z2L = c.b2 * xL - c.a2 * yL;
                                leftChannel[s] = static_cast<float> (yL);

                                if (rightChannel != nullptr)
                                {
                                    const double xR = static_cast<double> (rightChannel[s]);
                                    const double yR = c.b0 * xR + st.z1R;
                                    st.z1R = c.b1 * xR - c.a1 * yR + st.z2R;
                                    st.z2R = c.b2 * xR - c.a2 * yR;
                                    rightChannel[s] = static_cast<float> (yR);
                                }
                            }
                        }
                        else if (bp.stereoMode == 1)
                        {
                            for (int s = 0; s < numSamples; ++s)
                            {
                                const double xL = static_cast<double> (leftChannel[s]);
                                const double yL = c.b0 * xL + st.z1L;
                                st.z1L = c.b1 * xL - c.a1 * yL + st.z2L;
                                st.z2L = c.b2 * xL - c.a2 * yL;
                                leftChannel[s] = static_cast<float> (yL);
                            }
                        }
                        else if (bp.stereoMode == 2)
                        {
                            for (int s = 0; s < numSamples; ++s)
                            {
                                const double xR = static_cast<double> (rightChannel[s]);
                                const double yR = c.b0 * xR + st.z1R;
                                st.z1R = c.b1 * xR - c.a1 * yR + st.z2R;
                                st.z2R = c.b2 * xR - c.a2 * yR;
                                rightChannel[s] = static_cast<float> (yR);
                            }
                        }
                        else if (bp.stereoMode == 3)
                        {
                            for (int s = 0; s < numSamples; ++s)
                            {
                                const double l = static_cast<double> (leftChannel[s]);
                                const double r = static_cast<double> (rightChannel[s]);
                                const double mid  = 0.5 * (l + r);
                                const double side = 0.5 * (l - r);

                                const double yMid = c.b0 * mid + st.z1L;
                                st.z1L = c.b1 * mid - c.a1 * yMid + st.z2L;
                                st.z2L = c.b2 * mid - c.a2 * yMid;

                                leftChannel[s]  = static_cast<float> (yMid + side);
                                rightChannel[s] = static_cast<float> (yMid - side);
                            }
                        }
                        else if (bp.stereoMode == 4)
                        {
                            for (int s = 0; s < numSamples; ++s)
                            {
                                const double l = static_cast<double> (leftChannel[s]);
                                const double r = static_cast<double> (rightChannel[s]);
                                const double mid  = 0.5 * (l + r);
                                const double side = 0.5 * (l - r);

                                const double ySide = c.b0 * side + st.z1R;
                                st.z1R = c.b1 * side - c.a1 * ySide + st.z2R;
                                st.z2R = c.b2 * side - c.a2 * ySide;

                                leftChannel[s]  = static_cast<float> (mid + ySide);
                                rightChannel[s] = static_cast<float> (mid - ySide);
                            }
                        }
                    }
                }
            }
        }

        // Auto-Gain inteligente com ponderação estática + dinâmica (Manual Pro-Q 3 p. 1 & p. 29)
        double autoMakeupDb = 0.0;
        if (autoGain)
        {
            for (int b = 0; b < kMaxBands; ++b)
            {
                const BandParameters bp = getBandParameters (b);
                if (bp.used && bp.enabled && doesShapeSupportGain (bp.shape))
                {
                    const double dynMod = static_cast<double> (currentDynamicGainDb[static_cast<size_t> (b)].load (std::memory_order_relaxed));
                    autoMakeupDb -= (bp.gainDb * gainScale + 0.5 * dynMod) * 0.25 / std::sqrt (juce::jmax (0.25, bp.q));
                }
            }
            autoMakeupDb = juce::jlimit (-18.0, 18.0, autoMakeupDb);
        }

        float linearGain = juce::Decibels::decibelsToGain (outGainDb + static_cast<float> (autoMakeupDb));
        if (phaseInvert)
            linearGain = -linearGain;

        const float balance = apvts.getRawParameterValue ("global_output_balance")->load (std::memory_order_relaxed) * 0.01f;
        if (rightChannel != nullptr && std::abs (balance) > 1.0e-5f)
        {
            const float firstGain = linearGain * (1.0f - juce::jmax (0.0f, balance));
            const float secondGain = linearGain * (1.0f + juce::jmin (0.0f, balance));
            const bool midSide = apvts.getRawParameterValue ("global_output_balance_mode")->load (std::memory_order_relaxed) > 0.5f;
            if (midSide)
            {
                for (int sample = 0; sample < numSamples; ++sample)
                {
                    const float l = leftChannel[sample], r = rightChannel[sample];
                    const float mid = 0.5f * (l + r) * firstGain;
                    const float side = 0.5f * (l - r) * secondGain;
                    leftChannel[sample] = mid + side;
                    rightChannel[sample] = mid - side;
                }
            }
            else
            {
                mainBus.applyGain (0, 0, numSamples, firstGain);
                mainBus.applyGain (1, 0, numSamples, secondGain);
            }
        }
        else if (std::abs (linearGain - 1.0f) > 1.0e-5f)
        {
            mainBus.applyGain (linearGain);
        }
    }

    float peakL = 0.0f;
    float peakR = 0.0f;

    for (int s = 0; s < numSamples; ++s)
    {
        const float l = leftChannel[s];
        const float r = (rightChannel != nullptr) ? rightChannel[s] : l;

        peakL = juce::jmax (peakL, std::abs (l));
        peakR = juce::jmax (peakR, std::abs (r));

        postFftFifo.push (l, r);
    }

    updateOutputMeter (peakL, peakR, numSamples, sr);
}

void ProQ3AudioProcessor::updateOutputMeter (float leftPeak, float rightPeak, int samples, double sampleRate) noexcept
{
    const auto elapsed = static_cast<float> (samples / sampleRate);
    const std::array<float, 2> peakGain { leftPeak, rightPeak };
    const std::array<std::atomic<float>*, 2> levels { &outputPeakDbL, &outputPeakDbR };

    for (size_t channel = 0; channel < levels.size(); ++channel)
    {
        const auto peak = juce::Decibels::gainToDecibels (peakGain[channel], kMeterFloorDb);
        const auto level = juce::jmax (peak, levels[channel]->load (std::memory_order_relaxed) - 30.0f * elapsed);
        levels[channel]->store (level, std::memory_order_relaxed);

        const auto held = outputHeldPeakDb[channel].load (std::memory_order_relaxed);
        if (peak >= held)
        {
            outputHeldPeakDb[channel].store (peak, std::memory_order_relaxed);
            meterHoldSamples[channel] = juce::roundToInt (sampleRate * 1.5);
        }
        else
        {
            meterHoldSamples[channel] -= samples;
            if (meterHoldSamples[channel] <= 0)
            {
                const auto releaseTime = static_cast<float> (-meterHoldSamples[channel] / sampleRate);
                outputHeldPeakDb[channel].store (juce::jmax (level, held - 18.0f * releaseTime), std::memory_order_relaxed);
                meterHoldSamples[channel] = 0;
            }
        }
    }
}

float ProQ3AudioProcessor::spectrumTiltDb (double frequency) noexcept
{
    return static_cast<float> (4.5 * std::log2 (juce::jmax (20.0, frequency) / 1000.0));
}

bool ProQ3AudioProcessor::pullSpectrum (SpectrumCapture& source, std::vector<float>& destMagnitudesDb)
{
    using FrameState = SpectrumCapture::FrameState;
    auto published = FrameState::ready;
    if (! source.frameState.compare_exchange_strong (published, FrameState::reading, std::memory_order_acquire))
        return false;

    auto& transform = spectrumTransforms[source.publishedPlan];
    const auto size = transform.size;
    const auto bins = size / 2 + 1;
    destMagnitudesDb.assign (bins, 0.0f);
    for (const auto& channel : *source.frame)
    {
        std::copy_n (channel.begin(), size, fftWorkspace.begin());
        std::fill_n (fftWorkspace.begin() + static_cast<std::ptrdiff_t> (size), size, 0.0f);
        transform.window.multiplyWithWindowingTable (fftWorkspace.data(), size);
        transform.fft.performFrequencyOnlyForwardTransform (fftWorkspace.data(), true);
        const auto inverseSize = 1.0f / static_cast<float> (size);
        for (size_t bin = 0; bin < bins; ++bin)
        {
            // The normalized Hann window has unity coherent gain.
            const auto scale = bin == 0 || bin == bins - 1 ? inverseSize : 2.0f * inverseSize;
            const auto amplitude = fftWorkspace[bin] * scale;
            destMagnitudesDb[bin] += 0.5f * amplitude * amplitude;
        }
    }
    source.frameState.store (FrameState::empty, std::memory_order_release);

    const auto binWidth = getCurrentSampleRate() / static_cast<double> (size);
    for (size_t bin = 0; bin < destMagnitudesDb.size(); ++bin)
    {
        const auto tiltDb = spectrumTiltDb (binWidth * static_cast<double> (bin));
        destMagnitudesDb[bin] = juce::Decibels::gainToDecibels (std::sqrt (destMagnitudesDb[bin]), -140.0f) + tiltDb;
    }
    return true;
}

bool ProQ3AudioProcessor::pullNextPreFftBlock (std::vector<float>& dest)
{
    return pullSpectrum (preFftFifo, dest);
}

bool ProQ3AudioProcessor::pullNextPostFftBlock (std::vector<float>& dest)
{
    return pullSpectrum (postFftFifo, dest);
}

bool ProQ3AudioProcessor::pullNextSidechainFftBlock (std::vector<float>& dest)
{
    return pullSpectrum (sidechainFftFifo, dest);
}

void ProQ3AudioProcessor::updateTrackProperties (const TrackProperties& properties)
{
    const juce::ScopedLock lock (getSpectrumPeerRegistry().mutex);
    spectrumTrackName = properties.name.trim();
}

juce::String ProQ3AudioProcessor::getSpectrumInstanceNameUnlocked() const
{
    if (spectrumTrackName.isNotEmpty())
        return spectrumTrackName;

    return spectrumInstanceAlias.isEmpty() ? juce::String ("NX Audio") : spectrumInstanceAlias;
}

juce::String ProQ3AudioProcessor::getSpectrumInstanceName() const
{
    const juce::ScopedLock lock (getSpectrumPeerRegistry().mutex);
    return getSpectrumInstanceNameUnlocked();
}

bool ProQ3AudioProcessor::hasSpectrumTrackName() const
{
    const juce::ScopedLock lock (getSpectrumPeerRegistry().mutex);
    return spectrumTrackName.isNotEmpty();
}

void ProQ3AudioProcessor::setSpectrumInstanceAlias (juce::String alias)
{
    alias = alias.trim().substring (0, 64);
    {
        const juce::ScopedLock lock (getSpectrumPeerRegistry().mutex);
        if (spectrumInstanceAlias == alias)
            return;
        spectrumInstanceAlias = std::move (alias);
    }
    updateHostDisplay (ChangeDetails {}.withNonParameterStateChanged (true));
}

std::vector<ProQ3AudioProcessor::SpectrumPeer> ProQ3AudioProcessor::getSpectrumPeers() const
{
    auto& registry = getSpectrumPeerRegistry();
    const juce::ScopedLock lock (registry.mutex);
    std::vector<SpectrumPeer> peers;
    for (auto* instance : registry.instances)
    {
        if (instance != this)
            peers.push_back ({ instance->spectrumPeerId, instance->getSpectrumInstanceNameUnlocked() });
    }

    for (auto& peer : peers)
    {
        const auto matchesName = [&peer] (const auto* instance)
        {
            return instance->getSpectrumInstanceNameUnlocked() == peer.name;
        };
        if (std::count_if (registry.instances.begin(), registry.instances.end(), matchesName) > 1)
            peer.name += " (" + juce::String (peer.id) + ")";
    }
    return peers;
}

void ProQ3AudioProcessor::refreshPostSpectrum()
{
    if (pullNextPostFftBlock (cachedPostSpectrum))
    {
        lastPostSpectrumMs = juce::Time::getMillisecondCounterHiRes();
        ++postSpectrumGeneration;
    }
}

bool ProQ3AudioProcessor::readPostSpectrum (juce::uint64& generation, std::vector<float>& magnitudes)
{
    const juce::ScopedLock lock (getSpectrumPeerRegistry().mutex);
    refreshPostSpectrum();
    if (generation == postSpectrumGeneration || juce::Time::getMillisecondCounterHiRes() - lastPostSpectrumMs > 200.0)
        return false;
    generation = postSpectrumGeneration;
    magnitudes = cachedPostSpectrum;
    return true;
}

bool ProQ3AudioProcessor::readPeerSpectrum (int peerId, std::vector<float>& magnitudes, double& rate)
{
    auto& registry = getSpectrumPeerRegistry();
    const juce::ScopedLock lock (registry.mutex);
    for (auto* instance : registry.instances)
    {
        if (instance == this || instance->spectrumPeerId != peerId)
            continue;
        instance->refreshPostSpectrum();
        if (instance->cachedPostSpectrum.empty() || juce::Time::getMillisecondCounterHiRes() - instance->lastPostSpectrumMs > 200.0)
            return false;
        magnitudes = instance->cachedPostSpectrum;
        rate = instance->getCurrentSampleRate();
        return true;
    }
    return false;
}

bool ProQ3AudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* ProQ3AudioProcessor::createEditor()
{
    return new ProQ3AudioProcessorEditor (*this);
}

void ProQ3AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    {
        const juce::ScopedLock lock (getSpectrumPeerRegistry().mutex);
        if (spectrumInstanceAlias.isNotEmpty())
            state.setProperty ("spectrumInstanceAlias", spectrumInstanceAlias, nullptr);
        else
            state.removeProperty ("spectrumInstanceAlias", nullptr);
    }
    state.setProperty ("selectedBandIndex", selectedBandIndex.load (std::memory_order_relaxed), nullptr);
    state.setProperty ("currentIsB", currentIsB, nullptr);
    state.setProperty ("midiOutputLevelCC", learnedControllers[0].load (std::memory_order_relaxed), nullptr);
    state.setProperty ("midiGainScaleCC", learnedControllers[1].load (std::memory_order_relaxed), nullptr);

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void ProQ3AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr)
    {
        if (xmlState->hasTagName (apvts.state.getType()))
        {
            auto newTree = juce::ValueTree::fromXml (*xmlState);
            {
                const juce::ScopedLock lock (getSpectrumPeerRegistry().mutex);
                spectrumInstanceAlias = newTree.getProperty ("spectrumInstanceAlias").toString().trim().substring (0, 64);
            }
            const std::array addedParameters { std::pair { "global_output_balance", 0.0f },
                                               std::pair { "global_output_balance_mode", 0.0f },
                                               std::pair { "analyzer_overlap", 1.0f } };
            for (const auto& [id, defaultValue] : addedParameters)
            {
                if (! newTree.getChildWithProperty ("id", id).isValid())
                {
                    juce::ValueTree parameter ("PARAM");
                    parameter.setProperty ("id", id, nullptr);
                    parameter.setProperty ("value", defaultValue, nullptr);
                    newTree.addChild (parameter, -1, nullptr);
                }
            }
            armedMidiTarget.store (-1, std::memory_order_relaxed);
            const auto loadController = [&newTree] (const char* id)
            {
                const int value = static_cast<int> (newTree.getProperty (id, -1));
                return juce::isPositiveAndBelow (value, 16 * 128) ? value : -1;
            };
            learnedControllers[0].store (loadController ("midiOutputLevelCC"), std::memory_order_relaxed);
            learnedControllers[1].store (loadController ("midiGainScaleCC"), std::memory_order_relaxed);
            if (newTree.getProperty (factoryPresetProperty).toString() == "Default")
                newTree.setProperty (factoryPresetProperty, "Default Setting", nullptr);
            setSelectedBandIndex (static_cast<int> (newTree.getProperty ("selectedBandIndex", -1)));
            currentIsB = static_cast<bool> (newTree.getProperty ("currentIsB", false));
            apvts.replaceState (newTree);
            stateA = apvts.copyState();
            stateB = apvts.copyState();
            updateAllFilterCoefficients();
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ProQ3AudioProcessor();
}
```

## Source/PluginEditor.h

```cpp
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include <array>
#include <functional>
#include <memory>
#include <vector>

class ProQ3LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ProQ3LookAndFeel();
    ~ProQ3LookAndFeel() override = default;

    void drawRotarySlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPosProportional,
                           float rotaryStartAngle,
                           float rotaryEndAngle,
                           juce::Slider& slider) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float position, float minPosition, float maxPosition,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;

    void drawButtonBackground (juce::Graphics& g,
                               juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics& g,
                       int width, int height,
                       bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox& box) override;

    void drawPopupMenuItem (juce::Graphics& g,
                            const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked,
                            bool hasSubMenu,
                            const juce::String& text,
                            const juce::String& shortcutKeyText,
                            const juce::Drawable* icon,
                            const juce::Colour* textColourToUse) override;
};

//==============================================================================
// Knob Concêntrico Duplo de GAIN + ANEL DE FAIXA DINÂMICA (Dynamic Range Ring)
// Centro: altera exclusivamente o Gain estático (-30 a +30 dB)
// Anel externo: área de arraste independente que altera exclusivamente o Dynamic Range (-30 a +30 dB)
// Exibe arco vermelho/âmbar para a faixa dinâmica configurada e arco amarelo vivo para a redução/expansão instantânea
//==============================================================================
class ConcentricGainDynamicsComponent final : public juce::Component
{
public:
    ConcentricGainDynamicsComponent();
    ~ConcentricGainDynamicsComponent() override = default;

    void bindParameters (juce::RangedAudioParameter* staticGainParam,
                         juce::RangedAudioParameter* dynamicRangeParam,
                         juce::UndoManager* undoMgr);

    void setLiveDynamicModulationDb (float modDb) noexcept;
    void setDynamicsActiveAndSupported (bool isSupported, bool isDynBypassed) noexcept;

    void paint (juce::Graphics& g) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    bool isHoveringOuterRing() const noexcept { return hoveringRing; }

private:
    bool hitTestOuterRing (juce::Point<float> pt) const noexcept;
    float getStaticGainDb() const noexcept;
    float getDynamicRangeDb() const noexcept;
    void setStaticGainDb (float newDb);
    void setDynamicRangeDb (float newDb);

    juce::RangedAudioParameter* gainParam = nullptr;
    juce::RangedAudioParameter* dynRangeParam = nullptr;
    juce::UndoManager* undoManager = nullptr;

    float liveDynamicModDb = 0.0f;
    bool shapeSupportsDynamics = true;
    bool dynamicsBypassed = false;

    bool hoveringRing = false;
    bool hoveringInner = false;
    bool draggingOuterRing = false;
    bool draggingInnerKnob = false;
    float dragStartY = 0.0f;
    float dragStartValueDb = 0.0f;
};

class ProQ3AudioProcessorEditor final : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    explicit ProQ3AudioProcessorEditor (ProQ3AudioProcessor&);
    ~ProQ3AudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseEnter (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseDoubleClick (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed (const juce::KeyPress& key) override;

    // Parser de entrada de texto com atalhos do manual Pro-Q 3 ("2k", "A4", "C#3 + 13", "50%")
    static double parseFrequencyTextInput (const juce::String& text, double currentFreqHz) noexcept;

private:
    struct PianoButton final : juce::Button
    {
        PianoButton();
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    };

    struct OutputPanel final : juce::Component
    {
        void paint (juce::Graphics&) override;
    };

    void timerCallback() override;
    void configureOutputControls();
    void updateOutputReadout();
    void showOutputPanel();
    void updateOutputPanelVisibility();
    void showOutputOptions();
    void configureAnalyzerPanel();
    void showAnalyzerMenu();
    void openAnalyzerPanel (bool pinned);
    void dismissAnalyzerPanel();
    void updateAnalyzerHover (double now);
    void refreshAnalyzerPanel();
    void layoutAnalyzerPanel();
    void selectAnalyzerSource (int sourceId);
    juce::String getAnalyzerSourceName() const;
    void updateExternalSpectrum (bool frozen);
    void updateSourcePreviews (double now, bool frozen);
    void updateSpectrumGrab (double now);
    double getAnalyzerTiltChange() const noexcept;
    static constexpr size_t analyzerPreviewPoints = 92;
    using AnalyzerPreview = std::array<float, analyzerPreviewPoints>;
    static double analyzerPreviewFrequency (double normalizedPosition);
    static AnalyzerPreview sampleAnalyzerProfile (const std::vector<float>&, double sampleRate);
    static AnalyzerPreview measureAnalyzerOverlaps (const AnalyzerPreview&, const AnalyzerPreview&);
    void configureMatchPanel();
    void openMatchPanel();
    void closeMatchPanel();
    void refreshMatchReferenceMenu();
    void refreshMatchControls();
    void updateMatchCapture (const std::vector<float>&, double sampleRate, bool sidechain);
    void buildMatchPreview();
    void applyMatchPreview();
    void saveMatchReference();
    void drawMatchSpectra (juce::Graphics&, juce::Rectangle<int>);
    static float sampleSpectrum (const std::vector<float>&, double frequency, double sampleRate);
    static double spectrumBandPower (const std::vector<float>&, double centre, double rate);
    bool grabSpectrumPeak (juce::Point<float> position);
    void layoutBandControls();
    bool isOverFrequencyStrip (juce::Point<int>) const noexcept;
    void updateFrequencyView (double anchor, double zoom, double position);
    void showMidiLearnMenu();
    void toggleExpandedView();
    void refreshStatusControls();
    void setParameterFromEditor (const char* id, float value);

    float frequencyToX (double freqHz) const noexcept;
    double xToFrequency (float x) const noexcept;
    float gainDbToY (double gainDb) const noexcept;
    double yToGainDb (float y) const noexcept;
    float analyzerDbToY (double analyzerDb) const noexcept;
    int getSpectrumDisplayRange() const noexcept;
    double getCurrentDisplayRangeDb() const noexcept;

    static juce::String frequencyToMusicalNoteName (double freqHz);
    static double snapFrequencyToNearestSemitone (double freqHz) noexcept;

    void attachFloatingControlsToBand (int bandIndex);
    void updateFloatingControlsVisibilityAndValues();
    void stepSelectedBand (int delta);
    void showFactoryPresetMenu();
    void stepFactoryPreset (int delta);
    void loadFactoryPreset (int index);
    void refreshFactoryPresetName();

    bool startNewCurveBandDrag (juce::Point<float> position);
    int findBandNodeAtPosition (juce::Point<float> pos, float maxRadiusPx = 14.0f) const noexcept;

    void drawTopToolbar (juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawEqGridAndScales (juce::Graphics& g, juce::Rectangle<int> area);
    void drawSpectrumAnalyzer (juce::Graphics& g, juce::Rectangle<int> area);
    void updateSpectrumPeaks (double now);
    void drawSpectrumPeaks (juce::Graphics& g);
    void drawEqCurvesAndBandNodes (juce::Graphics& g, juce::Rectangle<int> area);
    juce::Rectangle<int> getBandTooltipBounds (const ProQ3AudioProcessor::BandParameters&) const noexcept;
    bool isOverBandTooltip (juce::Point<int> position) const noexcept;
    void updateBandDynamicsShortcut();
    void drawBandParameterTooltip (juce::Graphics& g, int bandIndex);
    void drawPianoRollBar (juce::Graphics& g, juce::Rectangle<int> area);
    void drawFloatingBandControlsBackground (juce::Graphics& g);
    void drawOutputMeter (juce::Graphics& g, juce::Rectangle<int> meterBounds);
    void drawBottomToolbar (juce::Graphics& g, juce::Rectangle<int> bounds);

    ProQ3AudioProcessor& audioProcessor;
    ProQ3LookAndFeel lookAndFeel;

    juce::Rectangle<int> topBarBounds;
    juce::Rectangle<int> graphBounds;
    juce::Rectangle<int> pianoBounds;
    juce::Rectangle<int> meterBounds;
    juce::Rectangle<int> bottomBarBounds;
    juce::Rectangle<int> floatingPillBounds;
    juce::Rectangle<int> manualThreshMeterBounds;

    // Top Bar
    juce::TextButton undoButton  { "Undo" };
    juce::TextButton redoButton  { "Redo" };
    juce::TextButton abButton    { "A/B" };
    juce::TextButton copyButton  { "Copy" };
    juce::TextButton prevPresetButton { "<" };
    juce::TextButton presetNameButton { "Default" };
    juce::TextButton nextPresetButton { ">" };
    juce::TextButton helpButton  { "Help" };
    juce::TextButton expandButton;
    juce::Rectangle<int> compactEditorBounds;

    // Floating Band Controls Pill (com crista superior para o anel dinâmico, botão Auto, Bypass Dynamics e Clear Dynamics)
    juce::ToggleButton bandBypassButton { "" };
    juce::ComboBox     bandShapeBox;
    juce::ComboBox     bandSlopeBox;

    juce::Slider       freqSlider;
    ConcentricGainDynamicsComponent concentricGainKnob;
    juce::Slider       qSlider;
    juce::ToggleButton gainQInterButton { "G-Q" };

    // Controles específicos do anel dinâmico do GAIN (Manual Pro-Q 3 p. 13-14 + imagem de referência)
    juce::TextButton   clearDynamicsButton  { "x" };    // 'x' à esquerda do arco: zera dyn_range sem excluir a banda
    juce::ToggleButton bypassDynamicsButton { "D-On" }; // Power junto ao anel: suspende somente a dinâmica
    juce::ToggleButton autoThresholdButton  { "Auto" }; // Topo do arco: alterna entre Auto e Manual Threshold
    juce::Label        manualThresholdValue;
    juce::Slider       manualThresholdSlider;           // Slider de limiar manual em dB (-60 a 0 dB) visível quando Auto está desligado
    juce::ToggleButton externalSidechainButton { "SC" };// Botão External Side Chain visível no modo manual

    juce::Label        freqTitleLabel;
    juce::Label        gainTitleLabel;
    juce::Label        qTitleLabel;
    juce::Label        freqRangeHintLabel;
    juce::Label        gainRangeHintLabel;
    juce::Label        qRangeHintLabel;

    juce::TextButton   prevBandButton   { "<" };
    juce::Label        bandNumberLabel;
    juce::TextButton   nextBandButton   { ">" };
    juce::TextButton   deleteBandButton { "X" }; // 'X' no canto superior direito: exclui a banda
    juce::ComboBox     stereoModeBox;
    juce::TextButton   soloBandButton   { "Solo" };
    juce::TextButton   splitStereoButton { "Split" };

    // Attachments dinâmicos do APVTS para a banda selecionada
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   bandEnabledAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> bandShapeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> bandSlopeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   freqAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   qAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   gainQInterAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   bypassDynamicsAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   autoThresholdAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   manualThresholdAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   externalSidechainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> stereoModeAttachment;

    // Bottom Bar
    juce::ComboBox     processingModeBox;
    juce::TextButton   analyzerButton { "Analyzer: Pre+Post" };
    juce::TextButton   midiLearnButton { "MIDI Learn" };
    PianoButton        pianoToggleButton;
    juce::TextButton  displayRangeButton { "12 dB" };
    juce::TextButton   outputControlButton { "100%  0.0 dB" };
    OutputPanel        outputPanel;
    juce::Label        outputLevelLabel    { {}, "Output Level" };
    juce::Label        outputScaleLabel    { {}, "Gain Scale" };
    juce::Label        outputBalanceLabel  { {}, "Balance" };
    juce::Slider       outputGainSlider;
    juce::Slider       outputScaleSlider;
    juce::Slider       outputBalanceSlider;
    juce::ComboBox     outputBalanceModeBox;
    juce::TextButton   outputPolarityButton { "Ø" };
    juce::TextButton   outputAutoGainButton { "A" };
    juce::TextButton   outputOptionsButton  { "..." };
    double outputPanelLastInteraction = 0.0;
    bool outputPanelKeyboardAccess = false;
    bool outputOptionsOpen = false;
    juce::ToggleButton globalBypassButton   { "Bypass" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> processingModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   pianoDisplayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   outputGainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   outputScaleAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   outputBalanceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> outputBalanceModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   outputPolarityAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   outputAutoGainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   globalBypassAttachment;

    juce::DrawableButton bandDynamicsShortcut { "Open Dynamic EQ", juce::DrawableButton::ImageFitted };
    struct AnalyzerBackdrop final : juce::Component
    {
        std::function<void()> dismiss;
        void mouseDown (const juce::MouseEvent&) override { if (dismiss) dismiss(); }
    };
    struct AnalyzerToolButton final : juce::Button
    {
        enum class Icon { freeze, grab, overlap, settings };
        AnalyzerToolButton (const char* name, Icon icon);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        const Icon icon;
    };

    struct AnalyzerPanel final : juce::Component, private juce::ListBoxModel
    {
        static constexpr int rowHeight = 36;

        struct Source
        {
            int id;
            juce::String name;
            AnalyzerPreview levels {};
            AnalyzerPreview overlap {};
            bool hasTrace = false;
        };

        struct SettingsPane final : juce::Component
        {
            void paint (juce::Graphics&) override;
        };

        AnalyzerPanel();
        ~AnalyzerPanel() override;
        void paint (juce::Graphics&) override;
        void resized() override;
        void setSelectedSource (int id);
        void setSources (const std::vector<ProQ3AudioProcessor::SpectrumPeer>&);
        int preferredHeight() const noexcept;
        bool sourceIsVisible (int row) const;
        bool hasOpenCombo() const;

        std::function<void (int)> onSourceChanged;
        std::vector<Source> sources;
        juce::ListBox sourceList;
        juce::Label instance;
        juce::TextButton pre { "Pre" }, post { "Post" }, peaks { "Peak Alerts" };
        AnalyzerToolButton freeze { "Freeze", AnalyzerToolButton::Icon::freeze };
        AnalyzerToolButton grab { "Spectrum Grab", AnalyzerToolButton::Icon::grab };
        AnalyzerToolButton overlaps { "Spectrum Overlap", AnalyzerToolButton::Icon::overlap };
        AnalyzerToolButton settingsButton { "Analyzer Settings", AnalyzerToolButton::Icon::settings };
        juce::TextButton match { "EQ Match" };
        SettingsPane settings;
        juce::ComboBox speed, tilt, range, detail, resolution;

    private:
        int getNumRows() override;
        juce::String getNameForRow (int) override;
        void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
        void selectedRowsChanged (int row) override;
        bool syncingSelection = false;
    };
    AnalyzerBackdrop analyzerBackdrop;
    AnalyzerPanel analyzerPanel;
    bool analyzerPinned = false;
    bool analyzerHoverSuppressed = false;
    double analyzerHoverSince = 0.0;
    double analyzerLastInteraction = 0.0;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>, 5> analyzerAttachments;
    std::vector<ProQ3AudioProcessor::SpectrumPeer> analyzerPeers;
    int analyzerExternalId = 0;
    juce::uint64 seenPostSpectrum = 0;
    double externalSampleRate = 0.0;
    double lastExternalRefreshMs = 0.0;
    double lastAnalyzerRefreshMs = 0.0;
    double lastPreviewRefreshMs = 0.0;
    double spectrumGrabHoverSince = 0.0;
    juce::Point<int> spectrumGrabHoverAnchor;
    juce::Point<int> spectrumGrabPointer;
    bool spectrumGrabPointerInGraph = false;
    bool spectrumGrabActive = false;
    bool draggingSpectrumPeak = false;
    std::vector<float> externalSpectrumDb;
    AnalyzerPreview activeSpectrumOverlap {};
    static constexpr std::array<double, 18> matchFrequencies {
        45, 63, 90, 125, 180, 250, 355, 500, 710, 1000, 1400, 2000, 2800, 4000, 5600, 8000, 11300, 16000
    };
    using MatchPowers = std::array<double, matchFrequencies.size()>;
    struct MatchCapture
    {
        MatchPowers power {};
        int frames = 0;
        bool recording = false;
        bool ready() const noexcept { return frames >= 5; }
    };
    struct MatchReference
    {
        juce::String name;
        MatchPowers power {};
    };
    struct MatchPanel final : juce::Component
    {
        void paint (juce::Graphics&) override;
        juce::TextButton input { "Record Input" }, resetInput { "Reset Input" }, recordReference { "Record Reference" };
        juce::TextButton preview { "Match" }, finish { "Apply Match" }, back { "Capture" }, close { "Close" }, save { "Save" };
        juce::ComboBox reference;
        juce::Label status;
        juce::TextEditor referenceName;
        juce::Slider bandCount;
    };
    static juce::File getMatchLibraryFile();
    static std::vector<MatchReference> loadMatchLibrary();

    MatchPanel matchPanel;
    MatchCapture matchInput, matchReference;
    std::vector<MatchReference> savedMatchReferences;
    std::vector<ProQ3AudioProcessor::BandParameters> matchPreview;
    int selectedMatchSource = 0;
    int matchExternalId = 0;
    bool showingMatchPreview = false;
    bool savingMatchReference = false;
    int expandedDynamicsBand = -1;
    int attachedBandIndex = -1;
    int hoveredBandIndex  = -1;
    int draggingBandIndex = -1;
    bool draggingFrequencyView = false;
    double frequencyViewStart = 0.0;
    double frequencyViewSpan = 1.0;
    double dragStartViewAnchor = 0.0;
    double dragStartViewSpan = 1.0;
    bool waitingForEdgeDrag = false;
    bool isDraggingNode   = false;
    bool isDraggingDynHandle = false;
    bool isSelectionRectDragging = false;
    juce::Point<float> dragStartMousePos;
    juce::Rectangle<float> selectionLassoRect;
    double dragStartFreqHz = 1000.0;
    double dragStartGainDb = 0.0;
    double dragStartDynRangeDb = 0.0;
    double dragStartQ      = 1.0;
    juce::Point<float> lastMousePos { -1.0f, -1.0f };
    bool showHelpOverlay = false;

    enum class SpectrumSource { none, pre, post, sidechain };
    SpectrumSource peakSource = SpectrumSource::none;
    double peakSampleRate = 0.0;
    size_t peakSpectrumBins = 0;

    static constexpr double peakConfirmationMs = 110.0;

    struct SpectrumPeak
    {
        double frequency = 0.0;
        float level = 0.0f;
        double firstSeen = 0.0;
        double lastSeen = 0.0;
    };
    std::array<SpectrumPeak, 5> spectrumPeaks {};
    std::array<juce::Rectangle<float>, 5> peakTags {};
    std::array<double, 5> peakTagFrequencies {};
    std::vector<float> rawSpectrumDb;
    double lastSpectrumFrameMs = 0.0;
    double spectrumIdleMs = 0.0;
    std::vector<float> smoothedPreSpectrumDb;
    std::vector<float> smoothedPostSpectrumDb;
    std::vector<float> smoothedSidechainSpectrumDb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProQ3AudioProcessorEditor)
};
```

## Source/PluginEditor.cpp

```cpp
#include "PluginEditor.h"
#include <cmath>
#include <map>

namespace
{
    const juce::Colour kBgDark           { 0xff232328 };
    const juce::Colour kGridLineMajor    { 0xff28282d };
    const juce::Colour kGridLineMinor    { 0xff232327 };
    const juce::Colour kGridZeroLine     { 0xffe5a920 };
    const juce::Colour kTextMuted        { 0xff7e7e86 };
    const juce::Colour kTextBright       { 0xffd8d8dc };
    const juce::Colour kControlAccent    { 0xff7de1d2 };
    const juce::Colour kEqGold           { 0xfff5b82e };
    const juce::Colour kEqGlow       { 0x20f5b82e };
    const juce::Colour kFloatingPillBg   { 0xb8242630 };
    const juce::Colour kFloatingBorder   { 0xff4a5c68 };
    const juce::Colour kBypassRed        { 0xffd94343 };
    const juce::Colour kDynRedArc        { 0xffc8282e };
    const juce::Colour kDynYellowLive    { 0xffffc626 };

    juce::Colour getBandColour (int bandIndex) noexcept
    {
        static constexpr std::array<juce::uint32, ProQ3AudioProcessor::kMaxBands> palette {
            0xffff667e, 0xff4aa3ff, 0xff66d936, 0xffc850e8, 0xffff83c1, 0xff55d9d8,
            0xffed9081, 0xff8da5ff, 0xffa5d58f, 0xffdd8dec, 0xff68c7a7, 0xff9bc5ec,
            0xffdd768e, 0xff5da4d5, 0xff8bd9c1, 0xffbba4ed, 0xffe3a0c1, 0xff83d7f1,
            0xfff5a6a0, 0xff7893e1, 0xff7fba91, 0xffca88d4, 0xff55afad, 0xffb2c9eb
        };
        return juce::Colour (palette[static_cast<size_t> (bandIndex)]);
    }

    constexpr double kMinFreqHz = 10.0;
    constexpr double kMaxFreqHz = 30000.0;
    constexpr double kPeakHoldMs = 240.0;
    const juce::Identifier spectrumControl { "spectrumControl" };
    const auto kExternalTrace = juce::Colour (0xffba91dc);
    const auto kSpectrumOverlapColour = juce::Colour (0xffee2e41);

    int graphEdgeFilterShape (float x, juce::Rectangle<int> graph) noexcept
    {
        const auto edgeWidth = static_cast<float> (graph.getWidth()) * 0.15f;
        if (x < graph.getX() + edgeWidth)
            return ProQ3AudioProcessor::kShapeLowCut;
        if (x >= graph.getRight() - edgeWidth)
            return ProQ3AudioProcessor::kShapeHighCut;
        return ProQ3AudioProcessor::kShapeBell;
    }

    constexpr float  kRotaryStartAngle = juce::MathConstants<float>::pi * 1.25f;
    constexpr float  kRotaryEndAngle   = juce::MathConstants<float>::pi * 2.75f;
}

float ProQ3AudioProcessorEditor::sampleSpectrum (const std::vector<float>& spectrum, double frequency, double sampleRate)
{
    if (spectrum.size() < 2 || sampleRate <= 0.0 || frequency > sampleRate * 0.5)
        return -160.0f;
    const auto last = static_cast<int> (spectrum.size()) - 1;
    const auto bin = juce::jlimit (0.0, static_cast<double> (last),
                                 frequency * (2.0 * last) / sampleRate);
    const auto low = juce::jmin (last - 1, static_cast<int> (bin));
    const auto a = spectrum[static_cast<size_t> (low)];
    const auto b = spectrum[static_cast<size_t> (low + 1)];
    return a + static_cast<float> (bin - low) * (b - a);
}


//==============================================================================
// LookAndFeel customizado estilo FabFilter Pro-Q 3 (Fiel à imagem de referência)
//==============================================================================
ProQ3LookAndFeel::ProQ3LookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, kTextBright);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff2c2f38));
    setColour (juce::ComboBox::textColourId, kTextBright);
    setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff434754));
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1f2229));
    setColour (juce::PopupMenu::textColourId, kTextBright);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff363c48));
}

void ProQ3LookAndFeel::drawRotarySlider (juce::Graphics& g,
                                         int x, int y, int width, int height,
                                         float sliderPos,
                                         float rotaryStartAngle,
                                         float rotaryEndAngle,
                                         juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float rx = centre.x - radius;
    const float ry = centre.y - radius;
    const float rw = radius * 2.0f;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Sombra circular externa do knob
    g.setColour (juce::Colour (0xff15161a));
    g.fillEllipse (rx, ry, rw, rw);

    const bool outputKnob = slider.getComponentID() == "output-level";
    const bool outputActive = outputKnob && slider.isMouseOverOrDragging();
    if (outputKnob)
    {
        const auto ringRadius = radius - 0.75f;
        juce::Path rim;
        rim.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (juce::Colour (0xff514859));
        g.strokePath (rim, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (angle - rotaryStartAngle > 0.01f)
        {
            juce::Path position;
            position.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, rotaryStartAngle, angle, true);
            g.setColour (kEqGold.withAlpha (outputActive ? 0.85f : 0.43f));
            g.strokePath (position, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }
    juce::ColourGradient knobGrad (juce::Colour (outputActive ? 0xfff1c765 : 0xff565963), centre.x, ry,
                                   juce::Colour (outputActive ? 0xffad761b : 0xff34373e), centre.x, ry + rw, false);
    g.setGradientFill (knobGrad);
    g.fillEllipse (rx + 2.5f, ry + 2.5f, rw - 5.0f, rw - 5.0f);

    g.setColour (juce::Colour (0xff676b76));
    g.drawEllipse (rx + 2.5f, ry + 2.5f, rw - 5.0f, rw - 5.0f, 1.0f);

    // Chanfro claro em gota ao redor do ponteiro (característico do knob do Pro-Q 3)
    juce::Path capWedge;
    const float capRadius = radius - 3.0f;
    capWedge.addPieSegment (centre.x - capRadius, centre.y - capRadius,
                            capRadius * 2.0f, capRadius * 2.0f,
                            angle - 0.32f, angle + 0.32f, 0.25f);
    g.setColour (slider.isEnabled() ? juce::Colour (0xffeeedeb) : juce::Colour (0xff6d707a));
    g.fillPath (capWedge);

    // Traço escuro central do ponteiro dentro da gota clara
    juce::Path pointerLine;
    pointerLine.addRoundedRectangle (-0.9f, -capRadius + 3.5f, 1.8f, capRadius * 0.42f, 0.8f);
    pointerLine.applyTransform (juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
    g.setColour (juce::Colour (0xff23252b));
    g.fillPath (pointerLine);
}

void ProQ3LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                         float position, float minPosition, float maxPosition,
                                         juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (slider.getComponentID() == "output-balance")
    {
        const auto cy = static_cast<float> (y) + static_cast<float> (height) * 0.5f;
        const auto left = static_cast<float> (x);
        const auto right = static_cast<float> (x + width);
        const auto middle = (left + right) * 0.5f;
        g.setColour (juce::Colour (0xc6191822));
        g.fillRoundedRectangle (left, cy - 3.5f, static_cast<float> (width), 7.0f, 3.0f);
        g.setColour (juce::Colour (0x997dacbb));
        g.fillRoundedRectangle (juce::jmin (middle, position), cy - 2.0f, std::abs (position - middle), 4.0f, 2.0f);
        g.setColour (juce::Colour (0xffa89fb1));
        g.drawLine (middle, cy - 5.0f, middle, cy + 5.0f, 1.0f);
        g.setColour (slider.isMouseOverOrDragging() ? kControlAccent : kTextBright);
        g.fillEllipse (position - 3.5f, cy - 3.5f, 7.0f, 7.0f);
        return;
    }
    if (slider.getComponentID() != "dynamic-threshold")
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, position, minPosition, maxPosition, style, slider);
        return;
    }

    const auto centre = static_cast<float> (x) + static_cast<float> (width) * 0.5f;
    const juce::Rectangle<float> thumb (centre - 9.0f, position - 3.5f, 18.0f, 7.0f);
    g.setColour (slider.isMouseOverOrDragging() ? kEqGold : juce::Colour (0xffdedbea));
    g.fillRoundedRectangle (thumb, 2.5f);
    g.setColour (juce::Colour (0xff1f202a));
    g.drawRoundedRectangle (thumb, 2.5f, 1.0f);
}

void ProQ3LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down)
{
    const auto id = button.getComponentID();
    const bool power = id == "eq-band-power" || id == "eq-dynamics-power";
    if (! power && id != "eq-gain-q")
    {
        juce::LookAndFeel_V4::drawToggleButton (g, button, highlighted, down);
        return;
    }

    const auto bounds = button.getLocalBounds().toFloat().reduced (0.75f);
    if (highlighted || down)
    {
        g.setColour (juce::Colour (down ? 0xff171821 : 0xff3b3845));
        g.fillRoundedRectangle (bounds, 4.0f);
    }
    const auto centre = bounds.getCentre();
    const auto colour = button.getToggleState() ? kControlAccent : kTextMuted;
    g.setColour (button.isEnabled() ? colour : colour.withMultipliedAlpha (0.4f));

    juce::Path symbol;
    if (power)
    {
        symbol.addCentredArc (centre.x, centre.y + 0.5f, 4.3f, 4.3f, 0.0f, 0.67f,
                              juce::MathConstants<float>::twoPi - 0.67f, true);
        symbol.startNewSubPath (centre.x, centre.y - 6.0f);
        symbol.lineTo (centre.x, centre.y - 0.4f);
    }
    else
    {
        symbol.addEllipse (centre.x - 2.0f, centre.y - 2.0f, 4.0f, 4.0f);
        for (int tooth = 0; tooth < 8; ++tooth)
        {
            const auto angle = juce::MathConstants<float>::pi * static_cast<float> (tooth) / 4.0f;
            const juce::Point<float> direction (std::sin (angle), std::cos (angle));
            symbol.startNewSubPath (centre + direction * 3.2f);
            symbol.lineTo (centre + direction * 5.5f);
        }
    }
    g.strokePath (symbol, juce::PathStrokeType (1.25f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void ProQ3LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool down)
{
    if (button.getComponentID() != "analyzer-settings")
    {
        juce::LookAndFeel_V4::drawButtonText (g, button, highlighted, down);
        return;
    }

    auto area = button.getLocalBounds().reduced (3, 2);
    const auto prefix = area.removeFromLeft (48);
    g.setFont (juce::FontOptions (11.1f));
    g.setColour (kTextMuted);
    g.drawText ("Analyzer", prefix, juce::Justification::centredLeft);
    const auto capsule = area.reduced (1, 0).toFloat();
    const bool open = button.getToggleState();
    g.setColour (open || highlighted ? juce::Colour (0xbf4a3d50) : juce::Colour (0x88352e3b));
    g.fillRoundedRectangle (capsule, 4.0f);
    g.setColour (open ? kEqGold.withAlpha (0.54f) : juce::Colour (0x995c4c64));
    g.drawRoundedRectangle (capsule.reduced (0.5f), 4.0f, 1.0f);
    g.setColour (open ? kEqGold : kTextBright);
    const auto value = button.getButtonText().fromFirstOccurrenceOf (": ", false, false);
    g.drawFittedText (value, area.reduced (5, 0), juce::Justification::centred, 1);
}

void ProQ3LookAndFeel::drawButtonBackground (juce::Graphics& g,
                                             juce::Button& button,
                                             const juce::Colour& /*backgroundColour*/,
                                             bool isHighlighted,
                                             bool isDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const bool toggled = button.getToggleState();
    const juce::String txt = button.getButtonText();
    const bool isBypassBtn = (txt == "Bypass");
    const bool isAutoBtn   = (txt == "Auto");

    if (button.getProperties().contains ("toolbar"))
    {
        if (isHighlighted || isDown || button.hasKeyboardFocus (false))
        {
            g.setColour (juce::Colour (isDown ? 0xff302a32 : 0xff2b2630));
            g.fillRoundedRectangle (bounds, 4.0f);
        }
        if (button.getComponentID() == "midi-learn")
        {
            g.setColour (toggled ? kEqGold : kTextMuted.withAlpha (0.65f));
            g.fillEllipse (5.0f, bounds.getCentreY() - 2.0f, 4.0f, 4.0f);
        }
        if (button.getComponentID() == "undo-edit" || button.getComponentID() == "redo-edit")
        {
            const auto centre = bounds.getCentre();
            const float direction = button.getComponentID() == "undo-edit" ? 1.0f : -1.0f;
            const float left = centre.x - 5.0f * direction;
            juce::Path arrow;
            arrow.startNewSubPath (centre.x + 5.0f * direction, centre.y + 2.5f);
            arrow.quadraticTo (centre.x + 4.0f * direction, centre.y - 4.0f, left, centre.y - 0.5f);
            arrow.startNewSubPath (left + 2.5f * direction, centre.y - 4.0f);
            arrow.lineTo (left, centre.y - 0.5f);
            arrow.lineTo (left + 4.0f * direction, centre.y + 1.0f);
            g.setColour (button.isEnabled() ? kTextBright : kTextMuted.withAlpha (0.5f));
            g.strokePath (arrow, juce::PathStrokeType (1.25f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        if (button.getComponentID() == "expand-editor")
        {
            const auto icon = bounds.withSizeKeepingCentre (11.0f, 11.0f);
            constexpr float arm = 3.6f;
            juce::Path corners;
            for (const float dx : { -1.0f, 1.0f })
            {
                for (const float dy : { -1.0f, 1.0f })
                {
                    const float x = icon.getCentreX() + dx * icon.getWidth() * 0.5f;
                    const float y = icon.getCentreY() + dy * icon.getHeight() * 0.5f;
                    corners.startNewSubPath (x - dx * arm, y);
                    corners.lineTo (x, y);
                    corners.lineTo (x, y - dy * arm);
                }
            }
            g.setColour (kTextBright);
            g.strokePath (corners, juce::PathStrokeType (1.1f));
        }
        return;
    }

    const auto outputId = button.getComponentID();
    if (outputId == "output-polarity" || outputId == "output-auto-gain" || outputId == "output-options")
    {
        const auto accent = outputId == "output-auto-gain" ? kEqGold : kControlAccent;
        g.setColour (toggled ? accent.withAlpha (0.25f) : juce::Colour (isHighlighted || isDown ? 0xb73d3546 : 0x8f292631));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (toggled ? accent.withAlpha (0.75f) : juce::Colour (0xa16b6074));
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
        const auto c = bounds.getCentre();
        g.setColour (toggled ? accent : kTextBright);
        if (outputId == "output-polarity")
        {
            g.drawEllipse (c.x - 4.4f, c.y - 4.4f, 8.8f, 8.8f, 1.3f);
            g.drawLine (c.x - 5.3f, c.y + 5.3f, c.x + 5.3f, c.y - 5.3f, 1.3f);
        }
        else if (outputId == "output-options")
        {
            for (const float dy : { -3.5f, 0.0f, 3.5f })
                g.drawLine (c.x - 5.0f, c.y + dy, c.x + 5.0f, c.y + dy, 1.15f);
        }
        return;
    }

    if (button.getProperties().contains (spectrumControl))
    {
        g.setColour (toggled ? juce::Colour (0xa3483c57)
                             : isDown ? juce::Colour (0xbb1c1922) : juce::Colour (isHighlighted ? 0xb13c3546 : 0x78272131));
        g.fillRoundedRectangle (bounds, 5.0f);
        const auto accent = button.getProperties().contains ("spectrumFeed") ? kEqGold : kExternalTrace;
        g.setColour (toggled ? accent.withAlpha (0.70f) : juce::Colour (0x88534b61));
        g.drawRoundedRectangle (bounds, 5.0f, 1.0f);
        if (toggled)
            g.fillRoundedRectangle (bounds.reduced (4.0f, 6.0f).withWidth (2.0f), 1.0f);
        return;
    }

    juce::Colour bg = juce::Colour (0xff2b2e37);
    if (isAutoBtn)
        bg = toggled ? juce::Colour (0xff2f333e) : juce::Colour (0xff423820);
    else if (toggled)
        bg = isBypassBtn ? juce::Colour (0xff6e2222) : juce::Colour (0xff3c414f);
    else if (isDown)
        bg = juce::Colour (0xff1d2026);
    else if (isHighlighted)
        bg = juce::Colour (0xff353945);

    g.setColour (bg);
    g.fillRoundedRectangle (bounds, 4.0f);

    juce::Colour border = juce::Colour (0xff464b59);
    if (isAutoBtn)
        border = toggled ? juce::Colour (0xff585e70) : kControlAccent;
    else if (toggled)
        border = isBypassBtn ? kBypassRed : kControlAccent;
    else if (isHighlighted)
        border = juce::Colour (0xff646b7e);

    g.setColour (border);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
}

void ProQ3LookAndFeel::drawComboBox (juce::Graphics& g,
                                     int width, int height,
                                     bool /*isButtonDown*/,
                                     int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/,
                                     juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
    if (box.getProperties().contains ("toolbar"))
    {
        if (box.isMouseOver() || box.hasKeyboardFocus (true) || box.isPopupActive())
        {
            g.setColour (juce::Colour (0xff342e38));
            g.fillRoundedRectangle (bounds, 4.0f);
        }
    }
    else
    {
        g.setColour (box.getProperties().contains (spectrumControl)
                        ? juce::Colour (box.isMouseOver() ? 0xb13c3546 : 0x78272131)
                        : juce::Colour (box.isMouseOver() ? 0xff383c47 : 0xff2d3039));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (juce::Colour (0xff474c5a));
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
    }

    if (box.getComponentID() == "band-stereo-mode" && box.getSelectedId() == 1)
    {
        g.setColour (juce::Colour (0xffffb94b));
        g.fillEllipse (static_cast<float> (width) - 26.0f, bounds.getCentreY() - 2.0f, 4.0f, 4.0f);
    }

    juce::Path arrow;
    const float cx = static_cast<float> (width) - 10.0f;
    const float cy = static_cast<float> (height) * 0.5f;
    arrow.addTriangle (cx - 3.2f, cy - 1.4f, cx + 3.2f, cy - 1.4f, cx, cy + 2.6f);
    g.setColour (kTextMuted);
    g.fillPath (arrow);
}

void ProQ3LookAndFeel::drawPopupMenuItem (juce::Graphics& g,
                                          const juce::Rectangle<int>& area,
                                          bool isSeparator, bool isActive,
                                          bool isHighlighted, bool isTicked,
                                          bool hasSubMenu,
                                          const juce::String& text,
                                          const juce::String& /*shortcutKeyText*/,
                                          const juce::Drawable* /*icon*/,
                                          const juce::Colour* /*textColourToUse*/)
{
    if (isSeparator)
    {
        g.setColour (juce::Colour (0xff323640));
        g.fillRect (area.reduced (6, 0).withHeight (1));
        return;
    }

    if (isHighlighted && isActive)
    {
        g.setColour (juce::Colour (0xff343946));
        g.fillRoundedRectangle (area.reduced (2, 1).toFloat(), 3.0f);
    }

    g.setColour (isTicked ? kControlAccent : (isActive ? kTextBright : kTextMuted));
    g.setFont (juce::FontOptions (13.0f));
    auto textArea = area.reduced (10, 0);
    if (isTicked)
        g.drawText ("v", textArea.removeFromLeft (14), juce::Justification::centredLeft, false);
    if (hasSubMenu)
    {
        const auto arrow = textArea.removeFromRight (12).toFloat().getCentre();
        juce::Path chevron;
        chevron.startNewSubPath (arrow.x - 2.0f, arrow.y - 3.0f);
        chevron.lineTo (arrow.x + 1.5f, arrow.y);
        chevron.lineTo (arrow.x - 2.0f, arrow.y + 3.0f);
        g.strokePath (chevron, juce::PathStrokeType (1.2f));
    }
    g.drawText (text, textArea, juce::Justification::centredLeft, true);
}

//==============================================================================
// Implementação do Knob Concêntrico Duplo GAIN + ANEL DINÂMICO (Fiel à image_435802.png)
//==============================================================================
ConcentricGainDynamicsComponent::ConcentricGainDynamicsComponent()
{
    setRepaintsOnMouseActivity (true);
}

void ConcentricGainDynamicsComponent::bindParameters (juce::RangedAudioParameter* staticGainParam,
                                                      juce::RangedAudioParameter* dynamicRangeParam,
                                                      juce::UndoManager* undoMgr)
{
    gainParam = staticGainParam;
    dynRangeParam = dynamicRangeParam;
    undoManager = undoMgr;
    repaint();
}

void ConcentricGainDynamicsComponent::setLiveDynamicModulationDb (float modDb) noexcept
{
    if (std::abs (liveDynamicModDb - modDb) > 0.02f)
    {
        liveDynamicModDb = modDb;
        repaint();
    }
}

void ConcentricGainDynamicsComponent::setDynamicsActiveAndSupported (bool isSupported, bool isDynBypassed) noexcept
{
    if (shapeSupportsDynamics != isSupported || dynamicsBypassed != isDynBypassed)
    {
        shapeSupportsDynamics = isSupported;
        dynamicsBypassed = isDynBypassed;
        repaint();
    }
}

float ConcentricGainDynamicsComponent::getStaticGainDb() const noexcept
{
    if (gainParam == nullptr) return 0.0f;
    return gainParam->convertFrom0to1 (gainParam->getValue());
}

float ConcentricGainDynamicsComponent::getDynamicRangeDb() const noexcept
{
    if (dynRangeParam == nullptr) return 0.0f;
    return dynRangeParam->convertFrom0to1 (dynRangeParam->getValue());
}

void ConcentricGainDynamicsComponent::setStaticGainDb (float newDb)
{
    if (gainParam == nullptr) return;
    const float clamped = juce::jlimit (-30.0f, 30.0f, newDb);
    gainParam->setValueNotifyingHost (gainParam->convertTo0to1 (clamped));
    repaint();
}

void ConcentricGainDynamicsComponent::setDynamicRangeDb (float newDb)
{
    if (dynRangeParam == nullptr) return;
    const float clamped = juce::jlimit (-30.0f, 30.0f, newDb);
    dynRangeParam->setValueNotifyingHost (dynRangeParam->convertTo0to1 (clamped));
    repaint();
}

bool ConcentricGainDynamicsComponent::hitTestOuterRing (juce::Point<float> pt) const noexcept
{
    const auto centre = getLocalBounds().toFloat().getCentre();
    const float maxR  = juce::jmin (getWidth(), getHeight()) * 0.5f - 2.0f;
    const float innerR = maxR * 0.72f;
    const float dist = pt.getDistanceFrom (centre);
    return (dist >= innerR && dist <= maxR + 4.0f);
}

void ConcentricGainDynamicsComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto centre = bounds.getCentre();
    const float maxR  = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 3.0f;
    const float ringMidR = maxR - 4.5f;
    const float ringThickness = 7.5f;
    const float innerR = maxR * 0.70f;

    const float staticGainDb = getStaticGainDb();
    const float dynRangeDb   = getDynamicRangeDb();
    const float zeroAngle    = kRotaryStartAngle + 0.5f * (kRotaryEndAngle - kRotaryStartAngle);

    auto dbToAngle = [] (float db) noexcept
    {
        const float norm = juce::jlimit (0.0f, 1.0f, (db + 30.0f) / 60.0f);
        return kRotaryStartAngle + norm * (kRotaryEndAngle - kRotaryStartAngle);
    };

    const float gainAngle = dbToAngle (staticGainDb);
    const float dynTargetDb = juce::jlimit (-30.0f, 30.0f, staticGainDb + dynRangeDb);
    const float dynAngle = dbToAngle (dynTargetDb);

    // 1. Trilha escura do anel externo de faixa dinâmica (Dynamic Range Ring)
    juce::Path outerTrack;
    outerTrack.addCentredArc (centre.x, centre.y, ringMidR, ringMidR,
                              0.0f, kRotaryStartAngle, kRotaryEndAngle, true);
    g.setColour (juce::Colour (0xff14161b));
    g.strokePath (outerTrack, juce::PathStrokeType (ringThickness + 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

    // Halo bordô sutil na metade direita do anel como na imagem image_435802.png
    juce::Path rightHaloArc;
    rightHaloArc.addCentredArc (centre.x, centre.y, ringMidR, ringMidR,
                                0.0f, zeroAngle, kRotaryEndAngle, true);
    g.setColour (juce::Colour (0x38982832));
    g.strokePath (rightHaloArc, juce::PathStrokeType (ringThickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

    // 2. Arco da Faixa Dinâmica Configurada (vermelho/coral como na imagem image_435802.png)
    if (std::abs (dynRangeDb) > 0.05f)
    {
        juce::Path configuredDynArc;
        configuredDynArc.addCentredArc (centre.x, centre.y, ringMidR, ringMidR,
                                        0.0f, gainAngle, dynAngle, true);

        const float alpha = (shapeSupportsDynamics && ! dynamicsBypassed) ? 0.92f : 0.35f;
        g.setColour (kDynRedArc.withAlpha (alpha));
        g.strokePath (configuredDynArc, juce::PathStrokeType (ringThickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

        // Traço fino branco/claro no limite da faixa dinâmica configurada (exatamente como na imagem)
        juce::Path tickMark;
        tickMark.addRoundedRectangle (-0.9f, -ringMidR - ringThickness * 0.5f, 1.8f, ringThickness + 1.0f, 0.5f);
        tickMark.applyTransform (juce::AffineTransform::rotation (dynAngle).translated (centre.x, centre.y));
        g.setColour (juce::Colours::white.withAlpha (alpha));
        g.fillPath (tickMark);
    }

    // 3. Barra Amarela em Tempo Real (Live Dynamic Gain Change inside the ring, Manual p. 13:
    // "The current dynamic gain change is shown as a yellow bar inside the ring, on top of the dynamic range")
    if (shapeSupportsDynamics && ! dynamicsBypassed && std::abs (liveDynamicModDb) > 0.03f)
    {
        const float liveTargetDb = juce::jlimit (-30.0f, 30.0f, staticGainDb + liveDynamicModDb);
        const float liveAngle = dbToAngle (liveTargetDb);

        juce::Path liveYellowArc;
        liveYellowArc.addCentredArc (centre.x, centre.y, ringMidR, ringMidR,
                                     0.0f, gainAngle, liveAngle, true);
        g.setColour (kDynYellowLive);
        g.strokePath (liveYellowArc, juce::PathStrokeType (ringThickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }
    else if (std::abs (staticGainDb) > 0.05f)
    {
        // Arco amarelo do ganho estático do centro (0 dB) até o ponteiro do knob
        juce::Path staticArc;
        staticArc.addCentredArc (centre.x, centre.y, ringMidR, ringMidR,
                                 0.0f, zeroAngle, gainAngle, true);
        g.setColour (kDynYellowLive.withAlpha (0.90f));
        g.strokePath (staticArc, juce::PathStrokeType (ringThickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }

    // Realce de hover no anel dinâmico externo
    if (hoveringRing || draggingOuterRing)
    {
        g.setColour (kControlAccent.withAlpha (0.45f));
        g.drawEllipse (centre.x - maxR, centre.y - maxR, maxR * 2.0f, maxR * 2.0f, 1.3f);
    }

    // 4. Knob Central de Ganho Estático (GAIN)
    const float rx = centre.x - innerR;
    const float ry = centre.y - innerR;
    const float rw = innerR * 2.0f;

    g.setColour (juce::Colour (0xff121317));
    g.fillEllipse (rx - 1.5f, ry - 1.5f, rw + 3.0f, rw + 3.0f);

    juce::ColourGradient knobGrad (juce::Colour (0xff5c606b), centre.x, ry,
                                   juce::Colour (0xff353840), centre.x, ry + rw, false);
    g.setGradientFill (knobGrad);
    g.fillEllipse (rx, ry, rw, rw);

    g.setColour ((hoveringInner || draggingInnerKnob) ? kControlAccent.withAlpha (0.7f) : juce::Colour (0xff6c707d));
    g.drawEllipse (rx, ry, rw, rw, 1.1f);

    // Gota clara do ponteiro do knob central (fiel à imagem image_435802.png)
    juce::Path capWedge;
    const float capR = innerR - 1.5f;
    capWedge.addPieSegment (centre.x - capR, centre.y - capR,
                            capR * 2.0f, capR * 2.0f,
                            gainAngle - 0.34f, gainAngle + 0.34f, 0.28f);
    g.setColour (juce::Colour (0xfff2f0ee));
    g.fillPath (capWedge);

    juce::Path pointerLine;
    pointerLine.addRoundedRectangle (-1.0f, -capR + 3.5f, 2.0f, capR * 0.42f, 0.8f);
    pointerLine.applyTransform (juce::AffineTransform::rotation (gainAngle).translated (centre.x, centre.y));
    g.setColour (juce::Colour (0xff23252b));
    g.fillPath (pointerLine);
}

void ConcentricGainDynamicsComponent::mouseMove (const juce::MouseEvent& e)
{
    const bool onRing = hitTestOuterRing (e.position);
    const bool onInner = ! onRing && (e.position.getDistanceFrom (getLocalBounds().toFloat().getCentre()) < juce::jmin (getWidth(), getHeight()) * 0.36f);

    if (onRing != hoveringRing || onInner != hoveringInner)
    {
        hoveringRing = onRing;
        hoveringInner = onInner;
        repaint();
    }
}

void ConcentricGainDynamicsComponent::mouseExit (const juce::MouseEvent&)
{
    hoveringRing = false;
    hoveringInner = false;
    repaint();
}

void ConcentricGainDynamicsComponent::mouseDown (const juce::MouseEvent& e)
{
    dragStartY = e.position.y;

    if (hitTestOuterRing (e.position) || e.mods.isAltDown())
    {
        draggingOuterRing = true;
        draggingInnerKnob = false;
        dragStartValueDb  = getDynamicRangeDb();
        if (undoManager != nullptr)
            undoManager->beginNewTransaction ("Adjust Dynamic Range Ring");
    }
    else
    {
        draggingInnerKnob = true;
        draggingOuterRing = false;
        dragStartValueDb  = getStaticGainDb();
        if (undoManager != nullptr)
            undoManager->beginNewTransaction ("Adjust Static Gain Knob");
    }
    repaint();
}

void ConcentricGainDynamicsComponent::mouseDrag (const juce::MouseEvent& e)
{
    const float sensitivity = e.mods.isShiftDown() ? 0.08f : 0.38f;
    const float deltaDb = (dragStartY - e.position.y) * sensitivity;

    if (draggingOuterRing)
    {
        setDynamicRangeDb (dragStartValueDb + deltaDb);
    }
    else if (draggingInnerKnob)
    {
        setStaticGainDb (dragStartValueDb + deltaDb);
    }
}

void ConcentricGainDynamicsComponent::mouseUp (const juce::MouseEvent&)
{
    draggingOuterRing = false;
    draggingInnerKnob = false;
    repaint();
}

void ConcentricGainDynamicsComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (hitTestOuterRing (e.position))
    {
        if (undoManager != nullptr)
            undoManager->beginNewTransaction ("Reset Dynamic Range");
        setDynamicRangeDb (0.0f);
    }
    else
    {
        if (undoManager != nullptr)
            undoManager->beginNewTransaction ("Reset Static Gain");
        setStaticGainDb (0.0f);
    }
}

void ConcentricGainDynamicsComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const float step = (e.mods.isShiftDown() ? 0.25f : 1.0f) * wheel.deltaY * 5.0f;
    if (hitTestOuterRing (e.position) || e.mods.isAltDown())
        setDynamicRangeDb (getDynamicRangeDb() + step);
    else
        setStaticGainDb (getStaticGainDb() + step);
}

//==============================================================================
// Parser de Frequência e Notas Musicais ("2k", "A4", "C#3 + 13", "50%", Manual p. 6 & p. 18)
//==============================================================================
double ProQ3AudioProcessorEditor::parseFrequencyTextInput (const juce::String& text, double currentFreqHz) noexcept
{
    juce::String s = text.trim();
    if (s.isEmpty())
        return currentFreqHz;

    // Porcentagem ("50%" -> posição central logarítmica = ~547.7 Hz)
    if (s.endsWithChar ('%'))
    {
        const double pct = juce::jlimit (0.0, 100.0, s.dropLastCharacters (1).getDoubleValue()) * 0.01;
        return kMinFreqHz * std::pow (kMaxFreqHz / kMinFreqHz, pct);
    }

    // Multiplicador "k" ou "kHz" (ex: "2k" -> 2000 Hz, "6.5k" -> 6500 Hz)
    const juce::String lower = s.toLowerCase();
    if (lower.contains ("k"))
    {
        const double val = lower.upToFirstOccurrenceOf ("k", false, false).trim().getDoubleValue();
        if (val > 0.0)
            return juce::jlimit (kMinFreqHz, kMaxFreqHz, val * 1000.0);
    }

    // Nota musical (ex: "A4" -> 440 Hz, "C#3 + 13", "Bb2")
    const juce::juce_wchar firstChar = s[0];
    if ((firstChar >= 'A' && firstChar <= 'G') || (firstChar >= 'a' && firstChar <= 'g'))
    {
        int semitoneFromC = 0;
        switch (juce::CharacterFunctions::toUpperCase (firstChar))
        {
            case 'C': semitoneFromC = 0; break;
            case 'D': semitoneFromC = 2; break;
            case 'E': semitoneFromC = 4; break;
            case 'F': semitoneFromC = 5; break;
            case 'G': semitoneFromC = 7; break;
            case 'A': semitoneFromC = 9; break;
            case 'B': semitoneFromC = 11; break;
            default: break;
        }

        int pos = 1;
        if (pos < s.length())
        {
            if (s[pos] == '#') { semitoneFromC += 1; ++pos; }
            else if (s[pos] == 'b') { semitoneFromC -= 1; ++pos; }
        }

        juce::String rest = s.substring (pos).trim();
        int octave = 4;
        double centsOffset = 0.0;

        int signPos = rest.indexOfAnyOf ("+-", 1);
        if (signPos > 0)
        {
            octave = rest.substring (0, signPos).trim().getIntValue();
            centsOffset = rest.substring (signPos).removeCharacters (" ").getDoubleValue();
        }
        else if (rest.isNotEmpty())
        {
            octave = rest.getIntValue();
        }

        const double midiNote = static_cast<double> ((octave + 1) * 12 + semitoneFromC) + (centsOffset / 100.0);
        const double freq = 440.0 * std::pow (2.0, (midiNote - 69.0) / 12.0);
        return juce::jlimit (kMinFreqHz, kMaxFreqHz, freq);
    }

    const double numericVal = s.getDoubleValue();
    if (numericVal > 0.0)
        return juce::jlimit (kMinFreqHz, kMaxFreqHz, numericVal);

    return currentFreqHz;
}

//==============================================================================
// Editor Principal do FabFilter Pro-Q 3
//==============================================================================
ProQ3AudioProcessorEditor::ProQ3AudioProcessorEditor (ProQ3AudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    setLookAndFeel (&lookAndFeel);
    setWantsKeyboardFocus (true);

    setResizable (true, true);
    setResizeLimits (780, 460, 2560, 1440);

    //---- Top Bar Buttons ----
    for (auto* button : { &undoButton, &redoButton, &abButton, &copyButton, &prevPresetButton,
                          &presetNameButton, &nextPresetButton, &helpButton, &expandButton })
    {
        button->getProperties().set ("toolbar", true);
        button->setColour (juce::TextButton::textColourOffId, kTextBright);
    }
    addAndMakeVisible (undoButton);
    undoButton.onClick = [this] { audioProcessor.undoManager.undo(); refreshFactoryPresetName(); repaint(); };

    addAndMakeVisible (redoButton);
    redoButton.onClick = [this] { audioProcessor.undoManager.redo(); refreshFactoryPresetName(); repaint(); };

    addAndMakeVisible (abButton);
    abButton.onClick = [this]
    {
        audioProcessor.toggleABState();
        refreshFactoryPresetName();
        abButton.setButtonText (audioProcessor.isStateBActive() ? "B/A" : "A/B");
        attachFloatingControlsToBand (audioProcessor.getSelectedBandIndex());
        repaint();
    };

    addAndMakeVisible (copyButton);
    copyButton.onClick = [this] { audioProcessor.copyActiveToInactiveState(); };

    addAndMakeVisible (prevPresetButton);
    prevPresetButton.onClick = [this] { stepFactoryPreset (-1); };

    addAndMakeVisible (presetNameButton);
    presetNameButton.onClick = [this] { showFactoryPresetMenu(); };
    refreshFactoryPresetName();

    addAndMakeVisible (nextPresetButton);
    nextPresetButton.onClick = [this] { stepFactoryPreset (1); };

    addAndMakeVisible (helpButton);
    helpButton.setClickingTogglesState (true);
    helpButton.setTooltip ("Show interaction help");
    helpButton.onClick = [this] { showHelpOverlay = helpButton.getToggleState(); repaint(); };
    undoButton.setComponentID ("undo-edit");
    undoButton.setTitle ("Undo");
    undoButton.setButtonText ({});
    undoButton.setTooltip ("Undo the last edit");
    redoButton.setComponentID ("redo-edit");
    redoButton.setTitle ("Redo");
    redoButton.setButtonText ({});
    redoButton.setTooltip ("Redo the last undone edit");
    prevPresetButton.setTooltip ("Previous preset");
    nextPresetButton.setTooltip ("Next preset");
    abButton.setTooltip ("Switch between A and B settings");
    copyButton.setTooltip ("Copy the active settings to the other slot");

    addAndMakeVisible (expandButton);
    expandButton.setComponentID ("expand-editor");
    expandButton.setTitle ("Expand editor");
    expandButton.setTooltip ("Expand editor");
    expandButton.onClick = [this] { toggleExpandedView(); };

    //---- Floating Band Controls Pill (Fiel à imagem image_435802.png) ----
    addAndMakeVisible (bandBypassButton);
    bandBypassButton.setComponentID ("eq-band-power");
    bandBypassButton.setTitle ("Band Power");
    bandBypassButton.setTooltip ("Enable / Bypass Selected EQ Band (Alt+Click band dot)");

    addAndMakeVisible (bandShapeBox);
    bandShapeBox.addItemList ({
        "Bell", "Low Shelf", "Low Cut", "High Shelf", "High Cut",
        "Notch", "Band Pass", "Tilt Shelf", "Flat Tilt"
    }, 1);

    addAndMakeVisible (bandSlopeBox);
    bandSlopeBox.addItemList ({
        "6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct",
        "30 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct",
        "96 dB/oct", "Brickwall"
    }, 1);

    // Knob FREQ (com suporte a atalhos "2k", "A4", "C#3")
    freqSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    freqSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    freqSlider.setVelocityBasedMode (true);
    freqSlider.setDoubleClickReturnValue (true, 1000.0);
    addAndMakeVisible (freqSlider);

    // Knob Central Concêntrico GAIN + Anel Dinâmico Independente
    addAndMakeVisible (concentricGainKnob);

    // Knob Q
    qSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    qSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    qSlider.setVelocityBasedMode (true);
    qSlider.setDoubleClickReturnValue (true, 1.0);
    addAndMakeVisible (qSlider);

    gainQInterButton.setComponentID ("eq-gain-q");
    gainQInterButton.setTitle ("Gain-Q Interaction");
    addAndMakeVisible (gainQInterButton);
    gainQInterButton.setTooltip ("Gain-Q Interaction");

    // Botões do Arco Dinâmico acima do Knob GAIN (como na imagem image_435802.png):
    juce::Path dynamicsIcon;
    dynamicsIcon.startNewSubPath (0.0f, 7.0f);
    dynamicsIcon.lineTo (3.5f, 7.0f);
    dynamicsIcon.lineTo (6.0f, 2.0f);
    dynamicsIcon.lineTo (9.0f, 12.0f);
    dynamicsIcon.lineTo (11.5f, 7.0f);
    dynamicsIcon.lineTo (15.0f, 7.0f);
    juce::DrawablePath normalIcon, hoveredIcon;
    for (auto* icon : { &normalIcon, &hoveredIcon })
    {
        icon->setPath (dynamicsIcon);
        icon->setFill (juce::Colours::transparentBlack);
        icon->setStrokeType (juce::PathStrokeType (1.35f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    normalIcon.setStrokeFill (kTextBright);
    hoveredIcon.setStrokeFill (kEqGold);
    bandDynamicsShortcut.setImages (&normalIcon, &hoveredIcon, &hoveredIcon);
    bandDynamicsShortcut.setEdgeIndent (3);
    bandDynamicsShortcut.setComponentID ("band-open-dynamics");
    bandDynamicsShortcut.setTooltip ("Open Dynamic EQ controls for this band");
    bandDynamicsShortcut.onClick = [this]
    {
        const auto band = hoveredBandIndex;
        const auto bp = audioProcessor.getBandParameters (band);
        if (! bp.used || ! ProQ3AudioProcessor::doesShapeSupportDynamics (bp.shape))
            return;
        attachFloatingControlsToBand (band);
        expandedDynamicsBand = band;
        updateFloatingControlsVisibilityAndValues();
        repaint (graphBounds);
    };
    bandDynamicsShortcut.addMouseListener (this, false);
    addChildComponent (bandDynamicsShortcut);

    // 1. 'x' (Clear Dynamics): zera a faixa dinâmica sem excluir a banda
    addAndMakeVisible (clearDynamicsButton);
    clearDynamicsButton.setTooltip ("Clear Dynamics (reset Dynamic Range to 0 dB without deleting the band)");
    clearDynamicsButton.onClick = [this]
    {
        if (attachedBandIndex >= 0)
        {
            audioProcessor.undoManager.beginNewTransaction ("Clear Band Dynamics");
            if (auto* pd = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (attachedBandIndex, "dyn_range")))
                pd->setValueNotifyingHost (pd->convertTo0to1 (0.0f));
            repaint();
        }
    };

    // 2. Power/Bypass Dynamics: suspende somente a dinâmica, preservando o ganho estático e a faixa dinâmica
    addAndMakeVisible (bypassDynamicsButton);
    bypassDynamicsButton.setComponentID ("eq-dynamics-power");
    bypassDynamicsButton.setTitle ("Dynamic EQ Power");
    bypassDynamicsButton.setTooltip ("Bypass Dynamics (suspend dynamic processing while preserving static gain and dynamic range)");

    // 3. Botão Auto Threshold: alterna entre Auto e Manual Threshold (exibindo slider manual + medidor + botão Sidechain)
    addAndMakeVisible (autoThresholdButton);
    autoThresholdButton.setTooltip ("Toggle Auto Threshold / Manual Threshold Slider + External Sidechain");

    manualThresholdSlider.setSliderStyle (juce::Slider::LinearVertical);
    manualThresholdSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    manualThresholdSlider.setName ("Dynamic Threshold");
    manualThresholdSlider.setComponentID ("dynamic-threshold");
    manualThresholdSlider.setTooltip ("Drag to set Manual Threshold. The meter shows the detector level in dB.");
    addAndMakeVisible (manualThresholdSlider);
    manualThresholdValue.setJustificationType (juce::Justification::centred);
    manualThresholdValue.setFont (juce::FontOptions (11.0f));
    manualThresholdValue.setColour (juce::Label::textColourId, kTextBright);
    manualThresholdValue.setEditable (false, true, false);
    manualThresholdValue.setTooltip ("Double-click to enter the threshold in dB");
    manualThresholdValue.onTextChange = [this]
    {
        if (attachedBandIndex < 0)
            return;
        const auto text = manualThresholdValue.getText().trim();
        if (text.containsAnyOf ("0123456789"))
            manualThresholdSlider.setValue (juce::jlimit (-60.0, 0.0, text.getDoubleValue()), juce::sendNotificationSync);
    };
    addAndMakeVisible (manualThresholdValue);

    addAndMakeVisible (externalSidechainButton);
    externalSidechainButton.setTooltip ("Trigger Dynamic EQ from External Side Chain Input");

    auto setupLabel = [this] (juce::Label& lbl, const juce::String& text, float fontSize, bool bold, juce::Colour col)
    {
        lbl.setText (text, juce::dontSendNotification);
        lbl.setJustificationType (juce::Justification::centred);
        lbl.setFont (juce::FontOptions (fontSize, bold ? juce::Font::bold : juce::Font::plain));
        lbl.setColour (juce::Label::textColourId, col);
        addAndMakeVisible (lbl);
    };

    setupLabel (freqRangeHintLabel, "10 Hz     30 kHz", 10.5f, false, kTextMuted);
    setupLabel (gainRangeHintLabel, "-30         +30",  10.5f, false, kTextMuted);
    setupLabel (qRangeHintLabel,    "0.025        40",  10.5f, false, kTextMuted);

    setupLabel (freqTitleLabel, "FREQ", 12.0f, true, kTextMuted);
    setupLabel (gainTitleLabel, "GAIN", 12.0f, true, kTextMuted);
    setupLabel (qTitleLabel,    "Q",    12.0f, true, kTextMuted);

    // Duplo clique no título FREQ permite digitar "2k", "A4", "C#3", etc.
    freqTitleLabel.setEditable (true, true, false);
    freqTitleLabel.onTextChange = [this]
    {
        if (attachedBandIndex >= 0)
        {
            const auto bp = audioProcessor.getBandParameters (attachedBandIndex);
            const double parsed = parseFrequencyTextInput (freqTitleLabel.getText(), bp.freqHz);
            if (auto* pf = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (attachedBandIndex, "freq")))
                pf->setValueNotifyingHost (pf->convertTo0to1 (static_cast<float> (parsed)));
        }
        freqTitleLabel.setText ("FREQ", juce::dontSendNotification);
        repaint();
    };

    addAndMakeVisible (prevBandButton);
    prevBandButton.onClick = [this] { stepSelectedBand (-1); };

    bandNumberLabel.setJustificationType (juce::Justification::centred);
    bandNumberLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    bandNumberLabel.setColour (juce::Label::textColourId, kTextBright);
    addAndMakeVisible (bandNumberLabel);

    addAndMakeVisible (nextBandButton);
    nextBandButton.onClick = [this] { stepSelectedBand (1); };

    // 'X' no canto superior direito do painel flutuante: exclui a banda
    addAndMakeVisible (deleteBandButton);
    deleteBandButton.setTooltip ("Delete Selected EQ Band");
    deleteBandButton.onClick = [this]
    {
        const int sel = audioProcessor.getSelectedBandIndex();
        if (sel >= 0)
        {
            audioProcessor.deactivateBand (sel);
            attachFloatingControlsToBand (audioProcessor.getSelectedBandIndex());
            repaint();
        }
    };

    addAndMakeVisible (stereoModeBox);
    stereoModeBox.setComponentID ("band-stereo-mode");
    stereoModeBox.addItemList ({ "Stereo", "Left", "Right", "Mid", "Side" }, 1);

    addAndMakeVisible (soloBandButton);
    soloBandButton.onStateChange = [this]
    {
        const int sel = audioProcessor.getSelectedBandIndex();
        if (soloBandButton.isDown() && sel >= 0)
        {
            const auto bp = audioProcessor.getBandParameters (sel);
            audioProcessor.setSoloBandIndex (sel, static_cast<float> (bp.freqHz), static_cast<float> (juce::jmax (1.2, bp.q * 1.5)));
        }
        else if (audioProcessor.getSoloBandIndex() >= 0)
        {
            audioProcessor.setSoloBandIndex (-1);
        }
    };

    // Botão Split (tesoura na imagem image_435802.png: divide a banda selecionada em Left/Right ou Mid/Side, Manual p. 12 & p. 19)
    addAndMakeVisible (splitStereoButton);
    splitStereoButton.setTooltip ("Split Band into Left/Right or Mid/Side Pair");
    splitStereoButton.onClick = [this]
    {
        if (attachedBandIndex < 0) return;
        const auto bp = audioProcessor.getBandParameters (attachedBandIndex);
        if (! bp.used) return;

        const int newBand = audioProcessor.activateFirstFreeBand (
            static_cast<float> (bp.freqHz), static_cast<float> (bp.gainDb),
            static_cast<float> (bp.q), bp.shape, static_cast<float> (bp.dynRangeDb));

        if (newBand >= 0)
        {
            const int modeA = (bp.stereoMode >= 3) ? 3 : 1; // Mid ou Left
            const int modeB = (bp.stereoMode >= 3) ? 4 : 2; // Side ou Right
            if (auto* psA = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (attachedBandIndex, "stereo")))
                psA->setValueNotifyingHost (psA->convertTo0to1 (static_cast<float> (modeA)));
            if (auto* psB = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (newBand, "stereo")))
                psB->setValueNotifyingHost (psB->convertTo0to1 (static_cast<float> (modeB)));
            attachFloatingControlsToBand (newBand);
            repaint();
        }
    };

    //---- Bottom Bar Controls ----
    addAndMakeVisible (processingModeBox);
    processingModeBox.getProperties().set ("toolbar", true);
    processingModeBox.setTooltip ("Processing mode");
    processingModeBox.addItemList ({ "Zero Latency", "Natural Phase", "Linear Phase" }, 1);
    processingModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        audioProcessor.apvts, "processing_mode", processingModeBox);

    for (auto* button : { &analyzerButton, &midiLearnButton })
    {
        addAndMakeVisible (*button);
        button->getProperties().set ("toolbar", true);
        button->setColour (juce::TextButton::textColourOffId, kTextBright);
        button->setColour (juce::TextButton::textColourOnId, kTextBright);
    }
    analyzerButton.setComponentID ("analyzer-settings");
    analyzerButton.setTooltip ("Hover for spectrum sources. Click to keep the panel open.");
    analyzerButton.onClick = [this] { showAnalyzerMenu(); };
    configureAnalyzerPanel();
    configureMatchPanel();
    midiLearnButton.setComponentID ("midi-learn");
    midiLearnButton.setTooltip ("Assign a MIDI controller to an output control");
    midiLearnButton.onClick = [this] { showMidiLearnMenu(); };

    addAndMakeVisible (pianoToggleButton);
    pianoDisplayAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, "piano_display", pianoToggleButton);

    addAndMakeVisible (displayRangeButton);
    displayRangeButton.setComponentID ("display-range");
    displayRangeButton.setTooltip ("Cycle display range: 12, 6, 3, and 30 dB");
    displayRangeButton.setColour (juce::TextButton::textColourOffId, kEqGold);
    displayRangeButton.onClick = [this]
    {
        constexpr std::array<int, 4> nextRange { 3, 0, 1, 2 };
        const auto index = juce::jlimit (0, 3, juce::roundToInt (audioProcessor.apvts.getRawParameterValue ("display_range")->load()));
        setParameterFromEditor ("display_range", static_cast<float> (nextRange[static_cast<size_t> (index)]));
        refreshStatusControls();
        repaint (graphBounds);
    };
    refreshStatusControls();

    configureOutputControls();

    addAndMakeVisible (globalBypassButton);
    globalBypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, "global_bypass", globalBypassButton);

    attachFloatingControlsToBand (audioProcessor.getSelectedBandIndex());

    setSize (900, 540);
    startTimerHz (60);
}

ProQ3AudioProcessorEditor::~ProQ3AudioProcessorEditor()
{
    stopTimer();
    outputControlButton.removeMouseListener (this);
    audioProcessor.setSoloBandIndex (-1);
    setLookAndFeel (nullptr);
}

ProQ3AudioProcessorEditor::PianoButton::PianoButton() : juce::Button ("Piano")
{
    setClickingTogglesState (true);
    setTooltip ("Show or hide the piano keyboard");
}

void ProQ3AudioProcessorEditor::PianoButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const bool active = getToggleState();
    if (highlighted || down || active || hasKeyboardFocus (false))
    {
        g.setColour (juce::Colour (active ? 0xff403a45 : 0xff342f38));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 4.0f);
    }

    const auto keys = getLocalBounds().toFloat().withSizeKeepingCentre (20.0f, 13.0f);
    g.setColour (juce::Colour (active ? 0xffbdc0c8 : 0x9eb4b6be));
    g.drawRoundedRectangle (keys, 1.4f, 1.0f);
    for (int key = 1; key < 5; ++key)
    {
        const auto x = keys.getX() + static_cast<float> (key) * 4.0f;
        g.drawLine (x, keys.getY() + 1.0f, x, keys.getBottom() - 0.5f, 0.7f);
        if (key != 3)
            g.fillRect (x - 0.85f, keys.getY() + 0.6f, 1.7f, 6.5f);
    }
}

void ProQ3AudioProcessorEditor::OutputPanel::paint (juce::Graphics& g)
{
    const auto frame = getLocalBounds().toFloat().reduced (1.0f);
    juce::ColourGradient body (juce::Colour (0xca35303f), frame.getX(), frame.getY(),
                               juce::Colour (0xd5201d29), frame.getX(), frame.getBottom(), false);
    g.setGradientFill (body);
    g.fillRoundedRectangle (frame, 8.0f);
    g.setColour (juce::Colour (0xc06c6076));
    g.drawRoundedRectangle (frame, 8.0f, 1.0f);
    g.drawHorizontalLine (106, frame.getX() + 11.0f, frame.getRight() - 11.0f);
}

void ProQ3AudioProcessorEditor::configureOutputControls()
{
    using State = juce::AudioProcessorValueTreeState;

    addAndMakeVisible (outputControlButton);
    outputControlButton.setComponentID ("output-readout");
    outputControlButton.setColour (juce::TextButton::textColourOffId, kTextBright);
    outputControlButton.setTooltip ("Output Level and Gain Scale");
    outputControlButton.addMouseListener (this, false);
    outputControlButton.onClick = [this]
    {
        outputPanelKeyboardAccess = true;
        showOutputPanel();
        outputGainSlider.grabKeyboardFocus();
    };

    addChildComponent (outputPanel);
    outputPanel.setComponentID ("output-panel");
    for (auto* label : { &outputLevelLabel, &outputScaleLabel, &outputBalanceLabel })
    {
        label->setColour (juce::Label::textColourId, kTextBright);
        label->setFont (juce::FontOptions (11.5f));
        outputPanel.addAndMakeVisible (*label);
    }
    outputLevelLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    outputLevelLabel.setJustificationType (juce::Justification::centred);

    outputGainSlider.setComponentID ("output-level");
    outputGainSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    outputGainSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 18);
    outputGainSlider.setTextValueSuffix (" dB");
    outputGainSlider.setDoubleClickReturnValue (true, 0.0);
    outputGainSlider.setTooltip ("Output Level");
    outputGainSlider.setWantsKeyboardFocus (true);
    outputPanel.addAndMakeVisible (outputGainSlider);
    outputGainAttachment = std::make_unique<State::SliderAttachment> (audioProcessor.apvts, "global_output_gain", outputGainSlider);

    for (auto* slider : { &outputScaleSlider, &outputBalanceSlider })
    {
        slider->setSliderStyle (juce::Slider::LinearHorizontal);
        slider->setColour (juce::Slider::trackColourId, kControlAccent);
        slider->setColour (juce::Slider::thumbColourId, kTextBright);
        outputPanel.addAndMakeVisible (*slider);
    }

    outputScaleSlider.setComponentID ("output-gain-scale");
    outputScaleSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 18);
    outputScaleSlider.setTextValueSuffix ("%");
    outputScaleSlider.setDoubleClickReturnValue (true, 100.0);
    outputScaleSlider.setTooltip ("Scale the gain of EQ bands");
    outputScaleAttachment = std::make_unique<State::SliderAttachment> (audioProcessor.apvts, "global_gain_scale", outputScaleSlider);

    outputBalanceSlider.setComponentID ("output-balance");
    outputBalanceSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    outputBalanceSlider.setTooltip ("Drag to balance L/R or M/S. Double-click to centre.");
    outputBalanceSlider.setDoubleClickReturnValue (true, 0.0);
    outputBalanceSlider.setPopupDisplayEnabled (true, true, &outputPanel);
    outputBalanceAttachment = std::make_unique<State::SliderAttachment> (audioProcessor.apvts, "global_output_balance", outputBalanceSlider);

    outputBalanceModeBox.setComponentID ("output-balance-mode");
    outputBalanceModeBox.setTooltip ("Left/Right or Mid/Side balance");
    outputBalanceModeBox.addItemList ({ "L/R", "M/S" }, 1);
    outputPanel.addAndMakeVisible (outputBalanceModeBox);
    outputBalanceModeAttachment = std::make_unique<State::ComboBoxAttachment> (audioProcessor.apvts, "global_output_balance_mode", outputBalanceModeBox);

    outputPolarityButton.setComponentID ("output-polarity");
    outputAutoGainButton.setComponentID ("output-auto-gain");
    outputOptionsButton.setComponentID ("output-options");
    outputPolarityButton.setButtonText ("");
    outputOptionsButton.setButtonText ("");
    outputPolarityButton.setTitle ("Invert Polarity");
    outputAutoGainButton.setTitle ("Auto Gain");
    outputOptionsButton.setTitle ("Output Options");
    outputPolarityButton.setTooltip ("Invert output polarity");
    outputAutoGainButton.setTooltip ("Automatically compensate EQ gain");
    outputOptionsButton.setTooltip ("Output options");
    for (auto* button : { &outputPolarityButton, &outputAutoGainButton, &outputOptionsButton })
    {
        button->setColour (juce::TextButton::textColourOffId, kTextBright);
        outputPanel.addAndMakeVisible (*button);
    }
    outputPolarityButton.setClickingTogglesState (true);
    outputAutoGainButton.setClickingTogglesState (true);
    outputPolarityAttachment = std::make_unique<State::ButtonAttachment> (audioProcessor.apvts, "global_phase_invert", outputPolarityButton);
    outputAutoGainAttachment = std::make_unique<State::ButtonAttachment> (audioProcessor.apvts, "global_auto_gain", outputAutoGainButton);
    outputOptionsButton.onClick = [this] { showOutputOptions(); };

    outputGainSlider.onValueChange = [this] { updateOutputReadout(); };
    outputScaleSlider.onValueChange = [this] { updateOutputReadout(); };
    updateOutputReadout();
}

void ProQ3AudioProcessorEditor::setParameterFromEditor (const char* id, float value)
{
    if (auto* parameter = audioProcessor.apvts.getParameter (id))
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        parameter->endChangeGesture();
    }
}

void ProQ3AudioProcessorEditor::showMidiLearnMenu()
{
    juce::PopupMenu menu;
    if (audioProcessor.isMidiLearnArmed())
        menu.addSectionHeader ("Move a MIDI CC control to finish learning");
    menu.addItem (1, "Learn Output Level");
    menu.addItem (2, "Learn Gain Scale");
    menu.addItem (3, "Clear MIDI Assignments",
                  audioProcessor.isMidiLearnArmed() || audioProcessor.hasMidiOutputMapping());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&midiLearnButton),
                        [editor = juce::Component::SafePointer<ProQ3AudioProcessorEditor> (this)] (int item)
                        {
                            if (editor == nullptr || item == 0)
                                return;
                            using Target = ProQ3AudioProcessor::MidiOutputTarget;
                            if (item == 3)
                                editor->audioProcessor.clearMidiLearn();
                            else
                                editor->audioProcessor.armMidiLearn (item == 1 ? Target::outputLevel : Target::gainScale);
                            editor->refreshStatusControls();
                        });
}

void ProQ3AudioProcessorEditor::toggleExpandedView()
{
    if (! compactEditorBounds.isEmpty())
    {
        setSize (compactEditorBounds.getWidth(), compactEditorBounds.getHeight());
        compactEditorBounds = {};
        expandButton.setTooltip ("Expand editor");
        return;
    }

    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
    if (display == nullptr)
        return;

    const auto available = display->userArea.reduced (24);
    const int width = juce::jmin (1280, available.getWidth());
    const int height = juce::jmin (800, available.getHeight());
    if (width < getWidth() || height < getHeight() || (width == getWidth() && height == getHeight()))
        return;

    compactEditorBounds = getLocalBounds();
    setSize (width, height);
    expandButton.setTooltip ("Restore editor size");
}

void ProQ3AudioProcessorEditor::refreshStatusControls()
{
    const auto isOn = [this] (const char* id)
    {
        return audioProcessor.apvts.getRawParameterValue (id)->load (std::memory_order_relaxed) > 0.5f;
    };

    const bool pre = isOn ("analyzer_pre");
    const bool post = isOn ("analyzer_post");
    const bool sidechain = isOn ("analyzer_sc");
    constexpr const char* states[] { "Off", "Pre", "Post", "Pre+Post", "Sidechain", "Pre+SC", "Post+SC", "Pre+Post+SC" };
    const auto state = (pre ? 1 : 0) + (post ? 2 : 0) + (sidechain ? 4 : 0);
    const auto analyzerText = juce::String ("Analyzer: ") + states[state] + (analyzerExternalId > 0 ? "+" + getAnalyzerSourceName() : "");
    if (analyzerButton.getButtonText() != analyzerText)
        analyzerButton.setButtonText (analyzerText);

    constexpr const char* ranges[] { "3 dB", "6 dB", "12 dB", "30 dB" };
    const auto range = juce::jlimit (0, 3, juce::roundToInt (audioProcessor.apvts.getRawParameterValue ("display_range")->load()));
    if (displayRangeButton.getButtonText() != ranges[range])
        displayRangeButton.setButtonText (ranges[range]);

    const bool learning = audioProcessor.isMidiLearnArmed();
    midiLearnButton.setToggleState (learning || audioProcessor.hasMidiOutputMapping(), juce::dontSendNotification);
    const auto* midiTooltip = learning ? "Move a MIDI CC control now" : "Assign a MIDI controller to an output control";
    if (midiLearnButton.getTooltip() != midiTooltip)
        midiLearnButton.setTooltip (midiTooltip);
}

void ProQ3AudioProcessorEditor::updateOutputReadout()
{
    const auto gain = outputGainSlider.getValue();
    outputControlButton.setButtonText (juce::String (juce::roundToInt (outputScaleSlider.getValue()))
                                      + "%   " + (gain > 0.0 ? "+" : "") + juce::String (gain, 1) + " dB");
}

void ProQ3AudioProcessorEditor::showOutputPanel()
{
    outputPanelLastInteraction = juce::Time::getMillisecondCounterHiRes();
    if (! outputPanel.isVisible())
    {
        outputPanel.setVisible (true);
        outputPanel.toFront (false);
    }
}

void ProQ3AudioProcessorEditor::updateOutputPanelVisibility()
{
    if (! outputPanel.isVisible())
        return;

    const bool keepKeyboardFocus = outputPanelKeyboardAccess
        && (outputControlButton.hasKeyboardFocus (false) || outputPanel.hasKeyboardFocus (true));
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (outputControlButton.isMouseOver() || outputPanel.isMouseOver (true) || keepKeyboardFocus
        || outputOptionsOpen || outputBalanceModeBox.isPopupActive()
        || juce::Desktop::getInstance().getMainMouseSource().isDragging())
    {
        outputPanelLastInteraction = now;
    }
    else if (now - outputPanelLastInteraction >= 220.0)
    {
        outputPanelKeyboardAccess = false;
        outputPanel.setVisible (false);
    }
}

void ProQ3AudioProcessorEditor::showOutputOptions()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Reset Output Level");
    menu.addItem (2, "Reset Gain Scale");
    menu.addItem (3, "Centre Balance");
    outputOptionsOpen = true;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&outputOptionsButton),
                        [editor = juce::Component::SafePointer<ProQ3AudioProcessorEditor> (this)] (int item)
                        {
                            if (editor == nullptr)
                                return;
                            editor->outputOptionsOpen = false;
                            editor->outputPanelLastInteraction = juce::Time::getMillisecondCounterHiRes();
                            if (item == 1)
                                editor->outputGainSlider.setValue (0.0, juce::sendNotificationSync);
                            else if (item == 2)
                                editor->outputScaleSlider.setValue (100.0, juce::sendNotificationSync);
                            else if (item == 3)
                                editor->outputBalanceSlider.setValue (0.0, juce::sendNotificationSync);
                        });
}

//==============================================================================
// Conversões Geométricas
//==============================================================================
double ProQ3AudioProcessorEditor::getCurrentDisplayRangeDb() const noexcept
{
    const int idx = static_cast<int> (std::lround (
        audioProcessor.apvts.getRawParameterValue ("display_range")->load (std::memory_order_relaxed)));

    switch (idx)
    {
        case 0: return 3.0;
        case 1: return 6.0;
        case 2: return 12.0;
        case 3: return 30.0;
        default: return 12.0;
    }
}

float ProQ3AudioProcessorEditor::frequencyToX (double freqHz) const noexcept
{
    const double clamped = juce::jlimit (kMinFreqHz, kMaxFreqHz, freqHz);
    const double norm = std::log (clamped / kMinFreqHz) / std::log (kMaxFreqHz / kMinFreqHz);
    return static_cast<float> (graphBounds.getX())
        + static_cast<float> ((norm - frequencyViewStart) * graphBounds.getWidth() / frequencyViewSpan);
}

double ProQ3AudioProcessorEditor::xToFrequency (float x) const noexcept
{
    if (graphBounds.getWidth() <= 1)
        return 1000.0;

    const double norm = juce::jlimit (0.0, 1.0,
        static_cast<double> (x - static_cast<float> (graphBounds.getX())) / static_cast<double> (graphBounds.getWidth()));
    return kMinFreqHz * std::pow (kMaxFreqHz / kMinFreqHz, frequencyViewStart + norm * frequencyViewSpan);
}

bool ProQ3AudioProcessorEditor::isOverFrequencyStrip (juce::Point<int> position) const noexcept
{
    return pianoBounds.contains (position)
        && audioProcessor.apvts.getRawParameterValue ("piano_display")->load() < 0.5f;
}

void ProQ3AudioProcessorEditor::updateFrequencyView (double anchor, double zoom, double position)
{
    frequencyViewSpan = juce::jlimit (0.12, 1.0, zoom);
    frequencyViewStart = juce::jlimit (0.0, 1.0 - frequencyViewSpan, anchor - position * frequencyViewSpan);
    layoutBandControls();
    updateBandDynamicsShortcut();
    repaint (graphBounds);
}

float ProQ3AudioProcessorEditor::gainDbToY (double gainDb) const noexcept
{
    const double maxDb = getCurrentDisplayRangeDb();
    const auto norm = juce::jlimit (0.0, 1.0, 0.35 - (0.31 * gainDb / maxDb));
    return static_cast<float> (graphBounds.getY() + norm * graphBounds.getHeight());
}

double ProQ3AudioProcessorEditor::yToGainDb (float y) const noexcept
{
    if (graphBounds.getHeight() <= 1)
        return 0.0;

    const double norm = juce::jlimit (0.0, 1.0,
        static_cast<double> (y - static_cast<float> (graphBounds.getY())) / static_cast<double> (graphBounds.getHeight()));
    return juce::jlimit (-30.0, 30.0, (0.35 - norm) * getCurrentDisplayRangeDb() / 0.31);
}

int ProQ3AudioProcessorEditor::getSpectrumDisplayRange() const noexcept
{
    constexpr std::array<int, 4> ranges { 60, 90, 100, 120 };
    return ranges[static_cast<size_t> (juce::jlimit (0, 3, analyzerPanel.range.getSelectedId() - 1))];
}

float ProQ3AudioProcessorEditor::analyzerDbToY (double analyzerDb) const noexcept
{
    // Use one dBFS axis for the spectrum, meter bars and meter labels.
    const auto top = graphBounds.getY() + 22;
    const auto height = graphBounds.getBottom() - top - 12;
    const auto norm = -analyzerDb / getSpectrumDisplayRange();
    return static_cast<float> (juce::jlimit (static_cast<double> (top),
                                            static_cast<double> (graphBounds.getBottom()), top + norm * height));
}

juce::String ProQ3AudioProcessorEditor::frequencyToMusicalNoteName (double freqHz)
{
    if (freqHz <= 0.0)
        return {};

    static const char* noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const double midiExact = 69.0 + 12.0 * std::log2 (freqHz / 440.0);
    const int midiNearest = static_cast<int> (std::lround (midiExact));
    const int cents = static_cast<int> (std::lround ((midiExact - static_cast<double> (midiNearest)) * 100.0));

    const int noteIdx = ((midiNearest % 12) + 12) % 12;
    const int octave  = (midiNearest / 12) - 1;

    juce::String s = juce::String (noteNames[noteIdx]) + juce::String (octave);
    if (cents != 0)
        s += (cents > 0 ? " +" : " ") + juce::String (cents);
    return s;
}

double ProQ3AudioProcessorEditor::snapFrequencyToNearestSemitone (double freqHz) noexcept
{
    const double midiExact = 69.0 + 12.0 * std::log2 (juce::jmax (10.0, freqHz) / 440.0);
    const double midiRound = std::round (midiExact);
    return 440.0 * std::pow (2.0, (midiRound - 69.0) / 12.0);
}

//==============================================================================
// Conexão do Painel Flutuante (Floating Band Controls Pill) à Banda Selecionada
//==============================================================================
void ProQ3AudioProcessorEditor::refreshFactoryPresetName()
{
    const auto name = audioProcessor.getFactoryPresetName (audioProcessor.getFactoryPresetIndex());
    presetNameButton.setButtonText (name.isEmpty() ? juce::String ("Presets") : name);
    presetNameButton.setTooltip (name);
}

void ProQ3AudioProcessorEditor::loadFactoryPreset (int index)
{
    audioProcessor.loadFactoryPreset (index);
    refreshFactoryPresetName();
    attachFloatingControlsToBand (audioProcessor.getSelectedBandIndex());
    repaint();
}

void ProQ3AudioProcessorEditor::stepFactoryPreset (int delta)
{
    const int count = audioProcessor.getFactoryPresetCount();
    const int current = audioProcessor.getFactoryPresetIndex();
    const int next = current < 0 ? (delta < 0 ? count - 1 : 0) : (current + delta + count) % count;
    loadFactoryPreset (next);
}

void ProQ3AudioProcessorEditor::showFactoryPresetMenu()
{
    juce::PopupMenu menu;
    std::map<juce::String, juce::PopupMenu> categories;
    const int current = audioProcessor.getFactoryPresetIndex();
    for (int index = 0; index < audioProcessor.getFactoryPresetCount(); ++index)
    {
        const auto category = audioProcessor.getFactoryPresetCategory (index);
        auto& section = category.isEmpty() ? menu : categories[category];
        section.addItem (index + 1, audioProcessor.getFactoryPresetName (index), true, index == current);
    }

    juce::PopupMenu dynamic;
    for (const auto& [category, presets] : categories)
        dynamic.addSubMenu (category, presets);
    menu.addSeparator();
    menu.addSubMenu ("Dynamic", dynamic);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetNameButton),
                        [editor = juce::Component::SafePointer<ProQ3AudioProcessorEditor> (this)] (int item)
                        {
                            if (editor != nullptr && item != 0)
                                editor->loadFactoryPreset (item - 1);
                        });
}

void ProQ3AudioProcessorEditor::attachFloatingControlsToBand (int bandIndex)
{
    if (bandIndex != attachedBandIndex || ! audioProcessor.getBandParameters (bandIndex).used)
        expandedDynamicsBand = -1;
    bandEnabledAttachment.reset();
    bandShapeAttachment.reset();
    bandSlopeAttachment.reset();
    freqAttachment.reset();
    qAttachment.reset();
    gainQInterAttachment.reset();
    bypassDynamicsAttachment.reset();
    autoThresholdAttachment.reset();
    manualThresholdAttachment.reset();
    externalSidechainAttachment.reset();
    stereoModeAttachment.reset();

    if (bandIndex < 0 || bandIndex >= ProQ3AudioProcessor::kMaxBands || ! audioProcessor.getBandParameters (bandIndex).used)
    {
        attachedBandIndex = -1;
        concentricGainKnob.bindParameters (nullptr, nullptr, nullptr);
        updateFloatingControlsVisibilityAndValues();
        return;
    }

    attachedBandIndex = bandIndex;
    audioProcessor.setSelectedBandIndex (bandIndex);
    layoutBandControls();

    bandEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "enabled"), bandBypassButton);

    bandShapeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "shape"), bandShapeBox);

    bandSlopeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "slope"), bandSlopeBox);

    freqAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "freq"), freqSlider);

    concentricGainKnob.bindParameters (
        audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (bandIndex, "gain")),
        audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (bandIndex, "dyn_range")),
        &audioProcessor.undoManager);

    qAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "q"), qSlider);

    gainQInterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "gain_q"), gainQInterButton);

    bypassDynamicsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "dyn_enabled"), bypassDynamicsButton);

    autoThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "dyn_auto"), autoThresholdButton);

    manualThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "dyn_thresh"), manualThresholdSlider);

    externalSidechainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "dyn_ext_sc"), externalSidechainButton);

    stereoModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        audioProcessor.apvts, ProQ3AudioProcessor::getBandParamID (bandIndex, "stereo"), stereoModeBox);

    bandNumberLabel.setText (juce::String (bandIndex + 1), juce::dontSendNotification);
    updateFloatingControlsVisibilityAndValues();
}

void ProQ3AudioProcessorEditor::updateFloatingControlsVisibilityAndValues()
{
    const bool visible = (attachedBandIndex >= 0 && attachedBandIndex < ProQ3AudioProcessor::kMaxBands
                          && audioProcessor.getBandParameters (attachedBandIndex).used);

    bandBypassButton.setVisible (visible);
    bandShapeBox.setVisible (visible);
    bandSlopeBox.setVisible (visible);
    freqSlider.setVisible (visible);
    concentricGainKnob.setVisible (visible);
    qSlider.setVisible (visible);
    gainQInterButton.setVisible (visible);
    freqTitleLabel.setVisible (visible);
    gainTitleLabel.setVisible (visible);
    qTitleLabel.setVisible (visible);
    freqRangeHintLabel.setVisible (visible);
    gainRangeHintLabel.setVisible (visible);
    qRangeHintLabel.setVisible (visible);
    prevBandButton.setVisible (visible);
    bandNumberLabel.setVisible (visible);
    nextBandButton.setVisible (visible);
    deleteBandButton.setVisible (visible);
    stereoModeBox.setVisible (visible);
    soloBandButton.setVisible (visible);
    splitStereoButton.setVisible (visible);

    if (visible)
    {
        const auto bp = audioProcessor.getBandParameters (attachedBandIndex);
        const bool supportsGain = ProQ3AudioProcessor::doesShapeSupportGain (bp.shape);
        const bool supportsDyn  = ProQ3AudioProcessor::doesShapeSupportDynamics (bp.shape);
        const bool supportsQ    = ProQ3AudioProcessor::doesShapeSupportQ (bp.shape, bp.slopeIndex);
        const bool hasDynRange  = std::abs (bp.dynRangeDb) > 0.05;

        concentricGainKnob.setEnabled (supportsGain);
        concentricGainKnob.setDynamicsActiveAndSupported (supportsDyn, ! bp.dynEnabled);
        concentricGainKnob.setLiveDynamicModulationDb (audioProcessor.getBandDynamicGainDb (attachedBandIndex));

        qSlider.setEnabled (supportsQ);
        bandSlopeBox.setEnabled (bp.shape != ProQ3AudioProcessor::kShapeFlatTilt);

        // Quando a banda tem Dynamic Range != 0 (ou está em modo Manual Threshold), exibe os botões do topo do anel
        // (clearDynamics 'x', bypassDynamics power, Auto button); quando Auto está desligado, expande o slider de limiar manual e o botão Sidechain (SC)
        const bool showDynTopControls = supportsDyn
            && (hasDynRange || ! bp.dynAutoThresh || expandedDynamicsBand == attachedBandIndex);
        clearDynamicsButton.setVisible (showDynTopControls);
        bypassDynamicsButton.setVisible (showDynTopControls);
        autoThresholdButton.setVisible (supportsDyn);

        const bool showManualThreshControls = showDynTopControls && ! bp.dynAutoThresh;
        manualThresholdSlider.setVisible (showManualThreshControls);
        manualThresholdValue.setVisible (showManualThreshControls);
        if (showManualThreshControls && ! manualThresholdValue.isBeingEdited())
        {
            const auto text = juce::String (bp.dynThresholdDb, 1) + " dB";
            if (manualThresholdValue.getText() != text)
                manualThresholdValue.setText (text, juce::dontSendNotification);
        }
        externalSidechainButton.setVisible (showManualThreshControls);
    }
    else
    {
        clearDynamicsButton.setVisible (false);
        bypassDynamicsButton.setVisible (false);
        autoThresholdButton.setVisible (false);
        manualThresholdSlider.setVisible (false);
        manualThresholdValue.setVisible (false);
        externalSidechainButton.setVisible (false);
    }

}

void ProQ3AudioProcessorEditor::stepSelectedBand (int delta)
{
    std::vector<int> activeBands;
    for (int i = 0; i < ProQ3AudioProcessor::kMaxBands; ++i)
    {
        if (audioProcessor.getBandParameters (i).used)
            activeBands.push_back (i);
    }

    if (activeBands.empty())
        return;

    std::sort (activeBands.begin(), activeBands.end(), [this] (int a, int b)
    {
        return audioProcessor.getBandParameters (a).freqHz < audioProcessor.getBandParameters (b).freqHz;
    });

    int curPos = 0;
    for (size_t i = 0; i < activeBands.size(); ++i)
    {
        if (activeBands[i] == attachedBandIndex)
        {
            curPos = static_cast<int> (i);
            break;
        }
    }

    const int n = static_cast<int> (activeBands.size());
    const int nextPos = ((curPos + delta) % n + n) % n;
    attachFloatingControlsToBand (activeBands[static_cast<size_t> (nextPos)]);
    repaint();
}

int ProQ3AudioProcessorEditor::findBandNodeAtPosition (juce::Point<float> pos, float maxRadiusPx) const noexcept
{
    int bestBand = -1;
    float bestDistSq = maxRadiusPx * maxRadiusPx;

    for (int i = 0; i < ProQ3AudioProcessor::kMaxBands; ++i)
    {
        const auto bp = audioProcessor.getBandParameters (i);
        if (! bp.used)
            continue;

        const float nx = frequencyToX (bp.freqHz);
        const float ny = gainDbToY (bp.gainDb);
        const float d2 = pos.getDistanceSquaredFrom ({ nx, ny });

        if (d2 <= bestDistSq)
        {
            bestDistSq = d2;
            bestBand = i;
        }
    }
    return bestBand;
}

//==============================================================================
// Eventos de Mouse e Teclado (incluindo Alt + Roda para Dynamic Range, Make Dynamic e Invert Gain)
//==============================================================================
void ProQ3AudioProcessorEditor::mouseEnter (const juce::MouseEvent& event)
{
    if (event.eventComponent == &outputControlButton)
        showOutputPanel();
}

void ProQ3AudioProcessorEditor::mouseExit (const juce::MouseEvent& event)
{
    spectrumGrabPointerInGraph = false;
    if (isOverBandTooltip (event.getEventRelativeTo (this).getPosition()))
        return;

    hoveredBandIndex = -1;
    updateBandDynamicsShortcut();
    repaint (graphBounds);
}

void ProQ3AudioProcessorEditor::mouseMove (const juce::MouseEvent& event)
{
    const auto position = event.getEventRelativeTo (this).position;
    lastMousePos = position;
    spectrumGrabPointer = position.roundToInt();
    spectrumGrabPointerInGraph = event.eventComponent == this
        && graphBounds.withTrimmedBottom (22).contains (spectrumGrabPointer);

    auto hit = findBandNodeAtPosition (position, 14.0f);
    if (hit < 0 && isOverBandTooltip (position.roundToInt()))
        hit = hoveredBandIndex;
    if (hit != hoveredBandIndex)
    {
        hoveredBandIndex = hit;
        updateBandDynamicsShortcut();
        repaint (graphBounds);
    }
    else if (graphBounds.contains (event.getPosition()) || pianoBounds.contains (event.getPosition()))
    {
        repaint();
    }
}

void ProQ3AudioProcessorEditor::mouseDown (const juce::MouseEvent& event)
{
    if (event.eventComponent == &bandDynamicsShortcut)
        return;

    lastMousePos = event.position;

    if (pianoBounds.contains (event.getPosition())
        && audioProcessor.apvts.getRawParameterValue ("piano_display")->load() >= 0.5f)
    {
        const int sel = audioProcessor.getSelectedBandIndex();
        if (sel >= 0 && audioProcessor.getBandParameters (sel).used)
        {
            const double clickedFreq = snapFrequencyToNearestSemitone (xToFrequency (event.position.x));
            if (auto* pf = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (sel, "freq")))
                pf->setValueNotifyingHost (pf->convertTo0to1 (static_cast<float> (clickedFreq)));
            repaint();
        }
        return;
    }

    if (! graphBounds.contains (event.getPosition()) || event.getNumberOfClicks() > 1)
        return;

    if (event.mods.isLeftButtonDown() && isOverFrequencyStrip (event.getPosition()))
    {
        draggingFrequencyView = true;
        dragStartMousePos = event.position;
        dragStartViewSpan = frequencyViewSpan;
        dragStartViewAnchor = std::log (xToFrequency (event.position.x) / kMinFreqHz) / std::log (kMaxFreqHz / kMinFreqHz);
        return;
    }

    if (spectrumGrabActive && event.mods.isLeftButtonDown() && grabSpectrumPeak (event.position))
        return;

    // Verifica primeiro se clicou no indicador de Dynamic Range acima/abaixo do nó da banda selecionada (Manual p. 7 & p. 13)
    const int sel = audioProcessor.getSelectedBandIndex();
    if (sel >= 0 && audioProcessor.getBandParameters (sel).used)
    {
        const auto bp = audioProcessor.getBandParameters (sel);
        if (ProQ3AudioProcessor::doesShapeSupportDynamics (bp.shape) && std::abs (bp.dynRangeDb) > 0.05)
        {
            const float nx = frequencyToX (bp.freqHz);
            const float dynY = gainDbToY (bp.gainDb + bp.dynRangeDb);
            if (event.position.getDistanceFrom ({ nx, dynY }) <= 10.0f)
            {
                draggingBandIndex = sel;
                isDraggingDynHandle = true;
                dragStartMousePos = event.position;
                dragStartDynRangeDb = bp.dynRangeDb;
                audioProcessor.undoManager.beginNewTransaction ("Drag Dynamic Range Handle");
                return;
            }
        }
    }

    const int hitBand = findBandNodeAtPosition (event.position, 15.0f);

    if (hitBand >= 0)
    {
        const auto bp = audioProcessor.getBandParameters (hitBand);

        // Alt-click toggles band bypass.
        if (event.mods.isAltDown() && event.mods.isLeftButtonDown())
        {
            audioProcessor.undoManager.beginNewTransaction ("Toggle Band Bypass");
            if (auto* pe = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (hitBand, "enabled")))
                pe->setValueNotifyingHost (bp.enabled ? 0.0f : 1.0f);
            attachFloatingControlsToBand (hitBand);
            repaint();
            return;
        }

        // Menu de contexto da banda com Make Dynamic, Invert Gain, 9 Tipos e 10 Slopes (Manual p. 1, p. 8, p. 13)
        if (event.mods.isPopupMenu())
        {
            attachFloatingControlsToBand (hitBand);

            juce::PopupMenu menu;
            menu.addSectionHeader ("Band " + juce::String (hitBand + 1));
            menu.addItem (1, bp.enabled ? "Bypass Band" : "Enable Band", true, ! bp.enabled);

            const bool isDyn = std::abs (bp.dynRangeDb) > 0.05;
            menu.addItem (2, isDyn ? "Clear Dynamics (Static EQ)" : "Make Dynamic (-6.0 dB)",
                          ProQ3AudioProcessor::doesShapeSupportDynamics (bp.shape), isDyn);
            menu.addItem (3, "Invert Gain (" + juce::String (-bp.gainDb, 2) + " dB)",
                          ProQ3AudioProcessor::doesShapeSupportGain (bp.shape));
            menu.addItem (4, "Reset Gain to 0.0 dB");
            menu.addItem (5, "Reset Q to 1.00");
            menu.addItem (6, "Quantize to Note (" + frequencyToMusicalNoteName (snapFrequencyToNearestSemitone (bp.freqHz)) + ")");

            juce::PopupMenu shapeMenu;
            const juce::StringArray shapes {
                "Bell", "Low Shelf", "Low Cut", "High Shelf", "High Cut",
                "Notch", "Band Pass", "Tilt Shelf", "Flat Tilt"
            };
            for (int s = 0; s < shapes.size(); ++s)
                shapeMenu.addItem (100 + s, shapes[s], true, bp.shape == s);
            menu.addSubMenu ("Shape", shapeMenu);

            juce::PopupMenu slopeMenu;
            const juce::StringArray slopes {
                "6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct",
                "30 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct",
                "96 dB/oct", "Brickwall"
            };
            for (int sl = 0; sl < slopes.size(); ++sl)
            {
                const bool allowed = (sl < 9) || (bp.shape == ProQ3AudioProcessor::kShapeLowCut || bp.shape == ProQ3AudioProcessor::kShapeHighCut);
                slopeMenu.addItem (200 + sl, slopes[sl], allowed, bp.slopeIndex == sl);
            }
            menu.addSubMenu ("Slope", slopeMenu);

            menu.addSeparator();
            menu.addItem (99, "Delete Band");

            menu.showMenuAsync (juce::PopupMenu::Options(), [this, hitBand, bp, isDyn] (int result)
            {
                if (result == 0) return;
                audioProcessor.undoManager.beginNewTransaction ("Band Context Menu");

                auto setP = [this, hitBand] (const juce::String& suffix, float val)
                {
                    if (auto* p = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (hitBand, suffix)))
                        p->setValueNotifyingHost (p->convertTo0to1 (val));
                };

                if (result == 1) setP ("enabled", bp.enabled ? 0.0f : 1.0f);
                else if (result == 2)
                {
                    // Make Dynamic atualiza exatamente o mesmo parâmetro dyn_range que o anel externo do GAIN!
                    setP ("dyn_range", isDyn ? 0.0f : (bp.gainDb >= 0.0 ? -6.0f : 6.0f));
                    setP ("dyn_enabled", 1.0f);
                }
                else if (result == 3) setP ("gain", static_cast<float> (-bp.gainDb));
                else if (result == 4) setP ("gain", 0.0f);
                else if (result == 5) setP ("q", 1.0f);
                else if (result == 6) setP ("freq", static_cast<float> (snapFrequencyToNearestSemitone (bp.freqHz)));
                else if (result >= 100 && result < 109) setP ("shape", static_cast<float> (result - 100));
                else if (result >= 200 && result < 210) setP ("slope", static_cast<float> (result - 200));
                else if (result == 99)
                {
                    audioProcessor.deactivateBand (hitBand);
                    attachFloatingControlsToBand (audioProcessor.getSelectedBandIndex());
                }
                updateFloatingControlsVisibilityAndValues();
                repaint();
            });
            return;
        }

        attachFloatingControlsToBand (hitBand);
        draggingBandIndex   = hitBand;
        isDraggingNode      = true;
        dragStartMousePos   = event.position;
        dragStartFreqHz     = bp.freqHz;
        dragStartGainDb     = bp.gainDb;
        dragStartDynRangeDb = bp.dynRangeDb;
        dragStartQ          = bp.q;

        audioProcessor.undoManager.beginNewTransaction ("Drag EQ Band " + juce::String (hitBand + 1));
        repaint();
        return;
    }

    if (event.mods.isLeftButtonDown() && analyzerPanel.grab.getToggleState() && grabSpectrumPeak (event.position))
        return;

    // Alt + clique/arraste ou Ctrl + clique cria uma banda (com Alt cria banda já dinâmica, Manual p. 1 & p. 7)
    if (event.mods.isCtrlDown() || event.mods.isCommandDown() || event.mods.isAltDown())
    {
        const float freq = static_cast<float> (xToFrequency (event.position.x));
        const float gain = static_cast<float> (yToGainDb (event.position.y));
        const float initGain = event.mods.isAltDown() ? 0.0f : gain;
        const float initDyn  = event.mods.isAltDown() ? gain : 0.0f;

        const int newIdx = audioProcessor.activateFirstFreeBand (freq, initGain, 1.0f, ProQ3AudioProcessor::kShapeBell, initDyn);
        if (newIdx >= 0)
        {
            attachFloatingControlsToBand (newIdx);
            draggingBandIndex = newIdx;
            isDraggingNode    = true;
            dragStartMousePos = event.position;
            dragStartFreqHz   = freq;
            dragStartGainDb   = initGain;
            dragStartQ        = 1.0;
            repaint();
        }
        return;
    }

    const double cursorFreq = xToFrequency (event.position.x);
    const double curveDbAtCursor = audioProcessor.getTotalMagnitudeDbAtFrequency (cursorFreq, false);
    const float curveY = gainDbToY (curveDbAtCursor);

    if (event.mods.isLeftButtonDown() && std::abs (event.position.y - curveY) <= 7.0f)
    {
        if (graphEdgeFilterShape (event.position.x, graphBounds) != ProQ3AudioProcessor::kShapeBell)
        {
            waitingForEdgeDrag = true;
            dragStartMousePos = event.position;
            return;
        }

        if (startNewCurveBandDrag (event.position))
            return;
    }

    isSelectionRectDragging = true;
    dragStartMousePos = event.position;
    selectionLassoRect = {};
    repaint();
}

bool ProQ3AudioProcessorEditor::grabSpectrumPeak (juce::Point<float> position)
{
    if (! analyzerPanel.grab.getToggleState()
        || (! spectrumGrabActive && audioProcessor.apvts.getRawParameterValue ("analyzer_peak_alerts")->load() < 0.5f))
        return false;
    for (size_t i = 0; i < peakTags.size(); ++i)
    {
        if (peakTags[i].isEmpty() || ! peakTags[i].expanded (4.0f, 5.0f).contains (position))
            continue;

        constexpr float initialGain = -3.0f;
        const auto frequency = static_cast<float> (peakTagFrequencies[i]);
        const auto band = audioProcessor.activateFirstFreeBand (frequency, initialGain, 2.0f);
        if (band < 0)
            return false;

        attachFloatingControlsToBand (band);
        draggingBandIndex = band;
        isDraggingNode = true;
        draggingSpectrumPeak = true;
        dragStartMousePos = position;
        dragStartFreqHz = frequency;
        dragStartGainDb = initialGain;
        dragStartQ = 2.0;
        repaint (graphBounds);
        return true;
    }
    return false;
}

bool ProQ3AudioProcessorEditor::startNewCurveBandDrag (juce::Point<float> position)
{
    const auto frequency = static_cast<float> (xToFrequency (position.x));
    const auto gain = static_cast<float> (yToGainDb (position.y));
    const int bandIndex = audioProcessor.activateFirstFreeBand (frequency, gain);
    if (bandIndex < 0)
        return false;

    attachFloatingControlsToBand (bandIndex);
    draggingBandIndex = bandIndex;
    isDraggingNode = true;
    dragStartMousePos = position;
    dragStartFreqHz = frequency;
    dragStartGainDb = gain;
    dragStartQ = 1.0;
    repaint (graphBounds);
    return true;
}

void ProQ3AudioProcessorEditor::mouseDrag (const juce::MouseEvent& event)
{
    if (event.eventComponent == &bandDynamicsShortcut)
        return;

    lastMousePos = event.position;

    if (draggingFrequencyView)
    {
        const auto width = static_cast<double> (graphBounds.getWidth());
        const auto anchorPosition = (static_cast<double> (dragStartMousePos.x) - graphBounds.getX()) / width;
        const auto delta = (event.position.x - dragStartMousePos.x) / width;
        const auto zoom = dragStartViewSpan * std::exp ((event.position.y - dragStartMousePos.y) * 0.015);
        updateFrequencyView (dragStartViewAnchor, zoom, anchorPosition + delta);
        return;
    }

    if (waitingForEdgeDrag)
    {
        if (event.position.getDistanceSquaredFrom (dragStartMousePos) < 16.0f)
            return;

        waitingForEdgeDrag = false;
        startNewCurveBandDrag (dragStartMousePos);
    }

    if (isDraggingDynHandle && draggingBandIndex >= 0)
    {
        const auto bp = audioProcessor.getBandParameters (draggingBandIndex);
        const double targetTotalDb = yToGainDb (event.position.y);
        const double newDynRange = juce::jlimit (-30.0, 30.0, targetTotalDb - bp.gainDb);
        if (auto* pd = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (draggingBandIndex, "dyn_range")))
            pd->setValueNotifyingHost (pd->convertTo0to1 (static_cast<float> (newDynRange)));
        repaint (graphBounds);
        return;
    }

    if (isDraggingNode && draggingBandIndex >= 0 && draggingBandIndex < ProQ3AudioProcessor::kMaxBands)
    {
        const float sensitivity = event.mods.isShiftDown() ? 0.2f : 1.0f;
        const float dx = (event.position.x - dragStartMousePos.x) * sensitivity;
        const float dy = (event.position.y - dragStartMousePos.y) * sensitivity;

        if (event.mods.isCtrlDown() || event.mods.isCommandDown())
        {
            const double qFactor = std::pow (2.0, static_cast<double> (-dy) / 75.0);
            const double newQ = juce::jlimit (0.025, 40.0, dragStartQ * qFactor);
            if (auto* pq = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (draggingBandIndex, "q")))
                pq->setValueNotifyingHost (pq->convertTo0to1 (static_cast<float> (newQ)));
        }
        else
        {
            bool lockHorizontal = false;
            bool lockVertical   = false;
            if (event.mods.isAltDown())
            {
                if (std::abs (dx) >= std::abs (dy))
                    lockVertical = true;
                else
                    lockHorizontal = true;
            }

            if (! lockHorizontal)
            {
                const float startX = frequencyToX (dragStartFreqHz);
                const double newFreq = xToFrequency (startX + dx);
                if (auto* pf = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (draggingBandIndex, "freq")))
                    pf->setValueNotifyingHost (pf->convertTo0to1 (static_cast<float> (newFreq)));
                layoutBandControls();
            }

            if (! lockVertical && ProQ3AudioProcessor::doesShapeSupportGain (audioProcessor.getBandParameters (draggingBandIndex).shape))
            {
                const float startY = gainDbToY (dragStartGainDb);
                const double newGain = juce::jlimit (-30.0, 30.0, yToGainDb (startY + dy));
                if (auto* pg = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (draggingBandIndex, "gain")))
                    pg->setValueNotifyingHost (pg->convertTo0to1 (static_cast<float> (newGain)));
            }
        }

        repaint (graphBounds);
        return;
    }

    if (isSelectionRectDragging)
    {
        selectionLassoRect = juce::Rectangle<float> (dragStartMousePos, event.position);
        for (int i = 0; i < ProQ3AudioProcessor::kMaxBands; ++i)
        {
            const auto bp = audioProcessor.getBandParameters (i);
            if (! bp.used)
                continue;
            const juce::Point<float> pt (frequencyToX (bp.freqHz), gainDbToY (bp.gainDb));
            if (selectionLassoRect.contains (pt))
            {
                attachFloatingControlsToBand (i);
                break;
            }
        }
        repaint (graphBounds);
    }
}

void ProQ3AudioProcessorEditor::mouseUp (const juce::MouseEvent& event)
{
    if (event.eventComponent == &bandDynamicsShortcut)
        return;

    if (draggingSpectrumPeak)
    {
        draggingSpectrumPeak = false;
        spectrumGrabActive = false;
        spectrumGrabHoverSince = 0.0;
        repaint (graphBounds);
    }
    draggingFrequencyView = false;
    waitingForEdgeDrag = false;
    isDraggingNode = false;
    isDraggingDynHandle = false;
    draggingBandIndex = -1;
    if (isSelectionRectDragging)
    {
        isSelectionRectDragging = false;
        selectionLassoRect = {};
        repaint();
    }
}

void ProQ3AudioProcessorEditor::mouseDoubleClick (const juce::MouseEvent& event)
{
    if (event.eventComponent == &bandDynamicsShortcut)
        return;

    if (! graphBounds.contains (event.getPosition()) || event.mods.isPopupMenu())
        return;
    if (isOverFrequencyStrip (event.getPosition()))
    {
        updateFrequencyView (0.0, 1.0, 0.0);
        return;
    }
    if (analyzerPanel.grab.getToggleState()
        && std::any_of (peakTags.begin(), peakTags.end(), [&] (const auto& tag)
                        { return ! tag.isEmpty() && tag.expanded (4.0f, 5.0f).contains (event.position); }))
        return;

    const int hitBand = findBandNodeAtPosition (event.position, 15.0f);
    if (hitBand >= 0)
    {
        attachFloatingControlsToBand (hitBand);
        freqTitleLabel.showEditor();
        return;
    }

    const float freq = static_cast<float> (xToFrequency (event.position.x));
    const float gain = static_cast<float> (yToGainDb (event.position.y));
    const int shape = graphEdgeFilterShape (event.position.x, graphBounds);
    const bool isCut = shape != ProQ3AudioProcessor::kShapeBell;
    const float initGain = (isCut || event.mods.isAltDown()) ? 0.0f : gain;
    const float initDyn = (! isCut && event.mods.isAltDown()) ? gain : 0.0f;

    const int newIdx = audioProcessor.activateFirstFreeBand (freq, initGain, 1.0f, shape, initDyn);
    if (newIdx >= 0)
    {
        attachFloatingControlsToBand (newIdx);
        repaint();
    }
}

void ProQ3AudioProcessorEditor::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.eventComponent == &bandDynamicsShortcut)
        return;

    if (! graphBounds.contains (event.getPosition()))
        return;

    if (isOverFrequencyStrip (event.getPosition()))
    {
        const auto position = (static_cast<double> (event.position.x) - graphBounds.getX()) / graphBounds.getWidth();
        const auto anchor = frequencyViewStart + position * frequencyViewSpan;
        updateFrequencyView (anchor, frequencyViewSpan * std::exp (-wheel.deltaY * 1.7), position);
        return;
    }

    int targetBand = findBandNodeAtPosition (event.position, 28.0f);
    if (targetBand < 0)
        targetBand = audioProcessor.getSelectedBandIndex();

    if (targetBand >= 0 && targetBand < ProQ3AudioProcessor::kMaxBands)
    {
        const auto bp = audioProcessor.getBandParameters (targetBand);
        if (! bp.used)
            return;

        // Manual Pro-Q 3 p. 7 & p. 13:
        // "Move the mouse wheel while holding down Alt to adjust the dynamic range setting"
        // "Hold both Alt and Ctrl to perform a linked change, trading gain for dynamic range"
        if (event.mods.isAltDown())
        {
            if (! ProQ3AudioProcessor::doesShapeSupportDynamics (bp.shape))
                return;

            const double deltaDb = static_cast<double> (wheel.deltaY) * (event.mods.isShiftDown() ? 0.8 : 3.0);
            const double newDyn = juce::jlimit (-30.0, 30.0, bp.dynRangeDb + deltaDb);
            if (auto* pd = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (targetBand, "dyn_range")))
                pd->setValueNotifyingHost (pd->convertTo0to1 (static_cast<float> (newDyn)));

            if (event.mods.isCtrlDown() || event.mods.isCommandDown())
            {
                const double newGain = juce::jlimit (-30.0, 30.0, bp.gainDb - deltaDb);
                if (auto* pg = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (targetBand, "gain")))
                    pg->setValueNotifyingHost (pg->convertTo0to1 (static_cast<float> (newGain)));
            }

            updateFloatingControlsVisibilityAndValues();
            repaint();
            return;
        }

        // Ctrl + roda do mouse ajusta o ganho estático (Manual p. 7)
        if (event.mods.isCtrlDown() || event.mods.isCommandDown())
        {
            const double deltaDb = static_cast<double> (wheel.deltaY) * (event.mods.isShiftDown() ? 0.5 : 2.0);
            const double newGain = juce::jlimit (-30.0, 30.0, bp.gainDb + deltaDb);
            if (auto* pg = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (targetBand, "gain")))
                pg->setValueNotifyingHost (pg->convertTo0to1 (static_cast<float> (newGain)));
            repaint();
            return;
        }

        // Roda do mouse padrão ajusta o Q
        const double step = event.mods.isShiftDown() ? 0.04 : 0.16;
        const double factor = std::pow (2.0, static_cast<double> (wheel.deltaY) * step * 4.0);
        const double newQ = juce::jlimit (0.025, 40.0, bp.q * factor);

        if (auto* pq = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (targetBand, "q")))
            pq->setValueNotifyingHost (pq->convertTo0to1 (static_cast<float> (newQ)));

        repaint();
    }
}

bool ProQ3AudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && matchPanel.isVisible())
    {
        closeMatchPanel();
        return true;
    }

    if (key == juce::KeyPress::escapeKey && analyzerPanel.isVisible())
    {
        if (analyzerPanel.settings.isVisible())
        {
            analyzerPanel.settingsButton.setToggleState (false, juce::dontSendNotification);
            analyzerPanel.settings.setVisible (false);
            layoutAnalyzerPanel();
        }
        else
        {
            dismissAnalyzerPanel();
        }
        return true;
    }

    if (key == juce::KeyPress::escapeKey && outputPanel.isVisible())
    {
        outputPanelKeyboardAccess = false;
        outputPanel.setVisible (false);
        return true;
    }

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        const int sel = audioProcessor.getSelectedBandIndex();
        if (sel >= 0 && audioProcessor.getBandParameters (sel).used)
        {
            audioProcessor.deactivateBand (sel);
            attachFloatingControlsToBand (audioProcessor.getSelectedBandIndex());
            repaint();
            return true;
        }
    }
    return false;
}

//==============================================================================
// Timer de 60 FPS
//==============================================================================
void ProQ3AudioProcessorEditor::timerCallback()
{
    updateOutputPanelVisibility();
    bool needsRepaint = false;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    updateAnalyzerHover (now);
    const auto elapsed = lastSpectrumFrameMs == 0.0 ? 1.0 / 60.0
        : juce::jlimit (0.0, 0.15, (now - lastSpectrumFrameMs) * 0.001);
    lastSpectrumFrameMs = now;

    const auto isOn = [this] (const char* id)
    {
        return audioProcessor.apvts.getRawParameterValue (id)->load() >= 0.5f;
    };
    auto source = SpectrumSource::none;
    if (isOn ("analyzer_peak_alerts") || analyzerPanel.grab.getToggleState())
    {
        if (isOn ("analyzer_post"))     source = SpectrumSource::post;
        else if (isOn ("analyzer_pre")) source = SpectrumSource::pre;
        else if (isOn ("analyzer_sc") && isOn ("analyzer_peak_alerts"))
            source = SpectrumSource::sidechain;
    }

    const auto sampleRate = audioProcessor.getCurrentSampleRate();
    if ((analyzerPanel.isVisible() || analyzerExternalId > 0) && now - lastAnalyzerRefreshMs >= 240.0)
    {
        lastAnalyzerRefreshMs = now;
        refreshAnalyzerPanel();
    }
    if (source != peakSource || ! juce::approximatelyEqual (sampleRate, peakSampleRate))
    {
        spectrumPeaks.fill ({});
        peakSource = source;
        peakSampleRate = sampleRate;
        needsRepaint = true;
    }
    updateSpectrumGrab (now);
    const bool frozen = analyzerPanel.freeze.getToggleState() || spectrumGrabActive;
    updateExternalSpectrum (frozen);
    for (auto& peak : spectrumPeaks)
    {
        if (! frozen && peak.frequency > 0.0 && now - peak.lastSeen >= kPeakHoldMs)
        {
            peak = {};
            needsRepaint = true;
        }
    }

    bool receivedSpectrum = false;
    const auto updateSpectrum = [&] (auto pull, std::vector<float>& display, SpectrumSource stream)
    {
        if (pull (rawSpectrumDb))
        {
            if (matchPanel.isVisible() && stream != SpectrumSource::post)
                updateMatchCapture (rawSpectrumDb, sampleRate, stream == SpectrumSource::sidechain);
            if (frozen)
                return;
            if (source == stream)
                updateSpectrumPeaks (now);
            if (display.size() != rawSpectrumDb.size())
                display = rawSpectrumDb;
            else
            {
                const auto speed = analyzerPanel.speed.getSelectedId();
                const auto falloff = speed == 1 ? 20.0 : speed == 3 ? 72.0 : 38.0;
                for (size_t i = 0; i < display.size(); ++i)
                    display[i] = juce::jmax (rawSpectrumDb[i], display[i] - static_cast<float> (falloff * elapsed));
            }
            receivedSpectrum = true;
            needsRepaint = true;
        }
        else if (! frozen && ! display.empty() && now - spectrumIdleMs > 120.0)
        {
            for (auto& db : display)
                db = juce::jmax (-160.0f, db - static_cast<float> (55.0 * elapsed));
            if (*std::max_element (display.begin(), display.end()) <= -105.0f)
                display.clear();
            needsRepaint = true;
        }
    };
    updateSpectrum ([this] (auto& data) { return audioProcessor.pullNextSidechainFftBlock (data); },
                    smoothedSidechainSpectrumDb, SpectrumSource::sidechain);
    updateSpectrum ([this] (auto& data) { return audioProcessor.pullNextPreFftBlock (data); },
                    smoothedPreSpectrumDb, SpectrumSource::pre);
    updateSpectrum ([this] (auto& data) { return audioProcessor.readPostSpectrum (seenPostSpectrum, data); },
                    smoothedPostSpectrumDb, SpectrumSource::post);
    if (receivedSpectrum)
        spectrumIdleMs = now;
    updateSourcePreviews (now, frozen);
    if (matchPanel.isVisible())
        refreshMatchControls();

    const int procSelected = audioProcessor.getSelectedBandIndex();
    if (procSelected != attachedBandIndex)
        attachFloatingControlsToBand (procSelected);

    updateFloatingControlsVisibilityAndValues();
    if (attachedBandIndex >= 0)
    {
        const auto oldPanel = floatingPillBounds;
        layoutBandControls();
        if (oldPanel != floatingPillBounds)
            needsRepaint = true;
    }
    updateBandDynamicsShortcut();
    refreshStatusControls();

    undoButton.setEnabled (audioProcessor.undoManager.canUndo());
    redoButton.setEnabled (audioProcessor.undoManager.canRedo());

    // Se houver qualquer modulação dinâmica ativa em alguma banda, redesenha a curva e o anel
    for (int i = 0; i < ProQ3AudioProcessor::kMaxBands; ++i)
    {
        if (std::abs (audioProcessor.getBandDynamicGainDb (i)) > 0.02f)
        {
            needsRepaint = true;
            break;
        }
    }

    if (needsRepaint || manualThresholdSlider.isVisible())
        repaint (graphBounds);
    repaint (meterBounds);
}

void ProQ3AudioProcessorEditor::updateSpectrumGrab (double now)
{
    if (draggingSpectrumPeak)
        return;

    const bool eligible = analyzerPanel.grab.getToggleState() && spectrumGrabPointerInGraph
        && ! analyzerPanel.isVisible() && ! matchPanel.isVisible() && ! outputPanel.isVisible() && ! showHelpOverlay
        && ! isDraggingNode && ! isDraggingDynHandle && ! isSelectionRectDragging && ! draggingFrequencyView
        && (peakSource == SpectrumSource::pre || peakSource == SpectrumSource::post);
    if (! eligible || (! spectrumGrabActive && (hoveredBandIndex >= 0 || now - spectrumIdleMs > 250.0)))
    {
        if (spectrumGrabActive)
            repaint (graphBounds);
        spectrumGrabActive = false;
        spectrumGrabHoverSince = 0.0;
        return;
    }

    if (spectrumGrabActive)
        return;

    if (spectrumGrabHoverSince <= 0.0 || spectrumGrabPointer.getDistanceSquaredFrom (spectrumGrabHoverAnchor) > 64)
    {
        spectrumGrabHoverAnchor = spectrumGrabPointer;
        spectrumGrabHoverSince = now;
        return;
    }

    if (now - spectrumGrabHoverSince >= 2000.0
        && std::any_of (spectrumPeaks.begin(), spectrumPeaks.end(), [now] (const auto& peak)
                        { return peak.frequency > 0.0 && now - peak.lastSeen <= kPeakHoldMs
                            && peak.lastSeen - peak.firstSeen >= peakConfirmationMs; }))
    {
        spectrumGrabActive = true;
        repaint (graphBounds);
    }
}

void ProQ3AudioProcessorEditor::updateSpectrumPeaks (double now)
{
    if (rawSpectrumDb.size() < 12)
        return;

    if (peakSpectrumBins != rawSpectrumDb.size())
    {
        peakSpectrumBins = rawSpectrumDb.size();
        spectrumPeaks.fill ({});
    }
    const auto binWidth = peakSampleRate / (2.0 * static_cast<double> (rawSpectrumDb.size() - 1));
    const auto last = static_cast<int> (rawSpectrumDb.size()) - 1;
    const auto read = [this] (int bin) { return rawSpectrumDb[static_cast<size_t> (bin)]; };
    const auto highest = *std::max_element (rawSpectrumDb.begin() + 1, rawSpectrumDb.end() - 1);

    struct Candidate { double frequency; float level; float score; };
    std::vector<Candidate> candidates;

    for (int bin = 2; bin < last - 1; ++bin)
    {
        const auto hz = bin * binWidth;
        if (hz < 45.0 || hz > 20000.0)
            continue;

        const auto level = read (bin);
        if (level < highest - 28.0f || level - ProQ3AudioProcessor::spectrumTiltDb (hz) < -52.0f
            || level <= read (bin - 1) || level <= read (bin + 1))
            continue;

        // Compare both shoulders of the peak, scaling the neighbourhood with frequency.
        const auto offset = juce::jmax (4, juce::roundToInt (bin * 0.105));
        const auto left = juce::jmax (1, bin - offset);
        const auto right = juce::jmin (last - 1, bin + offset);
        if (bin - left < 3 || right - bin < 3)
            continue;

        const auto shoulder = juce::jmax ((read (left) + read (left + 1) + read (left + 2)) / 3.0f,
                                          (read (right - 2) + read (right - 1) + read (right)) / 3.0f);
        const auto prominence = level - shoulder;
        if (prominence < 7.0f)
            continue;

        // Interpolate the summit between FFT bins for a steadier frequency label.
        const auto previous = read (bin - 1);
        const auto next = read (bin + 1);
        const auto shift = 0.5f * (previous - next) / (previous - 2.0f * level + next);
        candidates.push_back ({ (bin + static_cast<double> (shift)) * binWidth,
                                level, level + 0.4f * juce::jmin (prominence, 22.0f) });
    }

    std::sort (candidates.begin(), candidates.end(),
               [] (const auto& a, const auto& b) { return a.score > b.score; });

    std::array<double, 5> accepted {};
    size_t count = 0;
    for (const auto& candidate : candidates)
    {
        const auto nearAccepted = [&] (double frequency)
        {
            return std::abs (frequency - candidate.frequency) <= juce::jmax (2.0 * binWidth, candidate.frequency * 0.04);
        };
        if (std::any_of (accepted.begin(), accepted.begin() + static_cast<std::ptrdiff_t> (count), nearAccepted))
            continue;

        const auto matchesTrack = [&] (double frequency)
        {
            return frequency > 0.0
                && std::abs (frequency - candidate.frequency) <= juce::jmax (1.5 * binWidth, candidate.frequency * 0.012);
        };
        auto slot = std::find_if (spectrumPeaks.begin(), spectrumPeaks.end(),
                                  [&] (const auto& peak) { return matchesTrack (peak.frequency); });
        if (slot == spectrumPeaks.end())
            slot = std::min_element (spectrumPeaks.begin(), spectrumPeaks.end(),
                                     [] (const auto& a, const auto& b) { return a.lastSeen < b.lastSeen; });

        if (! matchesTrack (slot->frequency) || now - slot->lastSeen > 100.0)
            *slot = { candidate.frequency, candidate.level, now, now };
        else
        {
            slot->frequency += 0.24 * (candidate.frequency - slot->frequency);
            slot->level += 0.45f * (candidate.level - slot->level);
            slot->lastSeen = now;
        }

        accepted[count++] = candidate.frequency;
        if (count == accepted.size())
            break;
    }
}

void ProQ3AudioProcessorEditor::drawSpectrumPeaks (juce::Graphics& g)
{
    peakTags.fill ({});
    if (! spectrumGrabActive && audioProcessor.apvts.getRawParameterValue ("analyzer_peak_alerts")->load() < 0.5f)
        return;

    const juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (graphBounds);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    const auto usable = graphBounds.reduced (9, 5).withTrimmedRight (43).toFloat();
    std::array<juce::Rectangle<float>, 5> labels {};
    size_t count = 0;
    for (const auto& peak : spectrumPeaks)
    {
        if (peak.frequency <= 0.0 || peak.lastSeen - peak.firstSeen < peakConfirmationMs)
            continue;

        const auto x = frequencyToX (peak.frequency);
        if (x < static_cast<float> (graphBounds.getX()) || x > static_cast<float> (graphBounds.getRight()))
            continue;
        const auto tiltDb = getAnalyzerTiltChange() * std::log2 (peak.frequency / 1000.0);
        const auto y = analyzerDbToY (peak.level + tiltDb);
        const bool useNotes = spectrumGrabActive
            && audioProcessor.apvts.getRawParameterValue ("piano_display")->load() >= 0.5f;
        const auto text = useNotes ? frequencyToMusicalNoteName (peak.frequency)
            : peak.frequency >= 1000.0 ? juce::String (peak.frequency * 0.001, 2) + " kHz"
                                      : juce::String (juce::roundToInt (peak.frequency)) + " Hz";
        constexpr float width = 61.0f;
        constexpr float height = 16.0f;
        auto tag = juce::Rectangle<float> (juce::jlimit (usable.getX(), usable.getRight() - width, x - width / 2.0f),
                                           juce::jmax (usable.getY(), y - 23.0f), width, height);
        const auto overlapsLabel = [&] (const auto& other) { return tag.expanded (3.0f, 1.0f).intersects (other); };
        const auto end = labels.begin() + static_cast<std::ptrdiff_t> (count);
        while (tag.getY() >= usable.getY() && std::any_of (labels.begin(), end, overlapsLabel))
            tag.translate (0.0f, -18.0f);
        if (tag.getY() < usable.getY())
            continue;

        labels[count] = tag;
        const juce::Rectangle<float> summit (x - 5.0f, y - 4.0f, 10.0f, 8.0f);
        peakTags[count] = tag.getUnion (summit);
        peakTagFrequencies[count++] = peak.frequency;
        g.setColour (kEqGold.withAlpha (0.78f));
        g.drawLine (x, y, juce::jlimit (tag.getX() + 4.0f, tag.getRight() - 4.0f, x), tag.getBottom(), 1.0f);
        g.fillEllipse (x - 2.0f, y - 2.0f, 4.0f, 4.0f);
        g.setColour (spectrumGrabActive ? juce::Colour (0xffe8b952) : juce::Colour (0xff3b2e20));
        g.fillRoundedRectangle (tag, 4.0f);
        g.setColour (kEqGold.withAlpha (0.82f));
        g.drawRoundedRectangle (tag.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (spectrumGrabActive ? juce::Colour (0xff2b1f19) : juce::Colour (0xffffdb88));
        g.drawFittedText (text, tag.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
    }
}

//==============================================================================
// Renderização da Interface e do Painel da Banda com Crista Dinâmica ("Auto")
//==============================================================================
void ProQ3AudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBgDark);

    const auto clip = g.getClipBounds();
    if (clip.intersects (topBarBounds))
        drawTopToolbar (g, topBarBounds);

    if (clip.intersects (graphBounds))
    {
        drawEqGridAndScales (g, graphBounds);
        drawSpectrumAnalyzer (g, graphBounds);
        if (spectrumGrabActive)
        {
            const juce::Graphics::ScopedSaveState saved (g);
            g.reduceClipRegion (graphBounds);
            g.beginTransparencyLayer (0.26f);
            drawEqCurvesAndBandNodes (g, graphBounds);
            g.endTransparencyLayer();
        }
        else
        {
            drawEqCurvesAndBandNodes (g, graphBounds);
        }
        drawMatchSpectra (g, graphBounds);
        drawSpectrumPeaks (g);

        const int tooltipBand = (draggingBandIndex >= 0) ? draggingBandIndex : hoveredBandIndex;
        if (! spectrumGrabActive && tooltipBand >= 0 && tooltipBand < ProQ3AudioProcessor::kMaxBands)
            drawBandParameterTooltip (g, tooltipBand);

        if (audioProcessor.apvts.getRawParameterValue ("piano_display")->load() >= 0.5f)
            drawPianoRollBar (g, pianoBounds);

        drawFloatingBandControlsBackground (g);
        g.setColour (kFloatingBorder.withAlpha (0.65f));
        g.drawRect (graphBounds, 1);
    }

    if (clip.intersects (meterBounds))
        drawOutputMeter (g, meterBounds);
    if (clip.intersects (bottomBarBounds))
        drawBottomToolbar (g, bottomBarBounds);

    if (showHelpOverlay)
    {
        auto helpBox = graphBounds.reduced (130, 65).toFloat();
        g.setColour (juce::Colour (0xf2181b22));
        g.fillRoundedRectangle (helpBox, 8.0f);
        g.setColour (kControlAccent);
        g.drawRoundedRectangle (helpBox, 8.0f, 1.5f);

        g.setColour (kTextBright);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText ("NX Audio Pro EQ³",
                    helpBox.removeFromTop (34.0f).toNearestInt(), juce::Justification::centred, false);

        g.setFont (juce::FontOptions (12.5f));
        g.setColour (kTextMuted);
        const juce::String helpText =
            "- GAIN Inner Knob: Adjust Static Gain (-30 to +30 dB) | Outer Ring: Independent drag for Dynamic Range (-30 to +30 dB)\n"
            "- Auto Button (top of GAIN ring): Toggle Automatic Threshold vs Manual Threshold slider + detector meter & External Sidechain (SC)\n"
            "- Power Icon near ring: Bypass Dynamics only (keeps static gain) | 'x' near ring: Clear Dynamics to 0 dB without deleting band\n"
            "- Alt + Mouse Wheel over band dot (or 'Make Dynamic' in Right-Click menu): Adjust Dynamic Range directly\n"
            "- 9 Filter Shapes: Bell, Low Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt (6-96 dB/oct + Brickwall)\n"
            "- Double-click the left or right edge of the graph to create a Low Cut or High Cut.\n"
            "- Double-Click FREQ label to type exact values with shortcuts: '2k' (2000 Hz), 'A4' (440 Hz), 'C#3 + 13', or '50%'";
        g.drawFittedText (helpText, helpBox.reduced (22.0f, 8.0f).toNearestInt(), juce::Justification::topLeft, 10);
    }
}

void ProQ3AudioProcessorEditor::drawTopToolbar (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    juce::ColourGradient topGrad (juce::Colour (0xff25212a), 0.0f, static_cast<float> (bounds.getY()),
                                  juce::Colour (0xff18151e), 0.0f, static_cast<float> (bounds.getBottom()), false);
    g.setGradientFill (topGrad);
    g.fillRect (bounds);

    g.setColour (juce::Colour (0xff37313c));
    g.drawHorizontalLine (bounds.getBottom() - 1, static_cast<float> (bounds.getX()), static_cast<float> (bounds.getRight()));

    auto presetRail = prevPresetButton.getBounds().getUnion (nextPresetButton.getBounds()).toFloat();
    g.setColour (juce::Colour (0xff201c25));
    g.fillRoundedRectangle (presetRail, 5.0f);
    g.setColour (juce::Colour (0xff332c38));
    g.drawRoundedRectangle (presetRail.reduced (0.5f), 5.0f, 1.0f);

    const float cy = static_cast<float> (bounds.getCentreY());
    const juce::Rectangle<float> emblem (12.0f, cy - 12.0f, 39.0f, 24.0f);
    g.setColour (kEqGold.withAlpha (0.09f));
    g.fillRoundedRectangle (emblem, 6.0f);
    g.setColour (kEqGold.withAlpha (0.34f));
    g.drawRoundedRectangle (emblem, 6.0f, 1.0f);

    juce::Path mark;
    mark.startNewSubPath (18.5f, cy + 5.0f);
    mark.lineTo (18.5f, cy - 5.0f);
    mark.lineTo (28.0f, cy + 5.0f);
    mark.lineTo (28.0f, cy - 5.0f);
    mark.startNewSubPath (34.0f, cy + 5.0f);
    mark.lineTo (44.0f, cy - 5.0f);
    mark.startNewSubPath (34.0f, cy - 5.0f);
    mark.lineTo (44.0f, cy + 5.0f);
    juce::ColourGradient gold (juce::Colour (0xffffdb83), emblem.getTopLeft(),
                                juce::Colour (0xffe5a629), emblem.getBottomRight(), false);
    g.setGradientFill (gold);
    g.strokePath (mark, juce::PathStrokeType (1.65f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.fillEllipse (42.5f, cy - 6.5f, 3.1f, 3.1f);

    g.setColour (juce::Colour (0xffebcf9b));
    g.setFont (juce::FontOptions (14.5f, juce::Font::bold));
    g.drawFittedText ("NX Audio", 59, bounds.getY(), 66, bounds.getHeight(), juce::Justification::centredLeft, 1);

    const juce::Font modelFont { juce::FontOptions (17.0f, juce::Font::bold) };
    const auto baseline = cy + 5.6f;
    juce::GlyphArrangement model;
    model.addLineOfText (modelFont, "Pro EQ", 132.0f, baseline);
    model.addLineOfText (juce::Font { juce::FontOptions (10.0f, juce::Font::bold) }, "3",
                         model.getBoundingBox (0, -1, false).getRight() + 2.0f, baseline - 6.2f);
    g.setColour (kEqGold);
    model.draw (g);
}

void ProQ3AudioProcessorEditor::drawEqGridAndScales (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.saveState();
    g.reduceClipRegion (area);

    juce::ColourGradient canvas (juce::Colour (0xff1a181b), 0.0f, static_cast<float> (area.getY()),
                                  juce::Colour (0xff1d1b1e), 0.0f, static_cast<float> (area.getBottom()), false);
    g.setGradientFill (canvas);
    g.fillRect (area);

    const std::array<double, 27> minorFreqs {
        30.0, 40.0, 60.0, 70.0, 80.0, 90.0,
        150.0, 300.0, 400.0, 600.0, 700.0, 800.0, 900.0,
        1500.0, 3000.0, 4000.0, 6000.0, 7000.0, 8000.0, 9000.0,
        12000.0, 14000.0, 16000.0, 18000.0, 22000.0, 25000.0, 30000.0
    };

    g.setColour (kGridLineMinor);
    for (double f : minorFreqs)
    {
        const float x = std::round (frequencyToX (f));
        g.drawVerticalLine (static_cast<int> (x), static_cast<float> (area.getY()), static_cast<float> (area.getBottom()));
    }

    struct FreqMarker { double freq; const char* label; };
    const std::array<FreqMarker, 10> majorFreqs {{
        { 20.0,    "20"  },
        { 50.0,    "50"  },
        { 100.0,   "100" },
        { 200.0,   "200" },
        { 500.0,   "500" },
        { 1000.0,  "1k"  },
        { 2000.0,  "2k"  },
        { 5000.0,  "5k"  },
        { 10000.0, "10k" },
        { 20000.0, "20k" }
    }};

    g.setFont (juce::FontOptions (11.5f));
    for (const auto& m : majorFreqs)
    {
        const float x = std::round (frequencyToX (m.freq));
        g.setColour (kGridLineMajor);
        g.drawVerticalLine (static_cast<int> (x), static_cast<float> (area.getY()), static_cast<float> (area.getBottom()));

        g.setColour (kTextMuted);
        g.drawText (m.label, static_cast<int> (x) - 18, area.getBottom() - 18, 36, 14, juce::Justification::centred, false);
    }

    const double maxDb = getCurrentDisplayRangeDb();
    double stepDb = 3.0;
    if (maxDb <= 3.1)       stepDb = 1.0;
    else if (maxDb <= 6.1)  stepDb = 2.0;
    else if (maxDb <= 12.1) stepDb = 3.0;
    else                    stepDb = 6.0;

    for (double db = -maxDb; db <= maxDb + 0.01; db += stepDb)
    {
        const float y = std::round (gainDbToY (db));
        const bool isZero = (std::abs (db) < 0.01);

        if (! isZero)
        {
            g.setColour (kGridLineMajor);
            g.drawHorizontalLine (static_cast<int> (y), static_cast<float> (area.getX()), static_cast<float> (area.getRight()));
        }

        juce::String lbl = isZero ? "0" : ((db > 0.0 ? "+" : "") + juce::String (static_cast<int> (std::lround (db))));
        g.setColour (juce::Colour (0xffd9a226));
        g.drawText (lbl, area.getRight() - 40, static_cast<int> (y) - 7, 29, 14, juce::Justification::centredRight, false);
    }

    // Keep the fixed 0 dB line underneath the EQ response and its overlays.
    g.setColour (kGridZeroLine);
    g.drawLine (static_cast<float> (area.getX()), gainDbToY (0.0),
                static_cast<float> (area.getRight()), gainDbToY (0.0), 1.2f);

    g.restoreState();
}

void ProQ3AudioProcessorEditor::drawSpectrumAnalyzer (juce::Graphics& g, juce::Rectangle<int> area)
{
    const bool showPre  = audioProcessor.apvts.getRawParameterValue ("analyzer_pre")->load() >= 0.5f;
    const bool showPost = audioProcessor.apvts.getRawParameterValue ("analyzer_post")->load() >= 0.5f;
    const bool showSidechain = audioProcessor.apvts.getRawParameterValue ("analyzer_sc")->load() >= 0.5f;

    if (! showPre && ! showPost && ! showSidechain && externalSpectrumDb.empty())
        return;

    g.saveState();
    g.reduceClipRegion (area);

    const auto sampleRate = audioProcessor.getCurrentSampleRate();
    const auto tiltChange = getAnalyzerTiltChange();
    const auto detail = analyzerPanel.detail.getSelectedId();
    const auto blendWidth = detail == 1 ? 0.006f : detail == 3 ? 0.0f : 0.0025f;

    auto buildSpectrumPath = [&] (const std::vector<float>& magDb, double rate, juce::Path& strokePath, juce::Path& fillPath)
    {
        const auto right = juce::jmin (area.getRight(), juce::roundToInt (frequencyToX (rate * 0.5)));
        if (magDb.size() < 3 || right <= area.getX()
            || *std::max_element (magDb.begin(), magDb.end()) <= -105.0f)
            return;

        const auto bottomY = static_cast<float> (area.getBottom());
        const auto magnitudeAtX = [&] (float x)
        {
            const auto frequency = juce::jmin (rate * 0.5, xToFrequency (x));
            return sampleSpectrum (magDb, frequency, rate)
                + static_cast<float> (tiltChange * std::log2 (juce::jmax (20.0, frequency) / 1000.0));
        };
        for (int px = area.getX(); ; px = juce::jmin (right, px + 2))
        {
            const auto x = static_cast<float> (px);
            const auto width = static_cast<float> (area.getWidth()) * blendWidth;
            const auto db = (magnitudeAtX (x - width) + 2.0f * magnitudeAtX (x) + magnitudeAtX (x + width)) * 0.25f;
            const auto y = analyzerDbToY (db);
            if (px == area.getX())
                strokePath.startNewSubPath (x, y);
            else
                strokePath.lineTo (x, y);
            if (px == right)
                break;
        }

        fillPath = strokePath;
        fillPath.lineTo (static_cast<float> (right), bottomY);
        fillPath.lineTo (static_cast<float> (area.getX()), bottomY);
        fillPath.closeSubPath();
    };

    if ((showPre || showPost) && (showSidechain || analyzerExternalId > 0)
        && audioProcessor.apvts.getRawParameterValue ("analyzer_overlap")->load() >= 0.5f)
    {
        const auto steps = static_cast<double> (activeSpectrumOverlap.size() - 1);
        for (size_t i = 0; i < activeSpectrumOverlap.size(); ++i)
        {
            const auto strength = activeSpectrumOverlap[i];
            if (strength < 0.025f)
                continue;
            const auto position = static_cast<double> (i);
            const auto left = frequencyToX (analyzerPreviewFrequency (juce::jmax (0.0, position - 0.5) / steps));
            const auto right = frequencyToX (analyzerPreviewFrequency (juce::jmin (steps, position + 0.5) / steps));
            const auto top = static_cast<float> (area.getY());
            const auto bottom = static_cast<float> (area.getBottom());
            g.setColour (kSpectrumOverlapColour.withAlpha (0.30f * strength));
            g.fillRect (left, top, right - left, bottom - top);
        }
    }

    const auto paintGrabTrace = [&] (const juce::Path& stroke, const juce::Path& fill)
    {
        juce::ColourGradient focus (juce::Colour (0x9a8652b2), area.toFloat().getTopLeft(),
                                     juce::Colour (0x3173469b), area.toFloat().getBottomLeft(), false);
        g.setGradientFill (focus);
        g.fillPath (fill);
        g.setColour (juce::Colour (0xfff7edf7));
        g.strokePath (stroke, juce::PathStrokeType (1.45f, juce::PathStrokeType::curved));
    };

    if (showPre)
    {
        juce::Path stroke, fill;
        buildSpectrumPath (smoothedPreSpectrumDb, sampleRate, stroke, fill);
        if (spectrumGrabActive && ! showPost)
            paintGrabTrace (stroke, fill);
        else
        {
            g.setColour (juce::Colour (0x287f8193));
            g.fillPath (fill);
            g.setColour (juce::Colour (0x608e90a1));
            g.strokePath (stroke, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
        }
    }

    if (showPost)
    {
        juce::Path stroke, fill;
        buildSpectrumPath (smoothedPostSpectrumDb, sampleRate, stroke, fill);
        if (spectrumGrabActive)
            paintGrabTrace (stroke, fill);
        else
        {
            juce::ColourGradient spectrum (juce::Colour (0x779997a4), 0.0f, static_cast<float> (area.getY()),
                                            juce::Colour (0x28918f9c), 0.0f, static_cast<float> (area.getBottom()), false);
            g.setGradientFill (spectrum);
            g.fillPath (fill);
            g.setColour (juce::Colour (0xa5c3c0cc));
            g.strokePath (stroke, juce::PathStrokeType (1.15f, juce::PathStrokeType::curved));
        }
    }

    if (showSidechain)
    {
        juce::Path stroke, fill;
        buildSpectrumPath (smoothedSidechainSpectrumDb, sampleRate, stroke, fill);
        g.setColour (juce::Colour (0x24d17284));
        g.fillPath (fill);
        g.setColour (juce::Colour (0xaaee8996));
        g.strokePath (stroke, juce::PathStrokeType (1.1f, juce::PathStrokeType::curved));
    }

    if (analyzerExternalId > 0 && ! externalSpectrumDb.empty())
    {
        juce::Path stroke, fill;
        buildSpectrumPath (externalSpectrumDb, externalSampleRate, stroke, fill);
        g.setColour (kEqGold.withAlpha (0.12f));
        g.fillPath (fill);
        g.setColour (kEqGold.withAlpha (0.79f));
        g.strokePath (stroke, juce::PathStrokeType (1.25f, juce::PathStrokeType::curved));
    }

    g.restoreState();
}

void ProQ3AudioProcessorEditor::drawEqCurvesAndBandNodes (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.saveState();
    g.reduceClipRegion (area);

    const float zeroY = gainDbToY (0.0);
    const int selectedBand = audioProcessor.getSelectedBandIndex();
    const bool globalBypass = audioProcessor.apvts.getRawParameterValue ("global_bypass")->load() >= 0.5f;

    // Calculate each band's coefficients once per frame and reuse its samples in the total.
    const auto pointCount = static_cast<size_t> (area.getWidth() / 2 + 1);
    std::vector<double> frequencies (pointCount), totalLiveDb (pointCount), totalStaticDb (pointCount);
    for (size_t point = 0; point < pointCount; ++point)
        frequencies[point] = xToFrequency (static_cast<float> (area.getX() + 2 * static_cast<int> (point)));

    const auto toY = [this] (double db) { return gainDbToY (db); };

    struct BandOverlay
    {
        juce::Path edge, fill, dynamicRange;
        juce::Colour colour;
        float opacity;
        bool highlighted;
    };
    std::vector<BandOverlay> overlays;
    std::vector<float> dynamicEdgeY;

    bool hasLiveModulation = false;
    for (int b = 0; b < ProQ3AudioProcessor::kMaxBands; ++b)
    {
        const auto bp = audioProcessor.getBandParameters (b);
        if (! bp.used)
            continue;

        const bool isSelected = (b == selectedBand);
        const bool isHovered = (b == hoveredBandIndex);
        const bool hasDyn = ProQ3AudioProcessor::doesShapeSupportDynamics (bp.shape) && std::abs (bp.dynRangeDb) > 0.05;
        const bool showBand = isSelected || isHovered || std::abs (bp.gainDb) >= 0.02 || hasDyn
                              || ! ProQ3AudioProcessor::doesShapeSupportGain (bp.shape);
        if (! bp.enabled && ! showBand)
            continue;

        const auto dynamicGain = audioProcessor.getBandDynamicGainDb (b);
        const bool isModulated = std::abs (dynamicGain) > 1.0e-6f;
        hasLiveModulation |= bp.enabled && isModulated;
        const auto staticResponse = audioProcessor.makeBandResponse (bp);
        const auto liveResponse = isModulated ? audioProcessor.makeBandResponse (bp, dynamicGain) : staticResponse;
        const bool drawRange = hasDyn && bp.enabled && bp.dynEnabled && ! globalBypass;
        const auto rangeResponse = drawRange ? audioProcessor.makeBandResponse (bp, bp.dynRangeDb) : liveResponse;
        if (drawRange)
            dynamicEdgeY.resize (pointCount);

        juce::Path bandLiveCurve;
        juce::Path bandFill;

        for (size_t point = 0; point < pointCount; ++point)
        {
            const double liveDb = liveResponse.magnitudeDbAtFrequency (frequencies[point]);
            if (bp.enabled)
            {
                totalLiveDb[point] += liveDb;
                totalStaticDb[point] += isModulated ? staticResponse.magnitudeDbAtFrequency (frequencies[point]) : liveDb;
            }

            if (showBand)
            {
                const float x = static_cast<float> (area.getX() + 2 * static_cast<int> (point));
                const float y = toY (liveDb);
                if (drawRange)
                    dynamicEdgeY[point] = toY (rangeResponse.magnitudeDbAtFrequency (frequencies[point]));
                if (point == 0)
                {
                    bandLiveCurve.startNewSubPath (x, y);
                    bandFill.startNewSubPath (x, zeroY);
                }
                else
                {
                    bandLiveCurve.lineTo (x, y);
                }
                bandFill.lineTo (x, y);
            }
        }

        if (! showBand)
            continue;

        bandFill.lineTo (static_cast<float> (area.getRight()), zeroY);
        bandFill.closeSubPath();
        juce::Path dynamicFill;
        if (drawRange)
        {
            dynamicFill = bandLiveCurve;
            for (size_t point = pointCount; point > 0; --point)
            {
                const auto index = point - 1;
                const auto x = static_cast<float> (area.getX() + 2 * static_cast<int> (index));
                dynamicFill.lineTo (x, dynamicEdgeY[index]);
            }
            dynamicFill.closeSubPath();
        }

        overlays.push_back ({ std::move (bandLiveCurve), std::move (bandFill), std::move (dynamicFill),
                              getBandColour (b), bp.enabled && ! globalBypass ? 1.0f : 0.33f, isSelected || isHovered });
    }

    juce::Path totalLiveCurve;
    juce::Path totalStaticCurve;
    juce::Path totalFill;
    totalFill.startNewSubPath (static_cast<float> (area.getX()), zeroY);
    totalLiveCurve.startNewSubPath (static_cast<float> (area.getX()), toY (totalLiveDb.front()));
    totalStaticCurve.startNewSubPath (static_cast<float> (area.getX()), toY (totalStaticDb.front()));

    for (size_t point = 0; point < pointCount; ++point)
    {
        const float x = static_cast<float> (area.getX() + 2 * static_cast<int> (point));
        const float y = toY (totalLiveDb[point]);
        totalLiveCurve.lineTo (x, y);
        totalFill.lineTo (x, y);
        totalStaticCurve.lineTo (x, toY (totalStaticDb[point]));
    }

    totalFill.lineTo (static_cast<float> (area.getRight()), zeroY);
    totalFill.closeSubPath();
    g.setColour (kEqGold.withAlpha (globalBypass ? 0.04f : 0.12f));
    g.fillPath (totalFill);

    for (const auto& overlay : overlays)
    {
        g.setColour (overlay.colour.withAlpha ((overlay.highlighted ? 0.25f : 0.18f) * overlay.opacity));
        g.fillPath (overlay.fill);
        if (! overlay.dynamicRange.isEmpty())
        {
            g.setColour (overlay.colour.withAlpha (overlay.highlighted ? 0.17f : 0.11f));
            g.fillPath (overlay.dynamicRange);
        }
        g.setColour (overlay.colour.withAlpha ((overlay.highlighted ? 0.78f : 0.55f) * overlay.opacity));
        g.strokePath (overlay.edge, juce::PathStrokeType (overlay.highlighted ? 1.2f : 1.0f));
    }

    // Traço fantasma da curva estática quando a dinâmica está atuando
    if (hasLiveModulation)
    {
        g.setColour (kTextMuted.withAlpha (0.6f));
        g.strokePath (totalStaticCurve, juce::PathStrokeType (1.2f));
    }

    // Brilho e curva principal acompanhando em tempo real o ganho aplicado pelo DSP
    g.setColour (kEqGlow.withMultipliedAlpha (globalBypass ? 0.3f : 1.0f));
    g.strokePath (totalLiveCurve, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (globalBypass ? juce::Colour (0xff7e7352) : kEqGold);
    g.strokePath (totalLiveCurve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // 3. Nós numerados das bandas ativas (1 a 24) + Indicador de Dynamic Range no display
    g.setFont (juce::FontOptions (7.8f, juce::Font::bold));
    for (int b = 0; b < ProQ3AudioProcessor::kMaxBands; ++b)
    {
        const auto bp = audioProcessor.getBandParameters (b);
        if (! bp.used)
            continue;

        const float nx = frequencyToX (bp.freqHz);
        const float ny = gainDbToY (bp.gainDb);
        const bool isSelected = (b == selectedBand);
        const bool isHovered  = (b == hoveredBandIndex);
        const juce::Colour nodeCol = getBandColour (b);

        // Se a banda tem Dynamic Range != 0, desenha a haste vertical de alcance dinâmico e o ponto da redução atual (Manual p. 7 & p. 13)
        if (ProQ3AudioProcessor::doesShapeSupportDynamics (bp.shape) && std::abs (bp.dynRangeDb) > 0.05)
        {
            const float dynTargetY = gainDbToY (bp.gainDb + bp.dynRangeDb);
            const float liveModDb  = audioProcessor.getBandDynamicGainDb (b);
            const float liveY      = gainDbToY (bp.gainDb + static_cast<double> (liveModDb));

            g.setColour (bp.enabled && bp.dynEnabled ? nodeCol.withAlpha (0.78f) : juce::Colour (0xff666a75));
            g.drawLine (nx, ny, nx, dynTargetY, 1.8f);

            juce::Path handles;
            handles.addTriangle (nx - 3.0f, dynTargetY - 2.0f, nx + 3.0f, dynTargetY - 2.0f, nx, dynTargetY - 6.0f);
            handles.addTriangle (nx - 3.0f, dynTargetY + 2.0f, nx + 3.0f, dynTargetY + 2.0f, nx, dynTargetY + 6.0f);
            g.setColour (juce::Colour (0xffd5d4da).withAlpha (bp.enabled && bp.dynEnabled ? 0.55f : 0.28f));
            g.fillPath (handles);

            if (bp.dynEnabled && std::abs (liveModDb) > 0.03f)
            {
                g.setColour (bp.enabled ? nodeCol.brighter (0.28f) : juce::Colour (0xff666a75));
                g.drawLine (nx, ny, nx, liveY, 3.2f);
                g.fillEllipse (nx - 3.5f, liveY - 3.5f, 7.0f, 7.0f);
            }
        }

        const float radius = isSelected ? 6.3f : (isHovered ? 5.8f : 5.2f);
        const juce::Rectangle<float> dotRect (nx - radius, ny - radius, radius * 2.0f, radius * 2.0f);

        if (isSelected || isHovered)
        {
            g.setColour (nodeCol.withAlpha (0.23f));
            g.fillEllipse (dotRect.expanded (3.0f));
        }

        const auto dotColour = bp.enabled ? nodeCol : juce::Colour (0xff68717e);
        juce::ColourGradient dot (dotColour.brighter (0.38f), nx - radius * 0.32f, ny - radius * 0.45f,
                                  dotColour.darker (0.23f), nx + radius * 0.8f, ny + radius * 0.8f, true);
        g.setGradientFill (dot);
        g.fillEllipse (dotRect);
        g.setColour (juce::Colour (0xff121214));
        g.drawEllipse (dotRect, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.53f));
        g.fillEllipse (nx - radius * 0.55f, ny - radius * 0.62f, radius * 0.46f, radius * 0.31f);
        g.setColour (juce::Colour (0xff15141b));
        g.drawText (juce::String (b + 1), dotRect.toNearestInt(), juce::Justification::centred, false);
    }

    if (isSelectionRectDragging && ! selectionLassoRect.isEmpty())
    {
        g.setColour (kControlAccent.withAlpha (0.15f));
        g.fillRect (selectionLassoRect);
        g.setColour (kControlAccent.withAlpha (0.7f));
        g.drawRect (selectionLassoRect, 1.0f);
    }

    g.restoreState();
}

juce::Rectangle<int> ProQ3AudioProcessorEditor::getBandTooltipBounds (
    const ProQ3AudioProcessor::BandParameters& band) const noexcept
{
    const auto x = juce::roundToInt (frequencyToX (band.freqHz));
    const auto y = juce::roundToInt (gainDbToY (band.gainDb));
    constexpr int width = 172;
    constexpr int height = 68;
    const auto area = graphBounds.reduced (8);
    const int top = y - height - 20;
    return { juce::jlimit (area.getX(), area.getRight() - width, x - width / 2),
             juce::jlimit (area.getY(), area.getBottom() - height, top < area.getY() ? y + 20 : top),
             width, height };
}

bool ProQ3AudioProcessorEditor::isOverBandTooltip (juce::Point<int> position) const noexcept
{
    const auto band = audioProcessor.getBandParameters (hoveredBandIndex);
    if (! band.used || ! ProQ3AudioProcessor::doesShapeSupportDynamics (band.shape))
        return false;

    const auto node = juce::Point<float> (frequencyToX (band.freqHz), gainDbToY (band.gainDb)).roundToInt();
    const juce::Rectangle<int> nodeBounds (node.x - 10, node.y - 10, 20, 20);
    return getBandTooltipBounds (band).getUnion (nodeBounds).contains (position);
}

void ProQ3AudioProcessorEditor::updateBandDynamicsShortcut()
{
    const auto band = audioProcessor.getBandParameters (hoveredBandIndex);
    const bool visible = band.used && ProQ3AudioProcessor::doesShapeSupportDynamics (band.shape)
        && draggingBandIndex < 0 && ! showHelpOverlay && ! spectrumGrabActive;
    if (visible)
    {
        const auto card = getBandTooltipBounds (band);
        bandDynamicsShortcut.setBounds (card.getRight() - 24, card.getY() + 3, 20, 19);
    }
    bandDynamicsShortcut.setVisible (visible);
}

void ProQ3AudioProcessorEditor::drawBandParameterTooltip (juce::Graphics& g, int bandIndex)
{
    const auto bp = audioProcessor.getBandParameters (bandIndex);
    if (! bp.used)
        return;

    const auto box = getBandTooltipBounds (bp);
    g.setColour (juce::Colour (0xeb1c1f26));
    g.fillRoundedRectangle (box.toFloat(), 5.0f);
    g.setColour (juce::Colour (0xff4a505e));
    g.drawRoundedRectangle (box.toFloat(), 5.0f, 1.0f);

    static const char* shapeNames[9] = {
        "Bell", "Low Shelf", "Low Cut", "High Shelf", "High Cut",
        "Notch", "Band Pass", "Tilt Shelf", "Flat Tilt"
    };

    const juce::String freqStr = (bp.freqHz >= 1000.0)
                                    ? (juce::String (bp.freqHz / 1000.0, 2) + " kHz")
                                    : (juce::String (bp.freqHz, 1) + " Hz");
    const juce::String noteStr = frequencyToMusicalNoteName (bp.freqHz);
    const juce::String gainStr = (bp.gainDb >= 0.0 ? "+" : "") + juce::String (bp.gainDb, 2) + " dB";
    const float liveDyn = audioProcessor.getBandDynamicGainDb (bandIndex);
    const juce::String dynStr  = (std::abs (bp.dynRangeDb) > 0.05)
                                    ? (juce::String ("  | Dyn: ") + (bp.dynRangeDb >= 0.0 ? "+" : "") + juce::String (bp.dynRangeDb, 1)
                                       + " (" + (liveDyn >= 0.0f ? "+" : "") + juce::String (liveDyn, 1) + " dB)")
                                    : "";
    const juce::String qStr    = "Q: " + juce::String (bp.q, 2) + "  (" + shapeNames[juce::jlimit (0, 8, bp.shape)] + ")";

    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    g.setColour (kTextBright);
    const auto topTextWidth = box.getWidth() - (bandDynamicsShortcut.isVisible() ? 34 : 16);
    g.drawFittedText (freqStr + "  [" + noteStr + "]", box.getX() + 8, box.getY() + 5, topTextWidth, 15,
                      juce::Justification::centredLeft, 1);

    g.setColour (getBandColour (bandIndex));
    g.drawFittedText (gainStr + dynStr, box.getX() + 8, box.getY() + 23, box.getWidth() - 16, 15,
                      juce::Justification::centredLeft, 1);

    g.setFont (juce::FontOptions (11.0f));
    g.setColour (kTextMuted);
    g.drawText (qStr, box.getX() + 8, box.getY() + 42, box.getWidth() - 16, 15, juce::Justification::centredLeft, false);
}

void ProQ3AudioProcessorEditor::drawPianoRollBar (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (juce::Colour (0xff16181d));
    g.fillRect (area);

    auto isBlackKey = [] (int midiNote) noexcept
    {
        const int n = midiNote % 12;
        return (n == 1 || n == 3 || n == 6 || n == 8 || n == 10);
    };

    for (int midi = 21; midi <= 108; ++midi)
    {
        const double fCenter = 440.0 * std::pow (2.0, (static_cast<double> (midi) - 69.0) / 12.0);
        const double fLow    = 440.0 * std::pow (2.0, (static_cast<double> (midi) - 69.5) / 12.0);
        const double fHigh   = 440.0 * std::pow (2.0, (static_cast<double> (midi) - 68.5) / 12.0);

        const float x0 = frequencyToX (fLow);
        const float x1 = frequencyToX (fHigh);
        juce::Rectangle<float> keyRect (x0, static_cast<float> (area.getY()), juce::jmax (2.0f, x1 - x0), static_cast<float> (area.getHeight()));

        const bool black = isBlackKey (midi);
        g.setColour (black ? juce::Colour (0xff20232a) : juce::Colour (0xff383d49));
        g.fillRect (keyRect.reduced (0.4f, 1.0f));

        if (midi % 12 == 0)
        {
            const int oct = (midi / 12) - 1;
            g.setColour (kTextBright);
            g.setFont (juce::FontOptions (9.5f));
            g.drawText ("C" + juce::String (oct), static_cast<int> (frequencyToX (fCenter)) - 10, area.getBottom() - 12, 20, 11, juce::Justification::centred, false);
        }
    }

    for (int b = 0; b < ProQ3AudioProcessor::kMaxBands; ++b)
    {
        const auto bp = audioProcessor.getBandParameters (b);
        if (! bp.used)
            continue;

        const float x = frequencyToX (bp.freqHz);
        const float y = static_cast<float> (area.getCentreY());
        g.setColour (getBandColour (b));
        g.fillEllipse (x - 4.0f, y - 4.0f, 8.0f, 8.0f);
    }
}

void ProQ3AudioProcessorEditor::drawFloatingBandControlsBackground (juce::Graphics& g)
{
    if (attachedBandIndex < 0 || attachedBandIndex >= ProQ3AudioProcessor::kMaxBands)
        return;

    const auto bp = audioProcessor.getBandParameters (attachedBandIndex);
    if (! bp.used)
        return;

    auto pill = floatingPillBounds.toFloat();
    const float humpCenterX = pill.getCentreX() - 4.0f;
    const float humpTopY    = pill.getY() - 18.0f;

    // The raised centre leaves room for the dynamic controls above the gain knob.
    juce::Path pillShape;
    const float r = 8.0f;
    pillShape.startNewSubPath (pill.getX() + r, pill.getY());
    pillShape.lineTo (humpCenterX - 68.0f, pill.getY());
    pillShape.cubicTo (humpCenterX - 42.0f, pill.getY(),
                       humpCenterX - 36.0f, humpTopY,
                       humpCenterX, humpTopY);
    pillShape.cubicTo (humpCenterX + 36.0f, humpTopY,
                       humpCenterX + 42.0f, pill.getY(),
                       humpCenterX + 68.0f, pill.getY());
    pillShape.lineTo (pill.getRight() - r, pill.getY());
    pillShape.quadraticTo (pill.getRight(), pill.getY(), pill.getRight(), pill.getY() + r);
    pillShape.lineTo (pill.getRight(), pill.getBottom() - r);
    pillShape.quadraticTo (pill.getRight(), pill.getBottom(), pill.getRight() - r, pill.getBottom());
    pillShape.lineTo (pill.getX() + r, pill.getBottom());
    pillShape.quadraticTo (pill.getX(), pill.getBottom(), pill.getX(), pill.getBottom() - r);
    pillShape.lineTo (pill.getX(), pill.getY() + r);
    pillShape.quadraticTo (pill.getX(), pill.getY(), pill.getX() + r, pill.getY());
    pillShape.closeSubPath();

    const juce::Point<float> glowCentre { humpCenterX, humpTopY + 4.0f };
    {
        const juce::Graphics::ScopedSaveState save (g);
        g.addTransform (juce::AffineTransform::scale (1.0f, 0.36f, glowCentre.x, glowCentre.y));
        juce::ColourGradient glow (getBandColour (attachedBandIndex).withAlpha (0.26f), glowCentre,
                                  juce::Colours::transparentBlack, glowCentre.translated (90.0f, 0.0f), true);
        g.setGradientFill (glow);
        g.fillEllipse (glowCentre.x - 90.0f, glowCentre.y - 90.0f, 180.0f, 180.0f);
    }

    g.setColour (juce::Colour (0x68000000));
    g.fillPath (pillShape, juce::AffineTransform::translation (0.0f, 3.0f));

    juce::ColourGradient graphite (juce::Colour (0xba393744), humpCenterX, humpTopY,
                                   kFloatingPillBg, humpCenterX, pill.getBottom(), false);
    g.setGradientFill (graphite);
    g.fillPath (pillShape);

    g.setColour (juce::Colour (0x99535760));
    g.strokePath (pillShape, juce::PathStrokeType (1.0f));

    // Se estiver no modo de Limiar Manual (!bp.dynAutoThresh), desenha a barra do medidor do sinal detector
    // atrás/abaixo do slider de Threshold manual (Manual Pro-Q 3 p. 14: "The level of the trigger signal is shown in the slider")
    if (manualThresholdSlider.isVisible())
    {
        auto meterArea = manualThreshMeterBounds.toFloat();
        g.setColour (juce::Colour (0xff14161c));
        g.fillRoundedRectangle (meterArea, 3.0f);

        const float detDb = audioProcessor.getBandDetectorLevelDb (attachedBandIndex);
        const float norm  = juce::jlimit (0.0f, 1.0f, (detDb + 60.0f) / 60.0f);
        if (norm > 0.01f)
        {
            const auto part = meterArea.withTrimmedTop ((1.0f - norm) * meterArea.getHeight());
            juce::ColourGradient level (juce::Colour (0xffeb735e), meterArea.getCentreX(), meterArea.getY(),
                                        juce::Colour (0xff5ccd9c), meterArea.getCentreX(), meterArea.getBottom(), false);
            level.addColour (0.42, kEqGold);
            g.setGradientFill (level);
            g.fillRoundedRectangle (part, 3.0f);
        }

        g.setColour (juce::Colour (0xff484d5c));
        g.drawRoundedRectangle (meterArea, 3.0f, 1.0f);
    }
}

void ProQ3AudioProcessorEditor::drawOutputMeter (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour (juce::Colour (0xff1e1e22));
    g.fillRect (bounds);
    g.setColour (juce::Colour (0xff302f37));
    g.fillRect (bounds.getX(), bounds.getY(), 1, bounds.getHeight());

    const float dbL = audioProcessor.getOutputPeakDbL();
    const float dbR = audioProcessor.getOutputPeakDbR();
    const float peak = juce::jmax (audioProcessor.getOutputHeldPeakDb (0), audioProcessor.getOutputHeldPeakDb (1));
    g.setFont (juce::FontOptions (9.5f));
    g.setColour (kTextMuted);
    g.drawText (peak <= -99.9f ? juce::String ("-INF") : juce::String (peak, 1),
                bounds.getX() + 2, bounds.getY() + 1, bounds.getWidth() - 4, 14,
                juce::Justification::centred, false);

    const auto yForDb = [this] (float db)
    {
        return juce::roundToInt (analyzerDbToY (db));
    };

    const auto floor = -getSpectrumDisplayRange();
    g.setColour (juce::Colour (0xff6e6e75));
    for (int db = 0; db >= floor; db -= 10)
        g.drawText (juce::String (db), bounds.getX() + 2, yForDb (static_cast<float> (db)) - 6, 25, 12,
                    juce::Justification::centredRight, false);

    for (int channel = 0; channel < 2; ++channel)
    {
        const int x = bounds.getRight() - 15 + 7 * channel;
        const float level = channel == 0 ? dbL : dbR;
        for (int db = floor + 2; db <= 0; db += 2)
        {
            const auto threshold = static_cast<float> (db);
            const bool lit = level > -99.9f && level >= threshold;
            const auto y = yForDb (threshold);
            const int barHeight = juce::jmax (1, yForDb (threshold - 2.0f) - y - 1);
            g.setColour (! lit ? juce::Colour (0xff292830)
                              : (db >= -2 ? juce::Colour (0xffee6874)
                                         : (db >= -24 ? kEqGold : juce::Colour (0xff84cf9c))));
            g.fillRect (x, y, 5, barHeight);
        }
        const auto held = audioProcessor.getOutputHeldPeakDb (static_cast<size_t> (channel));
        if (held > ProQ3AudioProcessor::kMeterFloorDb && held > static_cast<float> (floor))
        {
            g.setColour (held >= 0.0f ? juce::Colour (0xffee6874) : kEqGold);
            g.fillRect (x, yForDb (held), 5, 2);
        }
    }
}

void ProQ3AudioProcessorEditor::drawBottomToolbar (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const auto rail = bounds.toFloat().reduced (4.5f, 2.5f);
    juce::ColourGradient graphite (juce::Colour (0xff302b34), rail.getX(), rail.getY(),
                                   juce::Colour (0xff252129), rail.getX(), rail.getBottom(), false);
    g.setGradientFill (graphite);
    g.fillRoundedRectangle (rail, 8.0f);

    g.setColour (juce::Colour (0xff403945));
    g.drawRoundedRectangle (rail, 8.0f, 1.0f);
    for (const int x : { midiLearnButton.getRight() + 6, outputControlButton.getX() - 20 })
        g.drawVerticalLine (x, rail.getY() + 5.0f, rail.getBottom() - 5.0f);

    const float x = static_cast<float> (outputControlButton.getX()) - 14.0f;
    const float y = rail.getCentreY();
    juce::Path speaker;
    speaker.startNewSubPath (x, y - 2.0f);
    speaker.lineTo (x + 2.5f, y - 2.0f);
    speaker.lineTo (x + 5.5f, y - 4.0f);
    speaker.lineTo (x + 5.5f, y + 4.0f);
    speaker.lineTo (x + 2.5f, y + 2.0f);
    speaker.lineTo (x, y + 2.0f);
    speaker.closeSubPath();
    g.setColour (kTextMuted);
    g.fillPath (speaker);
}

//==============================================================================
// Layout Responsivo (Fiel ao Painel da Banda da image_435802.png)
//==============================================================================
void ProQ3AudioProcessorEditor::layoutBandControls()
{
    const int pillW = 472;
    const int pillH = 126;
    const auto band = audioProcessor.getBandParameters (attachedBandIndex);
    const auto anchor = band.used ? juce::roundToInt (frequencyToX (band.freqHz)) : graphBounds.getCentreX();
    const int pillX = juce::jlimit (graphBounds.getX() + 8, graphBounds.getRight() - pillW - 8, anchor - pillW / 2);
    const int pillY = graphBounds.getBottom() - pillH - 28;
    floatingPillBounds = juce::Rectangle<int> (pillX, pillY, pillW, pillH);

    // Coluna esquerda: Power/Bypass, Shape selector (Bell / Flat Tilt / etc.) e Slope (12 dB/oct)
    bandBypassButton.setBounds (pillX + 10, pillY + 8,  22, 20);
    bandShapeBox.setBounds     (pillX + 10, pillY + 38, 96, 28);
    bandSlopeBox.setBounds     (pillX + 10, pillY + 74, 96, 24);

    // Knob 1: FREQ (esquerda)
    freqSlider.setBounds         (pillX + 116, pillY + 20, 68, 68);
    freqRangeHintLabel.setBounds (pillX + 110, pillY + 88, 80, 14);
    freqTitleLabel.setBounds     (pillX + 116, pillY + 103, 68, 16);

    // Knob 2: GAIN + ANEL DINÂMICO (centro, maior que FREQ e Q, como na imagem image_435802.png)
    const int gainCenterX = pillX + 232;
    concentricGainKnob.setBounds (gainCenterX - 42, pillY + 10, 84, 84);
    gainRangeHintLabel.setBounds (gainCenterX - 42, pillY + 88, 84, 14);
    gainTitleLabel.setBounds     (gainCenterX - 34, pillY + 103, 68, 16);

    // Controles do Arco Dinâmico acima/ao redor do GAIN (exatamente nas posições da image_435802.png):
    // 'x' (clear dynamics) à esquerda às 10h, Power (bypass dynamics) às 11h, e "Auto" no topo às 12h
    clearDynamicsButton.setBounds  (gainCenterX - 48, pillY + 8,  18, 18);
    bypassDynamicsButton.setBounds (gainCenterX - 34, pillY - 6,  28, 18);
    autoThresholdButton.setBounds  (gainCenterX - 4,  pillY - 14, 44, 18);

    // Quando "Auto" é desativado (Modo Manual), exibe o slider de Threshold manual, medidor do detector e botão External Sidechain (SC)
    manualThresholdValue.setBounds (gainCenterX - 35, pillY - 33, 94, 18);
    manualThresholdSlider.setBounds (pillX + 273, pillY + 39, 25, 78);
    const auto sliderTrack = lookAndFeel.getSliderLayout (manualThresholdSlider).sliderBounds
        .translated (manualThresholdSlider.getX(), manualThresholdSlider.getY());
    manualThreshMeterBounds = sliderTrack.withSizeKeepingCentre (8, sliderTrack.getHeight());
    externalSidechainButton.setBounds (gainCenterX + 63, pillY - 32, 32, 18);

    // Engrenagem Gain-Q Interaction entre GAIN e Q
    gainQInterButton.setBounds (pillX + 276, pillY + 22, 24, 16);

    // Knob 3: Q (direita)
    qSlider.setBounds         (pillX + 298, pillY + 20, 68, 68);
    qRangeHintLabel.setBounds (pillX + 292, pillY + 88, 80, 14);
    qTitleLabel.setBounds     (pillX + 298, pillY + 103, 68, 16);

    // Coluna direita: < 1 >, 'X' (excluir banda no canto superior direito), Stereo e Split/Solo
    prevBandButton.setBounds   (pillX + 378, pillY + 8,  18, 20);
    bandNumberLabel.setBounds  (pillX + 396, pillY + 8,  22, 20);
    nextBandButton.setBounds   (pillX + 418, pillY + 8,  18, 20);
    deleteBandButton.setBounds (pillX + 444, pillY + 8,  20, 20);

    stereoModeBox.setBounds     (pillX + 376, pillY + 38, 86, 28);
    soloBandButton.setBounds    (pillX + 376, pillY + 74, 42, 24);
    splitStereoButton.setBounds (pillX + 420, pillY + 74, 42, 24);

}
 
void ProQ3AudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    topBarBounds    = area.removeFromTop (36);
    bottomBarBounds = area.removeFromBottom (36);
    meterBounds     = area.removeFromRight (44);
    pianoBounds     = area.withTrimmedTop (area.getHeight() - 22);
    graphBounds     = area;

    //---- Top Bar ----
    int tx = 206;
    const int ty = 6;
    const int th = 24;

    undoButton.setBounds (tx, ty, 48, th); tx += 52;
    redoButton.setBounds (tx, ty, 48, th); tx += 58;
    abButton.setBounds   (tx, ty, 42, th); tx += 46;
    copyButton.setBounds (tx, ty, 48, th);

    const int presetW = 240;
    const int presetX = juce::jmax (tx + 60, (topBarBounds.getWidth() - presetW) / 2);
    prevPresetButton.setBounds (presetX, ty, 24, th);
    presetNameButton.setBounds (presetX + 26, ty, presetW - 52, th);
    nextPresetButton.setBounds (presetX + presetW - 24, ty, 24, th);

    helpButton.setBounds (topBarBounds.getRight() - 91, ty, 48, th);
    expandButton.setBounds (topBarBounds.getRight() - 37, ty, 25, th);

    layoutBandControls();

    displayRangeButton.setBounds (graphBounds.getRight() - 68, graphBounds.getY() + 8, 58, 22);

    auto footer = bottomBarBounds.reduced (14, 6).withTrimmedRight (12);
    pianoToggleButton.setBounds (footer.removeFromLeft (32));
    footer.removeFromLeft (9);
    midiLearnButton.setBounds (footer.removeFromLeft (100));
    footer.removeFromLeft (12);
    globalBypassButton.setBounds (footer.removeFromRight (73));
    outputControlButton.setBounds (footer.removeFromRight (116));
    footer.removeFromRight (21);

    auto leftFooter = footer.removeFromLeft (footer.getWidth() / 2);
    processingModeBox.setBounds (leftFooter.withSizeKeepingCentre (142, leftFooter.getHeight()));
    analyzerButton.setBounds (footer.withSizeKeepingCentre (160, footer.getHeight()));
    analyzerBackdrop.setBounds (getLocalBounds());
    layoutAnalyzerPanel();
    const auto matchX = juce::jlimit (8, getWidth() - 376, analyzerButton.getBounds().getCentreX() - 184);
    matchPanel.setBounds (matchX, bottomBarBounds.getY() - 164, 368, 159);

    constexpr int panelWidth = 196;
    constexpr int panelHeight = 199;
    const int panelX = juce::jlimit (8, getWidth() - panelWidth - 8, outputControlButton.getBounds().getCentreX() - panelWidth / 2);
    outputPanel.setBounds (panelX, outputControlButton.getY() - panelHeight - 2, panelWidth, panelHeight);
    outputLevelLabel.setBounds (8, 6, panelWidth - 16, 19);
    outputGainSlider.setBounds (52, 24, 92, 80);
    outputScaleLabel.setBounds (10, 113, 63, 22);
    outputScaleSlider.setBounds (72, 113, 114, 22);
    outputBalanceLabel.setBounds (10, 140, 57, 22);
    outputBalanceSlider.setBounds (67, 140, 67, 22);
    outputBalanceModeBox.setBounds (136, 140, 50, 22);
    outputPolarityButton.setBounds (10, 169, 40, 21);
    outputAutoGainButton.setBounds (56, 169, 40, 21);
    outputOptionsButton.setBounds (144, 169, 42, 21);
}
```
