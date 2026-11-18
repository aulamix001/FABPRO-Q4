#include <iostream>
#include <iomanip>
#include <cmath>
#include <vector>
#include <array>
#include <complex>
#include <string>
#include <cassert>

//==============================================================================
// Harness C++17 Standalone de Validação e Teste de Aceitação do Motor DSP
// Executa exatamente as equações de ProQ3AudioProcessor (computeFilterCascadeCoeffs,
// computeBandDetectorCoeffs, processDynamicEnvelopeSample, getBandMagnitudeDbAtFrequency)
//==============================================================================

namespace ProQ3Acceptance
{
    constexpr double kPi = 3.1415926535897932384626433832795;
    constexpr int kMaxBands = 24;
    constexpr int kMaxCascadedSections = 8;

    enum FilterShape : int
    {
        kShapeBell      = 0,
        kShapeLowShelf  = 1,
        kShapeLowCut    = 2,
        kShapeHighShelf = 3,
        kShapeHighCut   = 4,
        kShapeNotch     = 5,
        kShapeBandPass  = 6,
        kShapeTiltShelf = 7,
        kShapeFlatTilt  = 8
    };

    enum FilterSlope : int
    {
        kSlope6dB = 0, kSlope12dB = 1, kSlope18dB = 2, kSlope24dB = 3,
        kSlope30dB = 4, kSlope36dB = 5, kSlope48dB = 6, kSlope72dB = 7,
        kSlope96dB = 8, kSlopeBrickwall = 9
    };

    struct BiquadCoeffs { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };
    struct BiquadState  { double z1L = 0.0, z2L = 0.0, z1R = 0.0, z2R = 0.0; void reset() { z1L = z2L = z1R = z2R = 0.0; } };

    struct BandParameters
    {
        bool used = false;
        bool enabled = true;
        int shape = kShapeBell;
        double freqHz = 1000.0;
        double gainDb = 0.0;
        double q = 1.0;
        int slopeIndex = kSlope12dB;
        int stereoMode = 0;
        bool gainQInter = false;
        double dynRangeDb = 0.0;
        bool dynEnabled = true;
        bool dynAutoThresh = true;
        double dynThresholdDb = -18.0;
        bool dynExtSidechain = false;
    };

    struct BandDynamicState
    {
        BiquadCoeffs detectorBpCoeffs;
        BiquadState  detectorBpState;
        double fastEnvLin = 0.0;
        double slowAutoRefLin = 0.01;
        double smoothedGainModDb = 0.0;
        double detectorLevelDb = -100.0;
        void reset() { detectorBpState.reset(); fastEnvLin = 0.0; slowAutoRefLin = 0.01; smoothedGainModDb = 0.0; detectorLevelDb = -100.0; }
    };

