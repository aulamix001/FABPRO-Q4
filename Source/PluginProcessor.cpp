#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

namespace
{
    constexpr double kPi = 3.1415926535897932384626433832795;

    // Número de seções de 2ª ordem em cascata de acordo com o Slope do Pro-Q 3
    // 0: 6 dB/oct, 1: 12 dB/oct, 2: 18, 3: 24, 4: 30, 5: 36, 6: 48, 7: 72, 8: 96, 9: Brickwall
    int getNumSectionsForSlopeIndex (int slopeIndex) noexcept
    {
        switch (slopeIndex)
        {
            case ProQ3AudioProcessor::kSlope6dB:       return 1;
            case ProQ3AudioProcessor::kSlope12dB:      return 1;
            case ProQ3AudioProcessor::kSlope18dB:      return 2;
            case ProQ3AudioProcessor::kSlope24dB:      return 2;
            case ProQ3AudioProcessor::kSlope30dB:      return 3;
            case ProQ3AudioProcessor::kSlope36dB:      return 3;
            case ProQ3AudioProcessor::kSlope48dB:      return 4;
            case ProQ3AudioProcessor::kSlope72dB:      return 6;
            case ProQ3AudioProcessor::kSlope96dB:      return 8;
            case ProQ3AudioProcessor::kSlopeBrickwall: return 8;
            default:                                   return 1;
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

    stateA = apvts.copyState();
    stateB = apvts.copyState();
    updateAllFilterCoefficients();
}

ProQ3AudioProcessor::~ProQ3AudioProcessor() = default;

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
    if ((shape == kShapeLowCut || shape == kShapeHighCut) && slopeIndex == kSlopeBrickwall)
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
        const bool defaultUsed = (i == 0);

        // IDs originais preservados 100% intactos
        params.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { getBandParamID (i, "used"), 1 },
            prefix + "Used",
            defaultUsed));

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

    return { params.begin(), params.end() };
}

