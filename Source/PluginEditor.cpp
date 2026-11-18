#include "PluginEditor.h"
#include <cmath>

namespace
{
    const juce::Colour kBgDark           { 0xff101820 };
    const juce::Colour kBarDark          { 0xff1c1e23 };
    const juce::Colour kGridLineMajor    { 0xff34434f };
    const juce::Colour kGridLineMinor    { 0xff212e39 };
    const juce::Colour kGridZeroLine     { 0xff464032 };
    const juce::Colour kTextMuted        { 0xff8a8f9b };
    const juce::Colour kTextBright       { 0xffe4e7ee };
    const juce::Colour kYellowCurve      { 0xff7de1d2 };
    const juce::Colour kYellowGlow       { 0x487de1d2 };
    const juce::Colour kYellowFillTop    { 0x387de1d2 };
    const juce::Colour kYellowFillBot    { 0x087de1d2 };
    const juce::Colour kFloatingPillBg   { 0xf0212228 };
    const juce::Colour kFloatingBorder   { 0xff4a5c68 };
    const juce::Colour kBypassRed        { 0xffd94343 };
    const juce::Colour kDynRedArc        { 0xffc8282e };
    const juce::Colour kDynYellowLive    { 0xffffc626 };

    juce::Colour getBandStereoColour (int stereoMode) noexcept
    {
        switch (stereoMode)
        {
            case 1:  return juce::Colour (0xfff2f4f8); // Left (White)
            case 2:  return juce::Colour (0xffff5a5a); // Right (Red)
            case 3:  return juce::Colour (0xff4be072); // Mid (Green)
            case 4:  return juce::Colour (0xff49b6ff); // Side (Blue)
            default: return kYellowCurve;              // Stereo (Golden Yellow)
        }
    }

    constexpr double kMinFreqHz = 10.0;
    constexpr double kMaxFreqHz = 30000.0;
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

    // Corpo cinza acetinado do knob (como na imagem image_435802.png)
    juce::ColourGradient knobGrad (juce::Colour (0xff565963), centre.x, ry,
                                   juce::Colour (0xff34373e), centre.x, ry + rw, false);
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
    g.setColour (slider.isEnabled() ? juce::Colour (0xffd5d8e0) : juce::Colour (0xff6d707a));
    g.fillPath (capWedge);

