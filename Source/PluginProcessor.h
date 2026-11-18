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
    static constexpr int kFftOrder = 12;           // 4096 pontos
    static constexpr int kFftSize  = 1 << kFftOrder;

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
        BiquadCoeffs coefficients;
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

    // Ativa uma nova banda livre na frequência e ganho clicados
    int activateFirstFreeBand (float freqHz, float gainDb, float qValue = 1.0f, int shape = kShapeBell, float dynRangeDb = 0.0f);
    void deactivateBand (int bandIndex);

    // Suporte A/B Switch e Copy
    void toggleABState();
    void copyActiveToInactiveState();
    bool isStateBActive() const noexcept { return currentIsB; }

    // Analisador de espectro Pre/Post/Sidechain e Spectrum Grab
    bool pullNextPreFftBlock (std::vector<float>& destMagnitudesDb);
    bool pullNextPostFftBlock (std::vector<float>& destMagnitudesDb);
    bool pullNextSidechainFftBlock (std::vector<float>& destMagnitudesDb);

    float getOutputPeakDbL() const noexcept { return outputPeakDbL.load (std::memory_order_relaxed); }
    float getOutputPeakDbR() const noexcept { return outputPeakDbR.load (std::memory_order_relaxed); }
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

    struct SingleChannelFftFifo
    {
        std::array<float, kFftSize> fifo {};
        std::array<float, kFftSize * 2> fftWorkspace {};
        int fifoIndex = 0;
        std::atomic<bool> nextBlockReady { false };

        void pushSample (float s) noexcept
        {
            fifo[static_cast<size_t> (fifoIndex++)] = s;
            if (fifoIndex >= kFftSize)
            {
                if (! nextBlockReady.load (std::memory_order_relaxed))
                {
                    std::copy (fifo.begin(), fifo.end(), fftWorkspace.begin());
                    nextBlockReady.store (true, std::memory_order_release);
                }
                fifoIndex = 0;
            }
        }
    };

    SingleChannelFftFifo preFftFifo;
    SingleChannelFftFifo postFftFifo;
    SingleChannelFftFifo sidechainFftFifo;
    juce::dsp::FFT forwardFft { kFftOrder };
    juce::dsp::WindowingFunction<float> hannWindow { static_cast<size_t> (kFftSize), juce::dsp::WindowingFunction<float>::hann };

    std::array<BandDspChain, kMaxBands> dspBands;
    std::array<std::atomic<float>, kMaxBands> currentDynamicGainDb {};
    std::array<std::atomic<float>, kMaxBands> currentDetectorLevelDb {};

    BiquadCoeffs soloBandPassCoeffs;
    BiquadState  soloBandPassState;

    std::atomic<double> currentSampleRate { 44100.0 };
    std::atomic<float> outputPeakDbL { -100.0f };
    std::atomic<float> outputPeakDbR { -100.0f };
    std::atomic<int> selectedBandIndex { 0 };
    std::atomic<int> soloBandIndex { -1 };
    std::atomic<float> soloFrequencyHz { 1000.0f };
    std::atomic<float> soloBandwidthQ { 2.0f };

    juce::ValueTree stateA;
    juce::ValueTree stateB;
    bool currentIsB = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProQ3AudioProcessor)
};