const juce::String ProQ3AudioProcessor::getName() const { return JucePlugin_Name; }
bool ProQ3AudioProcessor::acceptsMidi() const { return false; }
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
    outNumSections = getNumSectionsForSlopeIndex (params.slopeIndex);
    for (auto& c : outCoeffs)
        c = BiquadCoeffs {};

    if (! params.used || ! params.enabled || sampleRate <= 0.0)
        return;

    const double nyquist = 0.498 * sampleRate;
    const double f0 = juce::jlimit (10.0, nyquist, params.freqHz);
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
        effectiveQ *= 0.55;
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

        case kShapeLowCut: // High-Pass
        {
            const double alpha = sinW0 / (2.0 * qComp);
            const double b0 =  (1.0 + cosW0) * 0.5;
            const double b1 = -(1.0 + cosW0);
            const double b2 =  (1.0 + cosW0) * 0.5;
            const double a0 =   1.0 + alpha;
            const double a1 =  -2.0 * cosW0;
            const double a2 =   1.0 - alpha;
            const double invA0 = 1.0 / a0;

            section.b0 = b0 * invA0;
            section.b1 = b1 * invA0;
            section.b2 = b2 * invA0;
            section.a1 = a1 * invA0;
            section.a2 = a2 * invA0;
            break;
        }

        case kShapeHighCut: // Low-Pass
        {
            const double alpha = sinW0 / (2.0 * qComp);
            const double b0 =  (1.0 - cosW0) * 0.5;
            const double b1 =   1.0 - cosW0;
            const double b2 =  (1.0 - cosW0) * 0.5;
            const double a0 =   1.0 + alpha;
            const double a1 =  -2.0 * cosW0;
            const double a2 =   1.0 - alpha;
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
    std::array<BiquadCoeffs, kMaxCascadedSections> coeffs;
    computeFilterCascadeCoeffs (parameters, response.sampleRate, gainScale, dynamicGainDb, coeffs, response.sections);
    response.coefficients = coeffs.front();
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

    const auto& c = coefficients;
    const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const std::complex<double> den = 1.0  + c.a1 * z1 + c.a2 * z2;

    const double sectionMag = std::abs (num / den);
    if (sectionMag <= 1.0e-12)
        return -120.0;

    return 20.0 * std::log10 (sectionMag) * static_cast<double> (sections);
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

int ProQ3AudioProcessor::activateFirstFreeBand (float freqHz, float gainDb, float qValue, int shape, float dynRangeDb)
{
    for (int i = 0; i < kMaxBands; ++i)
    {
        const auto bp = getBandParameters (i);
        if (! bp.used)
        {
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

void ProQ3AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    auto mainBus = getBusBuffer (buffer, true, 0);
    const int numMainChannels = mainBus.getNumChannels();
    const int numSamples      = buffer.getNumSamples();

    if (numSamples <= 0 || numMainChannels <= 0)
        return;

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

    // Captura de sinal Pre-EQ e Sidechain para o Spectrum Analyzer
    for (int s = 0; s < numSamples; ++s)
    {
        const float monoPre = (rightChannel != nullptr) ? 0.5f * (leftChannel[s] + rightChannel[s]) : leftChannel[s];
        preFftFifo.pushSample (monoPre);

        if (scLeft != nullptr)
        {
            const float monoSc = (scRight != nullptr) ? 0.5f * (scLeft[s] + scRight[s]) : scLeft[s];
            sidechainFftFifo.pushSample (monoSc);
        }
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

        if (std::abs (linearGain - 1.0f) > 1.0e-5f)
            mainBus.applyGain (linearGain);
    }

    float peakL = 0.0f;
    float peakR = 0.0f;

    for (int s = 0; s < numSamples; ++s)
    {
        const float l = leftChannel[s];
        const float r = (rightChannel != nullptr) ? rightChannel[s] : l;

        peakL = juce::jmax (peakL, std::abs (l));
        peakR = juce::jmax (peakR, std::abs (r));

        postFftFifo.pushSample (0.5f * (l + r));
    }

    const float newDbL = juce::Decibels::gainToDecibels (peakL, -100.0f);
    const float newDbR = juce::Decibels::gainToDecibels (peakR, -100.0f);

    const float prevL = outputPeakDbL.load (std::memory_order_relaxed);
    const float prevR = outputPeakDbR.load (std::memory_order_relaxed);

    outputPeakDbL.store (newDbL > prevL ? newDbL : prevL * 0.92f + newDbL * 0.08f, std::memory_order_relaxed);
    outputPeakDbR.store (newDbR > prevR ? newDbR : prevR * 0.92f + newDbR * 0.08f, std::memory_order_relaxed);
}

bool ProQ3AudioProcessor::pullNextPreFftBlock (std::vector<float>& destMagnitudesDb)
{
    if (! preFftFifo.nextBlockReady.load (std::memory_order_acquire))
        return false;

    hannWindow.multiplyWithWindowingTable (preFftFifo.fftWorkspace.data(), static_cast<size_t> (kFftSize));
    forwardFft.performFrequencyOnlyForwardTransform (preFftFifo.fftWorkspace.data());

    const int numBins = kFftSize / 2;
    destMagnitudesDb.resize (static_cast<size_t> (numBins));

    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    for (int i = 1; i < numBins; ++i)
    {
        const double binFreq = (static_cast<double> (i) * sr) / static_cast<double> (kFftSize);
        const double tiltDb  = 4.5 * std::log2 (juce::jmax (20.0, binFreq) / 1000.0);
        const float normMag  = preFftFifo.fftWorkspace[static_cast<size_t> (i)] / static_cast<float> (numBins);
        destMagnitudesDb[static_cast<size_t> (i)] = juce::Decibels::gainToDecibels (normMag, -120.0f) + static_cast<float> (tiltDb);
    }
    destMagnitudesDb[0] = -120.0f;

    preFftFifo.nextBlockReady.store (false, std::memory_order_release);
    return true;
}

bool ProQ3AudioProcessor::pullNextPostFftBlock (std::vector<float>& destMagnitudesDb)
{
    if (! postFftFifo.nextBlockReady.load (std::memory_order_acquire))
        return false;

    hannWindow.multiplyWithWindowingTable (postFftFifo.fftWorkspace.data(), static_cast<size_t> (kFftSize));
    forwardFft.performFrequencyOnlyForwardTransform (postFftFifo.fftWorkspace.data());

    const int numBins = kFftSize / 2;
    destMagnitudesDb.resize (static_cast<size_t> (numBins));

    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    for (int i = 1; i < numBins; ++i)
    {
        const double binFreq = (static_cast<double> (i) * sr) / static_cast<double> (kFftSize);
        const double tiltDb  = 4.5 * std::log2 (juce::jmax (20.0, binFreq) / 1000.0);
        const float normMag  = postFftFifo.fftWorkspace[static_cast<size_t> (i)] / static_cast<float> (numBins);
        destMagnitudesDb[static_cast<size_t> (i)] = juce::Decibels::gainToDecibels (normMag, -120.0f) + static_cast<float> (tiltDb);
    }
    destMagnitudesDb[0] = -120.0f;

    postFftFifo.nextBlockReady.store (false, std::memory_order_release);
    return true;
}

bool ProQ3AudioProcessor::pullNextSidechainFftBlock (std::vector<float>& destMagnitudesDb)
{
    if (! sidechainFftFifo.nextBlockReady.load (std::memory_order_acquire))
        return false;

    hannWindow.multiplyWithWindowingTable (sidechainFftFifo.fftWorkspace.data(), static_cast<size_t> (kFftSize));
    forwardFft.performFrequencyOnlyForwardTransform (sidechainFftFifo.fftWorkspace.data());

    const int numBins = kFftSize / 2;
    destMagnitudesDb.resize (static_cast<size_t> (numBins));

    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    for (int i = 1; i < numBins; ++i)
    {
        const double binFreq = (static_cast<double> (i) * sr) / static_cast<double> (kFftSize);
        const double tiltDb  = 4.5 * std::log2 (juce::jmax (20.0, binFreq) / 1000.0);
        const float normMag  = sidechainFftFifo.fftWorkspace[static_cast<size_t> (i)] / static_cast<float> (numBins);
        destMagnitudesDb[static_cast<size_t> (i)] = juce::Decibels::gainToDecibels (normMag, -120.0f) + static_cast<float> (tiltDb);
    }
    destMagnitudesDb[0] = -120.0f;

    sidechainFftFifo.nextBlockReady.store (false, std::memory_order_release);
    return true;
}

bool ProQ3AudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* ProQ3AudioProcessor::createEditor()
{
    return new ProQ3AudioProcessorEditor (*this);
}

void ProQ3AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("selectedBandIndex", selectedBandIndex.load (std::memory_order_relaxed), nullptr);
    state.setProperty ("currentIsB", currentIsB, nullptr);

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
            selectedBandIndex.store (static_cast<int> (newTree.getProperty ("selectedBandIndex", 0)), std::memory_order_relaxed);
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