    // Traço escuro central do ponteiro dentro da gota clara
    juce::Path pointerLine;
    pointerLine.addRoundedRectangle (-0.9f, -capRadius + 3.5f, 1.8f, capRadius * 0.42f, 0.8f);
    pointerLine.applyTransform (juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));
    g.setColour (juce::Colour (0xff23252b));
    g.fillPath (pointerLine);
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
        border = toggled ? juce::Colour (0xff585e70) : kYellowCurve;
    else if (toggled)
        border = isBypassBtn ? kBypassRed : kYellowCurve;
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
    g.setColour (box.isMouseOver() ? juce::Colour (0xff383c47) : juce::Colour (0xff2d3039));
    g.fillRoundedRectangle (bounds, 4.0f);

    g.setColour (juce::Colour (0xff474c5a));
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

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
                                          bool /*hasSubMenu*/,
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

    g.setColour (isTicked ? kYellowCurve : (isActive ? kTextBright : kTextMuted));
    g.setFont (juce::FontOptions (13.0f));
    auto textArea = area.reduced (10, 0);
    if (isTicked)
        g.drawText ("v", textArea.removeFromLeft (14), juce::Justification::centredLeft, false);
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
        g.setColour (kYellowCurve.withAlpha (0.45f));
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

    g.setColour ((hoveringInner || draggingInnerKnob) ? kYellowCurve.withAlpha (0.7f) : juce::Colour (0xff6c707d));
    g.drawEllipse (rx, ry, rw, rw, 1.1f);

    // Gota clara do ponteiro do knob central (fiel à imagem image_435802.png)
    juce::Path capWedge;
    const float capR = innerR - 1.5f;
    capWedge.addPieSegment (centre.x - capR, centre.y - capR,
                            capR * 2.0f, capR * 2.0f,
                            gainAngle - 0.34f, gainAngle + 0.34f, 0.28f);
    g.setColour (juce::Colour (0xffe2e5ec));
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
    addAndMakeVisible (undoButton);
    undoButton.onClick = [this] { audioProcessor.undoManager.undo(); repaint(); };

    addAndMakeVisible (redoButton);
    redoButton.onClick = [this] { audioProcessor.undoManager.redo(); repaint(); };

    addAndMakeVisible (abButton);
    abButton.onClick = [this]
    {
        audioProcessor.toggleABState();
        abButton.setButtonText (audioProcessor.isStateBActive() ? "B/A" : "A/B");
        attachFloatingControlsToBand (audioProcessor.getSelectedBandIndex());
        repaint();
    };

    addAndMakeVisible (copyButton);
    copyButton.onClick = [this] { audioProcessor.copyActiveToInactiveState(); };

    addAndMakeVisible (prevPresetButton);
    prevPresetButton.onClick = [this] { presetNameButton.setButtonText ("Default"); };

    addAndMakeVisible (presetNameButton);
    presetNameButton.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Default (Flat 1 Band @ 1000 Hz)");
        m.addItem (2, "Dynamic De-Esser Bell (6 kHz, Q=3.0, Gain=0 dB, Dyn=-6 dB)");
        m.addItem (3, "Flat Tilt Mastering (+1.5 dB @ 1 kHz)");
        m.addItem (4, "Clear All 24 Bands");

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetNameButton),
            [this] (int res)
            {
                if (res == 0) return;
                audioProcessor.undoManager.beginNewTransaction ("Load Preset");

                for (int i = 0; i < ProQ3AudioProcessor::kMaxBands; ++i)
                {
                    if (auto* pu = audioProcessor.apvts.getParameter (ProQ3AudioProcessor::getBandParamID (i, "used")))
                        pu->setValueNotifyingHost (0.0f);
                }

                if (res == 1)
                {
                    audioProcessor.activateFirstFreeBand (1000.0f, 0.0f, 1.0f, ProQ3AudioProcessor::kShapeBell, 0.0f);
                    presetNameButton.setButtonText ("Default");
                }
                else if (res == 2)
                {
                    audioProcessor.activateFirstFreeBand (6000.0f, 0.0f, 3.0f, ProQ3AudioProcessor::kShapeBell, -6.0f);
                    presetNameButton.setButtonText ("Dynamic De-Esser 6 kHz");
                }
                else if (res == 3)
                {
                    audioProcessor.activateFirstFreeBand (1000.0f, 1.5f, 1.0f, ProQ3AudioProcessor::kShapeFlatTilt, 0.0f);
                    presetNameButton.setButtonText ("Flat Tilt Mastering");
                }
                else if (res == 4)
                {
                    audioProcessor.setSelectedBandIndex (-1);
                    presetNameButton.setButtonText ("Clean Slate");
                }
                attachFloatingControlsToBand (audioProcessor.getSelectedBandIndex());
                repaint();
            });
    };

    addAndMakeVisible (nextPresetButton);
    nextPresetButton.onClick = [this] { presetNameButton.onClick(); };

    addAndMakeVisible (helpButton);
    helpButton.setClickingTogglesState (true);
    helpButton.onClick = [this] { showHelpOverlay = helpButton.getToggleState(); repaint(); };

    //---- Floating Band Controls Pill (Fiel à imagem image_435802.png) ----
    addAndMakeVisible (bandBypassButton);
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

    addAndMakeVisible (gainQInterButton);
    gainQInterButton.setTooltip ("Gain-Q Interaction");

    // Botões do Arco Dinâmico acima do Knob GAIN (como na imagem image_435802.png):
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
    bypassDynamicsButton.setTooltip ("Bypass Dynamics (suspend dynamic processing while preserving static gain and dynamic range)");

    // 3. Botão Auto Threshold: alterna entre Auto e Manual Threshold (exibindo slider manual + medidor + botão Sidechain)
    addAndMakeVisible (autoThresholdButton);
    autoThresholdButton.setTooltip ("Toggle Auto Threshold / Manual Threshold Slider + External Sidechain");

    manualThresholdSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    manualThresholdSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 16);
    manualThresholdSlider.setTextValueSuffix (" dB");
    manualThresholdSlider.setTooltip ("Manual Dynamic Threshold (-60 dB to 0 dB)");
    addAndMakeVisible (manualThresholdSlider);

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
    processingModeBox.addItemList ({ "Zero Latency", "Natural Phase", "Linear Phase" }, 1);
    processingModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        audioProcessor.apvts, "processing_mode", processingModeBox);

    addAndMakeVisible (analyzerToggleButton);
    analyzerPostAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, "analyzer_post", analyzerToggleButton);

    addAndMakeVisible (pianoToggleButton);
    pianoDisplayAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, "piano_display", pianoToggleButton);

    addAndMakeVisible (range12Button);
    range12Button.onClick = [this]
    {
        if (auto* p = audioProcessor.apvts.getParameter ("display_range"))
            p->setValueNotifyingHost (p->convertTo0to1 (2.0f));
    };

    addAndMakeVisible (range30Button);
    range30Button.onClick = [this]
    {
        if (auto* p = audioProcessor.apvts.getParameter ("display_range"))
            p->setValueNotifyingHost (p->convertTo0to1 (3.0f));
    };

    addAndMakeVisible (displayRangeBox);
    displayRangeBox.addItemList ({ "3 dB", "6 dB", "12 dB", "30 dB" }, 1);
    displayRangeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        audioProcessor.apvts, "display_range", displayRangeBox);

    outputGainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    outputGainSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 64, 20);
    outputGainSlider.setTextValueSuffix (" dB");
    addAndMakeVisible (outputGainSlider);
    outputGainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        audioProcessor.apvts, "global_output_gain", outputGainSlider);

    addAndMakeVisible (globalBypassButton);
    globalBypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.apvts, "global_bypass", globalBypassButton);

    attachFloatingControlsToBand (audioProcessor.getSelectedBandIndex());

    setSize (1100, 640);
    startTimerHz (60);
}