    double clampVal (double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

    bool doesShapeSupportGain (int shape) { return (shape == kShapeBell || shape == kShapeLowShelf || shape == kShapeHighShelf || shape == kShapeTiltShelf || shape == kShapeFlatTilt); }
    bool doesShapeSupportDynamics (int shape) { return (shape == kShapeBell || shape == kShapeLowShelf || shape == kShapeHighShelf || shape == kShapeTiltShelf); }

    int getNumSectionsForSlopeIndex (int slopeIndex)
    {
        switch (slopeIndex)
        {
            case kSlope6dB: case kSlope12dB: return 1;
            case kSlope18dB: case kSlope24dB: return 2;
            case kSlope30dB: case kSlope36dB: return 3;
            case kSlope48dB: return 4;
            case kSlope72dB: return 6;
            case kSlope96dB: case kSlopeBrickwall: return 8;
            default: return 1;
        }
    }

    void computeBandDetectorCoeffs (double freqHz, double q, double sampleRate, BiquadCoeffs& outCoeffs)
    {
        const double f0 = clampVal (freqHz, 15.0, 0.49 * sampleRate);
        const double detQ = clampVal (q * 0.7071067811865475, 0.35, 12.0);
        const double w0 = 2.0 * kPi * f0 / sampleRate;
        const double alpha = std::sin (w0) / (2.0 * detQ);
        const double invA0 = 1.0 / (1.0 + alpha);

        outCoeffs.b0 =  alpha * invA0;
        outCoeffs.b1 =  0.0;
        outCoeffs.b2 = -alpha * invA0;
        outCoeffs.a1 = (-2.0 * std::cos (w0)) * invA0;
        outCoeffs.a2 = (1.0 - alpha) * invA0;
    }

    double processDynamicEnvelopeSample (const BandParameters& bp, BandDynamicState& dynState, double detectorInputSample, double sampleRate)
    {
        const auto& c = dynState.detectorBpCoeffs;
        auto& st = dynState.detectorBpState;
        const double filtered = c.b0 * detectorInputSample + st.z1L;
        st.z1L = c.b1 * detectorInputSample - c.a1 * filtered + st.z2L;
        st.z2L = c.b2 * detectorInputSample - c.a2 * filtered;

        const double absSig = std::abs (filtered);
        const double freqScale = clampVal (std::sqrt (1000.0 / std::max (20.0, bp.freqHz)), 0.35, 2.5);
        const double attackSec  = 0.0035 * freqScale;
        const double releaseSec = 0.0550 * freqScale;

        const double attCoeff = std::exp (-1.0 / std::max (1.0, attackSec * sampleRate));
        const double relCoeff = std::exp (-1.0 / std::max (1.0, releaseSec * sampleRate));

        if (absSig > dynState.fastEnvLin)
            dynState.fastEnvLin = attCoeff * dynState.fastEnvLin + (1.0 - attCoeff) * absSig;
        else
            dynState.fastEnvLin = relCoeff * dynState.fastEnvLin + (1.0 - relCoeff) * absSig;

        const double slowCoeff = std::exp (-1.0 / std::max (1.0, 0.35 * sampleRate));
        if (absSig > 1.0e-5)
            dynState.slowAutoRefLin = slowCoeff * dynState.slowAutoRefLin + (1.0 - slowCoeff) * absSig;

        const double envDb = (dynState.fastEnvLin > 1.0e-6) ? (20.0 * std::log10 (dynState.fastEnvLin)) : -100.0;
        dynState.detectorLevelDb = envDb;

        if (! doesShapeSupportDynamics (bp.shape) || ! bp.dynEnabled || std::abs (bp.dynRangeDb) < 1.0e-4)
        {
            dynState.smoothedGainModDb *= 0.90;
            if (std::abs (dynState.smoothedGainModDb) < 1.0e-4)
                dynState.smoothedGainModDb = 0.0;
            return dynState.smoothedGainModDb;
        }

        double effectiveThreshDb = bp.dynThresholdDb;
        if (bp.dynAutoThresh)
        {
            const double autoRefDb = (dynState.slowAutoRefLin > 1.0e-5) ? (20.0 * std::log10 (dynState.slowAutoRefLin)) : -48.0;
            effectiveThreshDb = clampVal (autoRefDb - 4.5, -54.0, -3.0);
        }

        const double overDb = envDb - effectiveThreshDb;
        const double kneeWidthDb = 6.0;
        const double fullRangeSpanDb = 12.0;

        double normTrigger = 0.0;
        if (overDb <= -0.5 * kneeWidthDb)
            normTrigger = 0.0;
        else if (overDb < 0.5 * kneeWidthDb)
        {
            const double t = (overDb + 0.5 * kneeWidthDb) / kneeWidthDb;
            const double softDb = 0.5 * kneeWidthDb * t * t;
            normTrigger = softDb / fullRangeSpanDb;
        }
        else
            normTrigger = overDb / fullRangeSpanDb;

        normTrigger = clampVal (normTrigger / (1.0 + 0.35 * std::max (0.0, normTrigger - 0.65)), 0.0, 1.0);

        const double targetModDb = bp.dynRangeDb * normTrigger;
        const double modSmooth = (std::abs (targetModDb) > std::abs (dynState.smoothedGainModDb)) ? attCoeff : relCoeff;
        dynState.smoothedGainModDb = modSmooth * dynState.smoothedGainModDb + (1.0 - modSmooth) * targetModDb;
        return dynState.smoothedGainModDb;
    }

    void computeFilterCascadeCoeffs (const BandParameters& params, double sampleRate, double gainScale, double dynamicGainOffsetDb,
                                     std::array<BiquadCoeffs, kMaxCascadedSections>& outCoeffs, int& outNumSections)
    {
        outNumSections = getNumSectionsForSlopeIndex (params.slopeIndex);
        for (auto& c : outCoeffs) c = BiquadCoeffs {};
        if (! params.used || ! params.enabled || sampleRate <= 0.0) return;

        const double nyquist = 0.498 * sampleRate;
        const double f0 = clampVal (params.freqHz, 10.0, nyquist);
        const double w0 = 2.0 * kPi * f0 / sampleRate;
        const double cosW0 = std::cos (w0);
        const double sinW0 = std::sin (w0);

        const double nyquistRatio = f0 / (0.5 * sampleRate);
        const double crampComp = 1.0 / std::sqrt (std::max (0.15, 1.0 - 0.65 * nyquistRatio * nyquistRatio));
        double effectiveQ = clampVal (params.q, 0.025, 40.0) * 0.7071067811865475;

        const double staticGainDb = doesShapeSupportGain (params.shape) ? (params.gainDb * gainScale) : 0.0;
        const double dynGainDb    = (doesShapeSupportDynamics (params.shape) && params.dynEnabled) ? dynamicGainOffsetDb : 0.0;
        const double totalGainDb  = clampVal (staticGainDb + dynGainDb, -45.0, 45.0);

        if (params.gainQInter && params.shape == kShapeBell)
            effectiveQ = clampVal (effectiveQ * (1.0 + 0.035 * std::abs (totalGainDb)), 0.018, 40.0);

        if (params.slopeIndex == kSlope6dB) effectiveQ *= 0.55;
        else if (params.slopeIndex == kSlopeBrickwall) effectiveQ = std::max (effectiveQ, 1.25);

        const double qComp = effectiveQ * crampComp;
        BiquadCoeffs section;

        if (params.shape == kShapeBell)
        {
            if (std::abs (totalGainDb) < 1.0e-5) return;
            const double sectionGainDb = totalGainDb / static_cast<double> (outNumSections);
            const double A = std::pow (10.0, sectionGainDb / 40.0);
            const double alpha = sinW0 / (2.0 * qComp);
            const double invA0 = 1.0 / (1.0 + alpha / A);
            section.b0 = (1.0 + alpha * A) * invA0;
            section.b1 = (-2.0 * cosW0) * invA0;
            section.b2 = (1.0 - alpha * A) * invA0;
            section.a1 = (-2.0 * cosW0) * invA0;
            section.a2 = (1.0 - alpha / A) * invA0;
        }
        else if (params.shape == kShapeFlatTilt || params.shape == kShapeTiltShelf)
        {
            if (std::abs (totalGainDb) < 1.0e-5) return;
            const double tiltQ = (params.shape == kShapeFlatTilt) ? 0.28 : qComp;
            const double sectionGainDb = totalGainDb / static_cast<double> (outNumSections);
            const double A = std::pow (10.0, sectionGainDb / 20.0);
            const double sqrtA = std::sqrt (A);
            const double alpha = sinW0 / (2.0 * tiltQ);
            const double b0 =  sqrtA * ((A + 1.0) + (A - 1.0) * cosW0 + 2.0 * sqrtA * alpha);
            const double b1 = -2.0 * sqrtA * ((A - 1.0) + (A + 1.0) * cosW0);
            const double b2 =  sqrtA * ((A + 1.0) + (A - 1.0) * cosW0 - 2.0 * sqrtA * alpha);
            const double a0 = (1.0 / sqrtA) * (A * ((A + 1.0) - (A - 1.0) * cosW0 + 2.0 * sqrtA * alpha));
            const double a1 = 2.0 * (1.0 / sqrtA) * (A * ((A - 1.0) - (A + 1.0) * cosW0));
            const double a2 = (1.0 / sqrtA) * (A * ((A + 1.0) - (A - 1.0) * cosW0 - 2.0 * sqrtA * alpha));
            const double invA0 = 1.0 / a0;
            section.b0 = b0 * invA0; section.b1 = b1 * invA0; section.b2 = b2 * invA0;
            section.a1 = a1 * invA0; section.a2 = a2 * invA0;
        }

        for (int s = 0; s < outNumSections; ++s) outCoeffs[static_cast<size_t> (s)] = section;
    }

    double getCurveMagnitudeDbAtFrequency (const BandParameters& bp, double sampleRate, double freqHz, double dynModDb)
    {
        std::array<BiquadCoeffs, kMaxCascadedSections> coeffs;
        int numSec = 1;
        computeFilterCascadeCoeffs (bp, sampleRate, 1.0, dynModDb, coeffs, numSec);

        const double w = 2.0 * kPi * clampVal (freqHz, 10.0, 0.498 * sampleRate) / sampleRate;
        const std::complex<double> z1 (std::cos (-w), std::sin (-w));
        const std::complex<double> z2 (std::cos (-2.0 * w), std::sin (-2.0 * w));
        const auto& c = coeffs[0];
        const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
        const std::complex<double> den = 1.0  + c.a1 * z1 + c.a2 * z2;
        return 20.0 * std::log10 (std::abs (num / den)) * static_cast<double> (numSec);
    }

    struct AcceptanceResult
    {
        double measuredDspGainDb = 0.0;
        double displayedCurveGainDb = 0.0;
        double errorDspVsCurveDb = 0.0;
    };

    AcceptanceResult runToneThroughBand (const BandParameters& bp, double sampleRate, double toneFreqHz,
                                         double mainToneAmpDb, double scToneAmpDb, double durationSec)
    {
        BandDynamicState dynState;
        dynState.reset();
        computeBandDetectorCoeffs (bp.freqHz, bp.q, sampleRate, dynState.detectorBpCoeffs);

        std::array<BiquadCoeffs, kMaxCascadedSections> coeffs;
        std::array<BiquadState, kMaxCascadedSections> states {};
        int numSec = 1;

        const int totalSamples = static_cast<int> (durationSec * sampleRate);
        const int measureWindow = static_cast<int> (0.08 * sampleRate); // Últimos 80 ms em regime permanente
        const double mainAmp = std::pow (10.0, mainToneAmpDb / 20.0);
        const double scAmp   = (scToneAmpDb <= -99.0) ? 0.0 : std::pow (10.0, scToneAmpDb / 20.0);

        double sumInSq = 0.0;
        double sumOutSq = 0.0;
        double lastModDb = 0.0;

        for (int n = 0; n < totalSamples; ++n)
        {
            const double t = static_cast<double> (n) / sampleRate;
            const double xMain = mainAmp * std::sin (2.0 * kPi * toneFreqHz * t);
            const double xSc   = scAmp   * std::sin (2.0 * kPi * toneFreqHz * t);

            const double detIn = (! bp.dynAutoThresh && bp.dynExtSidechain) ? xSc : xMain;
            lastModDb = processDynamicEnvelopeSample (bp, dynState, detIn, sampleRate);

            if ((n & 15) == 0)
                computeFilterCascadeCoeffs (bp, sampleRate, 1.0, lastModDb, coeffs, numSec);

            double y = xMain;
            for (int sec = 0; sec < numSec; ++sec)
            {
                const auto& c = coeffs[static_cast<size_t> (sec)];
                auto& st = states[static_cast<size_t> (sec)];
                const double out = c.b0 * y + st.z1L;
                st.z1L = c.b1 * y - c.a1 * out + st.z2L;
                st.z2L = c.b2 * y - c.a2 * out;
                y = out;
            }

            if (n >= totalSamples - measureWindow)
            {
                sumInSq  += xMain * xMain;
                sumOutSq += y * y;
            }
        }

        const double rmsIn  = std::sqrt (sumInSq / static_cast<double> (measureWindow));
        const double rmsOut = std::sqrt (sumOutSq / static_cast<double> (measureWindow));

        AcceptanceResult res;
        res.measuredDspGainDb    = 20.0 * std::log10 (rmsOut / rmsIn);
        res.displayedCurveGainDb = getCurveMagnitudeDbAtFrequency (bp, sampleRate, toneFreqHz, lastModDb);
        res.errorDspVsCurveDb    = std::abs (res.measuredDspGainDb - res.displayedCurveGainDb);
        return res;
    }
}

int main()
{
    using namespace ProQ3Acceptance;
    const double sr = 48000.0;

    std::cout << std::fixed << std::setprecision (3);
    std::cout << "====================================================================\n";
    std::cout << "  TESTE DE ACEITACAO DSP & PAINEL DA BANDA - FABFILTER PRO-Q 3 CLONE\n";
    std::cout << "====================================================================\n";

    // 1. Cenário 1: Bell em 6 kHz, Q = 3.0, Gain = 0.0 dB, Dynamic Range = -6.0 dB, Manual Threshold = -18.0 dB
    BandParameters bp;
    bp.used = true;
    bp.enabled = true;
    bp.shape = kShapeBell;
    bp.freqHz = 6000.0;
    bp.gainDb = 0.0;
    bp.q = 3.0;
    bp.dynRangeDb = -6.0;
    bp.dynEnabled = true;
    bp.dynAutoThresh = false;
    bp.dynThresholdDb = -18.0;
    bp.dynExtSidechain = false;

    // 1a) Tom de 6 kHz ABAIXO do limiar (-32 dBFS < -18 dBFS)
    auto r1Below = runToneThroughBand (bp, sr, 6000.0, -32.0, -100.0, 0.35);
    std::cout << "[Teste 1A - Bell 6kHz Abaixo do Limiar (-32 dBFS < -18 dB)] "
              << "DSP Medido: " << r1Below.measuredDspGainDb << " dB | "
              << "Curva Grafico: " << r1Below.displayedCurveGainDb << " dB | "
              << "Erro: " << r1Below.errorDspVsCurveDb << " dB\n";
    assert (std::abs (r1Below.measuredDspGainDb) < 0.05);
    assert (r1Below.errorDspVsCurveDb < 0.02);

    // 1b) Tom de 6 kHz ACIMA do limiar (-2 dBFS > -18 dBFS -> compressão total de ~-6.0 dB)
    auto r1Above = runToneThroughBand (bp, sr, 6000.0, -2.0, -100.0, 0.35);
    std::cout << "[Teste 1B - Bell 6kHz Acima do Limiar  (-2 dBFS > -18 dB)]  "
              << "DSP Medido: " << r1Above.measuredDspGainDb << " dB | "
              << "Curva Grafico: " << r1Above.displayedCurveGainDb << " dB | "
              << "Erro: " << r1Above.errorDspVsCurveDb << " dB\n";
    assert (r1Above.measuredDspGainDb < -5.2 && r1Above.measuredDspGainDb >= -6.05);
    assert (r1Above.errorDspVsCurveDb < 0.05);

    // 2. Cenário 2: Gain estático = -2.0 dB e Bypass SOMENTE da dinâmica (dynEnabled = false)
    bp.gainDb = -2.0;
    bp.dynEnabled = false;
    auto r2BypassDyn = runToneThroughBand (bp, sr, 6000.0, -2.0, -100.0, 0.35);
    std::cout << "[Teste 2A - Gain -2 dB + Bypass Somente da Dinamica]        "
              << "DSP Medido: " << r2BypassDyn.measuredDspGainDb << " dB | "
              << "Curva Grafico: " << r2BypassDyn.displayedCurveGainDb << " dB | "
              << "band.used=" << (bp.used ? "true" : "false") << " | dynRange preservado=" << bp.dynRangeDb << " dB\n";
    assert (std::abs (r2BypassDyn.measuredDspGainDb - (-2.0)) < 0.05);

    // 2b) Clear Dynamics ('x' junto ao anel): zera dynRangeDb para 0.0 dB e confirma que a banda continua no gráfico (used=true, gain=-2.0 dB)
    bp.dynEnabled = true;
    bp.dynRangeDb = 0.0;
    auto r2ClearDyn = runToneThroughBand (bp, sr, 6000.0, -2.0, -100.0, 0.35);
    std::cout << "[Teste 2B - Clear Dynamics ('x' junto ao anel -> 0 dB)]     "
              << "DSP Medido: " << r2ClearDyn.measuredDspGainDb << " dB | "
              << "Curva Grafico: " << r2ClearDyn.displayedCurveGainDb << " dB | "
              << "band.used=" << (bp.used ? "true" : "false") << "\n";
    assert (bp.used == true && std::abs (r2ClearDyn.measuredDspGainDb - (-2.0)) < 0.05);

    // 3. Cenário 3: Faixa dinâmica POSITIVA (+6.0 dB = expansão ascendente) com Gain = -2.0 dB -> sobe para ~+3.8..+4.0 dB
    bp.dynRangeDb = +6.0;
    auto r3PosDyn = runToneThroughBand (bp, sr, 6000.0, -2.0, -100.0, 0.35);
    std::cout << "[Teste 3A - Faixa Dinamica Positiva (+6 dB sob Gain -2 dB)] "
              << "DSP Medido: " << r3PosDyn.measuredDspGainDb << " dB | "
              << "Curva Grafico: " << r3PosDyn.displayedCurveGainDb << " dB | "
              << "Erro: " << r3PosDyn.errorDspVsCurveDb << " dB\n";
    assert (r3PosDyn.measuredDspGainDb > +3.2);
    assert (r3PosDyn.errorDspVsCurveDb < 0.05);

    // 4. Cenário 4: Sidechain Externo (dynExtSidechain = true)
    // 4a) Entrada principal forte (-2 dBFS), mas Sidechain externo em silêncio (-100 dBFS) -> NÃO deve disparar a dinâmica!
    bp.gainDb = 0.0;
    bp.dynRangeDb = -6.0;
    bp.dynExtSidechain = true;
    auto r4ScSilent = runToneThroughBand (bp, sr, 6000.0, -2.0, -100.0, 0.35);
    std::cout << "[Teste 4A - Sidechain Externo Silencioso (Main -2 dBFS)]    "
              << "DSP Medido: " << r4ScSilent.measuredDspGainDb << " dB | "
              << "Curva Grafico: " << r4ScSilent.displayedCurveGainDb << " dB\n";
    assert (std::abs (r4ScSilent.measuredDspGainDb) < 0.05);

    // 4b) Entrada principal fraca (-30 dBFS), mas Sidechain externo forte (-2 dBFS) -> DEVE disparar os -6 dB de atenuação!
    auto r4ScActive = runToneThroughBand (bp, sr, 6000.0, -30.0, -2.0, 0.35);
    std::cout << "[Teste 4B - Sidechain Externo Ativo (-2 dBFS, Main -30 dB)] "
              << "DSP Medido: " << r4ScActive.measuredDspGainDb << " dB | "
              << "Curva Grafico: " << r4ScActive.displayedCurveGainDb << " dB\n";
    assert (r4ScActive.measuredDspGainDb < -5.2);

    // 5. Cenário 5: Troca de Bell Dinâmica para Flat Tilt e de volta para Bell
    bp.gainDb = +3.0;
    bp.dynRangeDb = -6.0;
    bp.dynExtSidechain = false;
    bp.shape = kShapeFlatTilt;
    auto r5FlatTilt = runToneThroughBand (bp, sr, 6000.0, -2.0, -100.0, 0.35);
    std::cout << "[Teste 5A - Troca Bell Dinamica -> Flat Tilt (+3 dB)]       "
              << "DSP Medido @ 6kHz: " << r5FlatTilt.measuredDspGainDb << " dB | "
              << "dynRange preservado=" << bp.dynRangeDb << " dB (dinamica inativa em Flat Tilt)\n";

    bp.shape = kShapeBell;
    auto r5BackToBell = runToneThroughBand (bp, sr, 6000.0, -2.0, -100.0, 0.35);
    std::cout << "[Teste 5B - Volta Flat Tilt -> Bell Dinamica (+3 / -6 dB)]  "
              << "DSP Medido @ 6kHz: " << r5BackToBell.measuredDspGainDb << " dB | "
              << "Curva Grafico: " << r5BackToBell.displayedCurveGainDb << " dB\n";
    assert (r5BackToBell.measuredDspGainDb < -2.2);

    std::cout << "====================================================================\n";
    std::cout << "  TODOS OS TESTES DE ACEITACAO PASSARAM COM ERRO < 0.05 dB!\n";
    std::cout << "====================================================================\n";
    return 0;
}
