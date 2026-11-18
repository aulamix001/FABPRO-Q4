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
    void timerCallback() override;

    float frequencyToX (double freqHz) const noexcept;
    double xToFrequency (float x) const noexcept;
    float gainDbToY (double gainDb) const noexcept;
    double yToGainDb (float y) const noexcept;
    float analyzerDbToY (double analyzerDb) const noexcept;
    double getCurrentDisplayRangeDb() const noexcept;

    static juce::String frequencyToMusicalNoteName (double freqHz);
    static double snapFrequencyToNearestSemitone (double freqHz) noexcept;

    void attachFloatingControlsToBand (int bandIndex);
    void updateFloatingControlsVisibilityAndValues();
    void stepSelectedBand (int delta);

    bool startNewCurveBandDrag (juce::Point<float> position);
    int findBandNodeAtPosition (juce::Point<float> pos, float maxRadiusPx = 14.0f) const noexcept;

    void drawTopToolbar (juce::Graphics& g, juce::Rectangle<int> bounds);
    void drawEqGridAndScales (juce::Graphics& g, juce::Rectangle<int> area);
    void drawSpectrumAnalyzer (juce::Graphics& g, juce::Rectangle<int> area);
    void drawEqCurvesAndBandNodes (juce::Graphics& g, juce::Rectangle<int> area);
    void drawBandParameterTooltip (juce::Graphics& g, juce::Rectangle<int> area, int bandIndex);
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
    juce::TextButton helpButton  { "? Help" };

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
    juce::ToggleButton analyzerToggleButton { "Analyzer" };
    juce::ToggleButton pianoToggleButton    { "Piano" };
    juce::TextButton   range12Button        { "12 dB" };
    juce::TextButton   range30Button        { "30 dB" };
    juce::ComboBox     displayRangeBox;
    juce::Slider       outputGainSlider;
    juce::ToggleButton globalBypassButton   { "Bypass" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> processingModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   analyzerPostAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   pianoDisplayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> displayRangeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   outputGainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   globalBypassAttachment;

    int attachedBandIndex = -1;
    int hoveredBandIndex  = -1;
    int draggingBandIndex = -1;
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

    std::vector<float> rawPreSpectrumDb;
    std::vector<float> rawPostSpectrumDb;
    std::vector<float> rawSidechainSpectrumDb;
    std::vector<float> smoothedPreSpectrumDb;
    std::vector<float> smoothedPostSpectrumDb;
    std::vector<float> smoothedSidechainSpectrumDb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProQ3AudioProcessorEditor)
};