ProQ3AudioProcessorEditor::~ProQ3AudioProcessorEditor()
{
    stopTimer();
    audioProcessor.setSoloBandIndex (-1);
    setLookAndFeel (nullptr);
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
    return static_cast<float> (graphBounds.getX()) + static_cast<float> (norm * graphBounds.getWidth());
}

double ProQ3AudioProcessorEditor::xToFrequency (float x) const noexcept
{
    if (graphBounds.getWidth() <= 1)
        return 1000.0;

    const double norm = juce::jlimit (0.0, 1.0,
        static_cast<double> (x - static_cast<float> (graphBounds.getX())) / static_cast<double> (graphBounds.getWidth()));
    return kMinFreqHz * std::pow (kMaxFreqHz / kMinFreqHz, norm);
}

float ProQ3AudioProcessorEditor::gainDbToY (double gainDb) const noexcept
{
    const double maxDb = getCurrentDisplayRangeDb();
    const double clamped = juce::jlimit (-maxDb, maxDb, gainDb);
    const double norm = 0.5 - (clamped / (2.0 * maxDb));
    return static_cast<float> (graphBounds.getY()) + static_cast<float> (norm * graphBounds.getHeight());
}

double ProQ3AudioProcessorEditor::yToGainDb (float y) const noexcept
{
    if (graphBounds.getHeight() <= 1)
        return 0.0;

    const double maxDb = getCurrentDisplayRangeDb();
    const double norm = juce::jlimit (0.0, 1.0,
        static_cast<double> (y - static_cast<float> (graphBounds.getY())) / static_cast<double> (graphBounds.getHeight()));
    return (0.5 - norm) * (2.0 * maxDb);
}

float ProQ3AudioProcessorEditor::analyzerDbToY (double analyzerDb) const noexcept
{
    const double clamped = juce::jlimit (-90.0, 0.0, analyzerDb);
    const double norm = -clamped / 90.0;
    return static_cast<float> (graphBounds.getY()) + static_cast<float> (norm * graphBounds.getHeight());
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
void ProQ3AudioProcessorEditor::attachFloatingControlsToBand (int bandIndex)
{
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
        const bool showDynTopControls = visible && (hasDynRange || ! bp.dynAutoThresh);
        clearDynamicsButton.setVisible (showDynTopControls);
        bypassDynamicsButton.setVisible (showDynTopControls);
        autoThresholdButton.setVisible (visible);

        const bool showManualThreshControls = visible && (! bp.dynAutoThresh);
        manualThresholdSlider.setVisible (showManualThreshControls);
        externalSidechainButton.setVisible (showManualThreshControls);
    }
    else
    {
        clearDynamicsButton.setVisible (false);
        bypassDynamicsButton.setVisible (false);
        autoThresholdButton.setVisible (false);
        manualThresholdSlider.setVisible (false);
        externalSidechainButton.setVisible (false);
    }

    const int rangeIdx = static_cast<int> (std::lround (
        audioProcessor.apvts.getRawParameterValue ("display_range")->load (std::memory_order_relaxed)));
    range12Button.setToggleState (rangeIdx == 2, juce::dontSendNotification);
    range30Button.setToggleState (rangeIdx == 3, juce::dontSendNotification);
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
void ProQ3AudioProcessorEditor::mouseMove (const juce::MouseEvent& event)
{
    lastMousePos = event.position;
    const int hit = findBandNodeAtPosition (event.position, 14.0f);
    if (hit != hoveredBandIndex)
    {
        hoveredBandIndex = hit;
        repaint();
    }
    else if (graphBounds.contains (event.getPosition()) || pianoBounds.contains (event.getPosition()))
    {
        repaint();
    }
}

void ProQ3AudioProcessorEditor::mouseDown (const juce::MouseEvent& event)
{
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

        // Alt + clique alterna o Bypass da banda
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
    lastMousePos = event.position;

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

void ProQ3AudioProcessorEditor::mouseUp (const juce::MouseEvent&)
{
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
    if (! graphBounds.contains (event.getPosition()) || event.mods.isPopupMenu())
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
    if (! graphBounds.contains (event.getPosition()))
        return;

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
    bool needsRepaint = false;

    if (audioProcessor.pullNextPreFftBlock (rawPreSpectrumDb))
    {
        if (smoothedPreSpectrumDb.size() != rawPreSpectrumDb.size())
            smoothedPreSpectrumDb = rawPreSpectrumDb;
        else
        {
            for (size_t i = 0; i < rawPreSpectrumDb.size(); ++i)
                smoothedPreSpectrumDb[i] = smoothedPreSpectrumDb[i] * 0.78f + rawPreSpectrumDb[i] * 0.22f;
        }
        needsRepaint = true;
    }

    if (audioProcessor.pullNextPostFftBlock (rawPostSpectrumDb))
    {
        if (smoothedPostSpectrumDb.size() != rawPostSpectrumDb.size())
            smoothedPostSpectrumDb = rawPostSpectrumDb;
        else
        {
            for (size_t i = 0; i < rawPostSpectrumDb.size(); ++i)
                smoothedPostSpectrumDb[i] = smoothedPostSpectrumDb[i] * 0.78f + rawPostSpectrumDb[i] * 0.22f;
        }
        needsRepaint = true;
    }

    if (audioProcessor.pullNextSidechainFftBlock (rawSidechainSpectrumDb))
    {
        if (smoothedSidechainSpectrumDb.size() != rawSidechainSpectrumDb.size())
            smoothedSidechainSpectrumDb = rawSidechainSpectrumDb;
        else
        {
            for (size_t i = 0; i < rawSidechainSpectrumDb.size(); ++i)
                smoothedSidechainSpectrumDb[i] = smoothedSidechainSpectrumDb[i] * 0.78f + rawSidechainSpectrumDb[i] * 0.22f;
        }
        needsRepaint = true;
    }

    const int procSelected = audioProcessor.getSelectedBandIndex();
    if (procSelected != attachedBandIndex)
        attachFloatingControlsToBand (procSelected);

    updateFloatingControlsVisibilityAndValues();
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

    if (needsRepaint)
        repaint (graphBounds);
    repaint (meterBounds);
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
        drawEqCurvesAndBandNodes (g, graphBounds);

        const int tooltipBand = (draggingBandIndex >= 0) ? draggingBandIndex : hoveredBandIndex;
        if (tooltipBand >= 0 && tooltipBand < ProQ3AudioProcessor::kMaxBands)
            drawBandParameterTooltip (g, graphBounds, tooltipBand);

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
        g.setColour (kYellowCurve);
        g.drawRoundedRectangle (helpBox, 8.0f, 1.5f);

        g.setColour (kTextBright);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText ("FABPRO Q4 • Dynamic EQ",
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
    juce::ColourGradient topGrad (juce::Colour (0xff23262e), 0.0f, 0.0f,
                                  juce::Colour (0xff17191e), 0.0f, static_cast<float> (bounds.getBottom()), false);
    g.setGradientFill (topGrad);
    g.fillRect (bounds);

    g.setColour (juce::Colour (0xff2d313b));
    g.fillRect (bounds.getX(), bounds.getBottom() - 1, bounds.getWidth(), 1);

    g.setColour (kYellowCurve);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("FABPRO Q4", 14, bounds.getY(), 132, bounds.getHeight(), juce::Justification::centredLeft, false);
}

void ProQ3AudioProcessorEditor::drawEqGridAndScales (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.saveState();
    g.reduceClipRegion (area);

    juce::ColourGradient bgGrad (juce::Colour (0xff1a2632), static_cast<float> (area.getCentreX()), static_cast<float> (area.getY()),
                                 kBgDark, static_cast<float> (area.getCentreX()), static_cast<float> (area.getBottom()), false);
    g.setGradientFill (bgGrad);
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
        g.drawText (m.label, static_cast<int> (x) - 18, area.getY() + 4, 36, 14, juce::Justification::centred, false);
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

        g.setColour (isZero ? kGridZeroLine : kGridLineMajor);
        g.drawHorizontalLine (static_cast<int> (y), static_cast<float> (area.getX()), static_cast<float> (area.getRight()));

        juce::String lbl = isZero ? "0" : ((db > 0.0 ? "+" : "") + juce::String (static_cast<int> (std::lround (db))));
        g.setColour (juce::Colour (0xffb89b4a));
        g.drawText (lbl, area.getX() + 6, static_cast<int> (y) - 7, 30, 14, juce::Justification::centredLeft, false);
        g.drawText (lbl, area.getRight() - 68, static_cast<int> (y) - 7, 30, 14, juce::Justification::centredRight, false);
    }

    g.setColour (juce::Colour (0xff666b77));
    for (int anaDb = -10; anaDb >= -80; anaDb -= 10)
    {
        const float y = std::round (analyzerDbToY (static_cast<double> (anaDb)));
        g.drawText (juce::String (anaDb), area.getRight() - 34, static_cast<int> (y) - 7, 28, 14, juce::Justification::centredRight, false);
    }

    g.restoreState();
}

void ProQ3AudioProcessorEditor::drawSpectrumAnalyzer (juce::Graphics& g, juce::Rectangle<int> area)
{
    const bool showPre  = audioProcessor.apvts.getRawParameterValue ("analyzer_pre")->load() >= 0.5f;
    const bool showPost = audioProcessor.apvts.getRawParameterValue ("analyzer_post")->load() >= 0.5f;

    if ((! showPre && ! showPost) || smoothedPostSpectrumDb.empty())
        return;

    g.saveState();
    g.reduceClipRegion (area);

    const double sr = audioProcessor.getCurrentSampleRate();
    const int numBins = static_cast<int> (smoothedPostSpectrumDb.size());

    auto buildSpectrumPath = [&] (const std::vector<float>& magDb, juce::Path& strokePath, juce::Path& fillPath)
    {
        if (magDb.empty())
            return;

        const float bottomY = static_cast<float> (area.getBottom());
        bool started = false;

        for (int px = area.getX(); px <= area.getRight(); px += 2)
        {
            const double freq = xToFrequency (static_cast<float> (px));
            const double exactBin = (freq * static_cast<double> (ProQ3AudioProcessor::kFftSize)) / sr;
            const int bin0 = juce::jlimit (1, numBins - 2, static_cast<int> (exactBin));
            const float frac = static_cast<float> (exactBin - static_cast<double> (bin0));
            const float db = magDb[static_cast<size_t> (bin0)] * (1.0f - frac)
                           + magDb[static_cast<size_t> (bin0 + 1)] * frac;

            const float y = analyzerDbToY (static_cast<double> (db));
            if (! started)
            {
                strokePath.startNewSubPath (static_cast<float> (px), y);
                fillPath.startNewSubPath (static_cast<float> (px), bottomY);
                fillPath.lineTo (static_cast<float> (px), y);
                started = true;
            }
            else
            {
                strokePath.lineTo (static_cast<float> (px), y);
                fillPath.lineTo (static_cast<float> (px), y);
            }
        }

        if (started)
        {
            fillPath.lineTo (static_cast<float> (area.getRight()), bottomY);
            fillPath.closeSubPath();
        }
    };

    if (showPre && ! smoothedPreSpectrumDb.empty())
    {
        juce::Path preStroke, preFill;
        buildSpectrumPath (smoothedPreSpectrumDb, preStroke, preFill);
        g.setColour (juce::Colour (0x44757b88));
        g.strokePath (preStroke, juce::PathStrokeType (1.0f));
    }

    if (showPost && ! smoothedPostSpectrumDb.empty())
    {
        juce::Path postStroke, postFill;
        buildSpectrumPath (smoothedPostSpectrumDb, postStroke, postFill);

        juce::ColourGradient specGrad (juce::Colour (0x55a88842), 0.0f, static_cast<float> (area.getY() + 60),
                                       juce::Colour (0x1230343f), 0.0f, static_cast<float> (area.getBottom()), false);
        g.setGradientFill (specGrad);
        g.fillPath (postFill);

        g.setColour (juce::Colour (0x99b8bcc8));
        g.strokePath (postStroke, juce::PathStrokeType (1.25f));
    }

    // Se a banda selecionada estiver com External Sidechain ativo, desenha o espectro do Sidechain em vermelho claro (Manual p. 25)
    if (attachedBandIndex >= 0 && ! smoothedSidechainSpectrumDb.empty())
    {
        const auto bp = audioProcessor.getBandParameters (attachedBandIndex);
        if (! bp.dynAutoThresh && bp.dynExtSidechain)
        {
            juce::Path scStroke, scFill;
            buildSpectrumPath (smoothedSidechainSpectrumDb, scStroke, scFill);
            g.setColour (juce::Colour (0xaaee5555));
            g.strokePath (scStroke, juce::PathStrokeType (1.2f));
        }
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

    const auto displayRange = getCurrentDisplayRangeDb();
    const auto toY = [area, displayRange] (double db)
    {
        const auto clamped = juce::jlimit (-displayRange, displayRange, db);
        return static_cast<float> (area.getY() + area.getHeight() * 0.5 - clamped * area.getHeight() / (2.0 * displayRange));
    };

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
        const auto staticResponse = audioProcessor.makeBandResponse (bp);
        const auto liveResponse = isModulated ? audioProcessor.makeBandResponse (bp, dynamicGain) : staticResponse;
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
        const auto stereoCol = getBandStereoColour (bp.stereoMode);
        const float alphaMul = (bp.enabled && ! globalBypass) ? 1.0f : 0.35f;
        if (isSelected || isHovered || hasDyn)
        {
            g.setColour (stereoCol.withAlpha ((isSelected ? 0.16f : 0.09f) * alphaMul));
            g.fillPath (bandFill);
        }
        g.setColour (stereoCol.withAlpha ((isSelected ? 0.78f : 0.38f) * alphaMul));
        g.strokePath (bandLiveCurve, juce::PathStrokeType (isSelected ? 1.5f : 1.0f));
    }

    juce::Path totalLiveCurve;
    juce::Path totalStaticCurve;
    juce::Path totalFill;
    totalLiveCurve.startNewSubPath (static_cast<float> (area.getX()), toY (totalLiveDb.front()));
    totalStaticCurve.startNewSubPath (static_cast<float> (area.getX()), toY (totalStaticDb.front()));
    totalFill.startNewSubPath (static_cast<float> (area.getX()), zeroY);

    for (size_t point = 0; point < pointCount; ++point)
    {
        const float x = static_cast<float> (area.getX() + 2 * static_cast<int> (point));
        const float y = toY (totalLiveDb[point]);
        totalLiveCurve.lineTo (x, y);
        totalStaticCurve.lineTo (x, toY (totalStaticDb[point]));
        totalFill.lineTo (x, y);
    }

    totalFill.lineTo (static_cast<float> (area.getRight()), zeroY);
    totalFill.closeSubPath();

    juce::ColourGradient curveFillGrad (kYellowFillTop.withMultipliedAlpha (globalBypass ? 0.35f : 1.0f),
                                        0.0f, static_cast<float> (area.getY()),
                                        kYellowFillBot.withMultipliedAlpha (globalBypass ? 0.35f : 1.0f),
                                        0.0f, static_cast<float> (area.getBottom()), false);
    g.setGradientFill (curveFillGrad);
    g.fillPath (totalFill);

    // Traço fantasma da curva estática quando a dinâmica está atuando
    g.setColour (kYellowCurve.withAlpha (0.28f));
    g.strokePath (totalStaticCurve, juce::PathStrokeType (1.2f));

    // Brilho e curva principal acompanhando em tempo real o ganho aplicado pelo DSP
    g.setColour (kYellowGlow.withMultipliedAlpha (globalBypass ? 0.3f : 1.0f));
    g.strokePath (totalLiveCurve, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (globalBypass ? juce::Colour (0xff7e7352) : kYellowCurve);
    g.strokePath (totalLiveCurve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // 3. Nós numerados das bandas ativas (1 a 24) + Indicador de Dynamic Range no display
    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    for (int b = 0; b < ProQ3AudioProcessor::kMaxBands; ++b)
    {
        const auto bp = audioProcessor.getBandParameters (b);
        if (! bp.used)
            continue;

        const float nx = frequencyToX (bp.freqHz);
        const float ny = gainDbToY (bp.gainDb);
        const bool isSelected = (b == selectedBand);
        const bool isHovered  = (b == hoveredBandIndex);
        const juce::Colour nodeCol = getBandStereoColour (bp.stereoMode);

        // Se a banda tem Dynamic Range != 0, desenha a haste vertical de alcance dinâmico e o ponto da redução atual (Manual p. 7 & p. 13)
        if (ProQ3AudioProcessor::doesShapeSupportDynamics (bp.shape) && std::abs (bp.dynRangeDb) > 0.05)
        {
            const float dynTargetY = gainDbToY (bp.gainDb + bp.dynRangeDb);
            const float liveModDb  = audioProcessor.getBandDynamicGainDb (b);
            const float liveY      = gainDbToY (bp.gainDb + static_cast<double> (liveModDb));

            g.setColour (bp.dynEnabled ? kDynRedArc.withAlpha (0.75f) : juce::Colour (0xff666a75));
            g.drawLine (nx, ny, nx, dynTargetY, 1.8f);

            // Alça triangular/losango no limite do Dynamic Range
            juce::Path handle;
            handle.addTriangle (nx - 4.5f, dynTargetY, nx + 4.5f, dynTargetY,
                                nx, dynTargetY + (bp.dynRangeDb < 0.0 ? 5.0f : -5.0f));
            g.fillPath (handle);

            if (bp.dynEnabled && std::abs (liveModDb) > 0.03f)
            {
                g.setColour (kDynYellowLive);
                g.drawLine (nx, ny, nx, liveY, 3.2f);
                g.fillEllipse (nx - 3.5f, liveY - 3.5f, 7.0f, 7.0f);
            }
        }

        const float radius = isSelected ? 11.5f : (isHovered ? 10.5f : 9.5f);
        juce::Rectangle<float> dotRect (nx - radius, ny - radius, radius * 2.0f, radius * 2.0f);

        if (isSelected || isHovered)
        {
            g.setColour (nodeCol.withAlpha (0.28f));
            g.fillEllipse (dotRect.expanded (4.5f));
        }

        g.setColour (bp.enabled ? (isSelected ? juce::Colour (0xff3a2e10) : juce::Colour (0xff23252c))
                                : juce::Colour (0xff1a1c20));
        g.fillEllipse (dotRect);

        g.setColour (bp.enabled ? nodeCol : juce::Colour (0xff6e7380));
        g.drawEllipse (dotRect, isSelected ? 2.2f : 1.6f);

        g.setColour (bp.enabled ? juce::Colour (0xfff7f8fb) : juce::Colour (0xff7c818e));
        g.drawText (juce::String (b + 1), dotRect.toNearestInt(), juce::Justification::centred, false);
    }

    if (isSelectionRectDragging && ! selectionLassoRect.isEmpty())
    {
        g.setColour (juce::Colour (0x28ffc93c));
        g.fillRect (selectionLassoRect);
        g.setColour (kYellowCurve.withAlpha (0.7f));
        g.drawRect (selectionLassoRect, 1.0f);
    }

    g.restoreState();
}

void ProQ3AudioProcessorEditor::drawBandParameterTooltip (juce::Graphics& g, juce::Rectangle<int> area, int bandIndex)
{
    const auto bp = audioProcessor.getBandParameters (bandIndex);
    if (! bp.used)
        return;

    const float nx = frequencyToX (bp.freqHz);
    const float ny = gainDbToY (bp.gainDb);

    const int boxW = 172;
    const int boxH = 68;
    int boxX = static_cast<int> (nx) - boxW / 2;
    int boxY = static_cast<int> (ny) - boxH - 20;

    if (boxY < area.getY() + 8)
        boxY = static_cast<int> (ny) + 20;
    boxX = juce::jlimit (area.getX() + 8, area.getRight() - boxW - 8, boxX);

    juce::Rectangle<int> box (boxX, boxY, boxW, boxH);
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
    g.drawText (freqStr + "  [" + noteStr + "]", box.getX() + 8, box.getY() + 5, box.getWidth() - 16, 15, juce::Justification::centredLeft, false);

    g.setColour (kYellowCurve);
    g.drawText (gainStr + dynStr, box.getX() + 8, box.getY() + 23, box.getWidth() - 16, 15, juce::Justification::centredLeft, false);

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
        g.setColour (getBandStereoColour (bp.stereoMode));
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

    // Formato orgânico do painel flutuante com a crista/cúpula central acima do knob GAIN para o botão "Auto"
    // (Exatamente como na imagem image_435802.png enviada pelo usuário!)
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

    // Brilho esverdeado/dourado sutil ao redor da borda superior quando selecionado (como na imagem image_435802.png)
    g.setColour (juce::Colour (0x284be072));
    g.strokePath (pillShape, juce::PathStrokeType (4.0f));

    g.setColour (juce::Colour (0x77000000));
    g.fillPath (pillShape, juce::AffineTransform::translation (0.0f, 3.0f));

    g.setColour (kFloatingPillBg);
    g.fillPath (pillShape);

    g.setColour (kFloatingBorder);
    g.strokePath (pillShape, juce::PathStrokeType (1.2f));

    // Se estiver no modo de Limiar Manual (!bp.dynAutoThresh), desenha a barra do medidor do sinal detector
    // atrás/abaixo do slider de Threshold manual (Manual Pro-Q 3 p. 14: "The level of the trigger signal is shown in the slider")
    if (! bp.dynAutoThresh)
    {
        auto meterArea = manualThreshMeterBounds.toFloat();
        g.setColour (juce::Colour (0xff14161c));
        g.fillRoundedRectangle (meterArea, 3.0f);

        const float detDb = audioProcessor.getBandDetectorLevelDb (attachedBandIndex);
        const float norm  = juce::jlimit (0.0f, 1.0f, (detDb + 60.0f) / 60.0f);
        if (norm > 0.01f)
        {
            auto fillRect = meterArea.withWidth (meterArea.getWidth() * norm);
            g.setColour (detDb >= static_cast<float> (bp.dynThresholdDb) ? kDynYellowLive : juce::Colour (0xff4be072));
            g.fillRoundedRectangle (fillRect, 3.0f);
        }

        g.setColour (juce::Colour (0xff484d5c));
        g.drawRoundedRectangle (meterArea, 3.0f, 1.0f);
    }
}

void ProQ3AudioProcessorEditor::drawOutputMeter (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour (juce::Colour (0xff121317));
    g.fillRect (bounds);

    g.setColour (juce::Colour (0xff2a2d36));
    g.fillRect (bounds.getX(), bounds.getY(), 1, bounds.getHeight());

    g.setColour (kTextMuted);
    g.setFont (juce::FontOptions (10.5f));
    g.drawText ("dB", bounds.getX(), bounds.getY() + 2, bounds.getWidth(), 14, juce::Justification::centred, false);

    const float dbL = audioProcessor.getOutputPeakDbL();
    const float dbR = audioProcessor.getOutputPeakDbR();

    auto drawBar = [&] (int x, int w, float dbVal)
    {
        const int topY = bounds.getY() + 18;
        const int botY = bounds.getBottom() - 6;
        const int totalH = botY - topY;
        const int numSegments = 48;

        const float norm = juce::jlimit (0.0f, 1.0f, (dbVal + 60.0f) / 60.0f);
        const int activeSegs = static_cast<int> (std::lround (norm * static_cast<float> (numSegments)));

        for (int s = 0; s < numSegments; ++s)
        {
            const float segRatio = static_cast<float> (s) / static_cast<float> (numSegments);
            const int segY = botY - static_cast<int> ((s + 1) * totalH / numSegments);
            const int segH = juce::jmax (2, (totalH / numSegments) - 1);

            if (s < activeSegs)
            {
                if (segRatio > 0.92f)
                    g.setColour (juce::Colour (0xffff4b4b));
                else if (segRatio > 0.72f)
                    g.setColour (kYellowCurve);
                else
                    g.setColour (juce::Colour (0xff67c948));
            }
            else
            {
                g.setColour (juce::Colour (0xff22252c));
            }
            g.fillRect (x, segY, w, segH);
        }
    };

    const int barW = (bounds.getWidth() - 7) / 2;
    drawBar (bounds.getX() + 3, barW, dbL);
    drawBar (bounds.getX() + 4 + barW, barW, dbR);
}

void ProQ3AudioProcessorEditor::drawBottomToolbar (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour (juce::Colour (0xff131d27));
    g.fillRect (bounds);

    g.setColour (kFloatingBorder);
    g.drawHorizontalLine (bounds.getY(), static_cast<float> (bounds.getX()), static_cast<float> (bounds.getRight()));

    g.setColour (juce::Colour (0xff283845));
    for (const int x : { 324, bounds.getRight() - 260 })
        g.drawVerticalLine (x, static_cast<float> (bounds.getY() + 8), static_cast<float> (bounds.getBottom() - 8));
}

//==============================================================================
// Layout Responsivo (Fiel ao Painel da Banda da image_435802.png)
//==============================================================================
void ProQ3AudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    topBarBounds    = area.removeFromTop (36);
    bottomBarBounds = area.removeFromBottom (36);
    meterBounds     = area.removeFromRight (24);
    pianoBounds     = area.withTrimmedTop (area.getHeight() - 22);
    graphBounds     = area;

    //---- Top Bar ----
    int tx = 162;
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

    helpButton.setBounds (topBarBounds.getRight() - 74, ty, 62, th);

    //---- Floating Band Controls Pill (Proporções exatas da image_435802.png) ----
    const int pillW = 472;
    const int pillH = 126;
    const int pillX = graphBounds.getX() + (graphBounds.getWidth() - pillW) / 2;
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
    manualThreshMeterBounds = juce::Rectangle<int> (gainCenterX - 54, pillY - 36, 118, 6);
    manualThresholdSlider.setBounds   (gainCenterX - 56, pillY - 30, 120, 16);
    externalSidechainButton.setBounds (gainCenterX + 66, pillY - 32, 32,  18);

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

    //---- Bottom Bar ----
    const int by = bottomBarBounds.getY() + 6;
    const int bh = 24;

    processingModeBox.setBounds    (12,  by, 128, bh);
    analyzerToggleButton.setBounds (148, by, 86,  bh);
    pianoToggleButton.setBounds    (242, by, 72,  bh);

    const int midX = juce::jlimit (368, bottomBarBounds.getRight() - 410, bottomBarBounds.getCentreX());
    range12Button.setBounds   (midX - 40, by, 54, bh);
    range30Button.setBounds   (midX + 18, by, 54, bh);
    displayRangeBox.setBounds (midX + 78, by, 72, bh);

    globalBypassButton.setBounds (bottomBarBounds.getRight() - 84, by, 72, bh);
    outputGainSlider.setBounds   (bottomBarBounds.getRight() - 250, by, 158, bh);
}
