// =============================================================================
// PluginEditor.h  —  BADTVibe GUI
// =============================================================================
//
// Layout (four sections divided by vertical rules, rack-mount style):
//
//  ┌─ BADT ──────────────────────────────────────────────────────[L]─┐
//  │ [A][P] [A][P] [A][P] │         │                         │ ##[R]│
//  │  TIME   AMP  PITCH   │  LFO    │ DVOL DRYPAN WETPAN WVOL│ ##   │
//  │  (◎)    (◎)   (◎)    │         │ [|]   (◎)    (◎)   [|] │ ##   │
//  │ [BYP]  [BYP]  [BYP]  │ [RATE]  │  [SOLO DRY] [SOLO WET] │ ##   │
//  │  ms     dB   cents   │ [DEPTH] │                         │ ##   │
//  ├──────────────────────────────────────────────────────────────────┤
//  │ DRY EQ ──────────────────────────────  WET EQ ─────────────────  │
//  │  LO SHELF  BELL PEAK  HI SHELF          (same layout)            │
//  │ F[──●──] F[──●──] F[──●──]  F[──●──] F[──●──] F[──●──]         │
//  │ Q[──●──] Q[──●──] Q[──●──]  Q[──●──] Q[──●──] Q[──●──]         │
//  │ G[──●──] G[──●──] G[──●──]  G[──●──] G[──●──] G[──●──]         │
//  └──────────────────────────────────────────────────────────────────┘
//
// =============================================================================
#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "BADTLookAndFeel.h"
#include <BinaryData.h>


// =============================================================================
// VUMeterComponent — vertical level meter with dB scale
// =============================================================================
class VUMeterComponent : public juce::Component,
                         public juce::Timer
{
public:
    explicit VUMeterComponent(std::atomic<float>& level)
        : audioLevel(level), displayLevel(0.0f)
    {
        startTimerHz(24);
    }

    ~VUMeterComponent() override { stopTimer(); }

    void timerCallback() override
    {
        float newLevel = audioLevel.load(std::memory_order_relaxed);
        if (newLevel > displayLevel)
            displayLevel = newLevel;
        else
            displayLevel *= 0.92f;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        // Reserve the right portion for dB scale labels.
        // The bar itself fills the left strip; labels go on the right.
        const float scaleW  = 22.0f;
        const float barW    = bounds.getWidth() - scaleW;

        auto barBounds  = bounds.withWidth(barW);
        auto scaleBounds = bounds.withLeft(barW);

        // Background
        g.fillAll(juce::Colour(0xFF111111));

        // Bar
        if (displayLevel > 0.0001f)
        {
            const float fraction = juce::jlimit(0.0f, 1.0f, displayLevel);
            juce::Colour barColour;
            if      (fraction < 0.25f) barColour = juce::Colour(0xFF22CC22);
            else if (fraction < 0.71f) barColour = juce::Colour(0xFFDDCC00);
            else                       barColour = juce::Colour(0xFFDD2222);

            const float barHeight = fraction * barBounds.getHeight();
            g.setColour(barColour);
            g.fillRect(barBounds.getX(),
                       barBounds.getBottom() - barHeight,
                       barBounds.getWidth(),
                       barHeight);
        }

        // Border around bar
        g.setColour(juce::Colour(0xFF444444));
        g.drawRect(barBounds, 1.0f);

        // dB scale: 0, -6, -12, -18, -24 dBFS
        // -x dBFS = 10^(x/20) linear. Map linear → y position:
        //   linear 0..1 occupies the full bar height (bottom=1.0, top=0.0)
        // Using the bar's colour thresholds (-3 dBFS ≈ 0.708, -12 dBFS ≈ 0.251):
        //   y = bottom - linear * height
        g.setFont(juce::Font(juce::FontOptions().withHeight(7.5f)));
        g.setColour(juce::Colour(0xFF888888));

        const float bTop    = barBounds.getY();
        const float bBottom = barBounds.getBottom();
        const float bH      = bBottom - bTop;

        static const float dBMarks[] = { 0.0f, -6.0f, -12.0f, -18.0f, -24.0f };
        for (float db : dBMarks)
        {
            // linear amplitude for this dB mark
            const float lin = std::pow(10.0f, db / 20.0f);
            const float yPos = bBottom - lin * bH;

            // Tick mark
            g.drawLine(barW - 3.0f, yPos, barW, yPos, 1.0f);

            // Label (right-aligned in the scale strip)
            juce::String lbl = (db == 0.0f) ? "0" : juce::String(static_cast<int>(db));
            g.drawFittedText(lbl,
                             static_cast<int>(barW + 1),
                             static_cast<int>(yPos - 5.0f),
                             static_cast<int>(scaleW - 2),
                             10,
                             juce::Justification::centredLeft, 1);
        }
    }

private:
    std::atomic<float>& audioLevel;
    float               displayLevel;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VUMeterComponent)
};


// =============================================================================
// DoubleClickKnob — Slider that fires a callback on double-click
// =============================================================================
class DoubleClickKnob : public juce::Slider
{
public:
    DoubleClickKnob() : juce::Slider() {}

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.getNumberOfClicks() >= 2)
        {
            if (onDoubleClick) onDoubleClick();
            return;
        }
        juce::Slider::mouseDown(event);
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (onDoubleClick) onDoubleClick();
    }

    std::function<void()> onDoubleClick;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DoubleClickKnob)
};


// =============================================================================
// DigitalDisplay — green-on-black LCD-style readout
// =============================================================================
class DigitalDisplay : public juce::Component
{
public:
    DigitalDisplay() = default;
    ~DigitalDisplay() override = default;

    void setText(const juce::String& s) { if (text != s) { text = s; repaint(); } }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xFF1A1A1A));
        g.fillRoundedRectangle(b, 3.0f);
        g.setColour(juce::Colour(0xFF030A04));
        g.fillRoundedRectangle(b.reduced(1.5f), 2.0f);
        g.setColour(juce::Colour(0xFF00E050));
        static auto typeface = juce::Typeface::createSystemTypefaceFor(
            BinaryData::digital7_ttf, BinaryData::digital7_ttfSize);
        g.setFont(juce::Font(typeface).withHeight(11.0f));
        g.drawFittedText(text, getLocalBounds().reduced(4, 2), juce::Justification::centred, 1);
    }

private:
    juce::String text { "--" };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DigitalDisplay)
};


// =============================================================================
// EQBandRow — one EQ band: gain slider only (freq/Q static)
// =============================================================================
struct EQBandRow
{
    juce::Slider gainSlider;
    juce::Label  bandLabel, gainLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        gainAttach;
};


// =============================================================================
// EQSection — inline EQ panel (3 bands, vertical gain sliders only)
// =============================================================================
class EQSection : public juce::Component
{
public:
    EQSection(BADTAudioProcessor& proc, bool isDry);
    ~EQSection() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    BADTAudioProcessor& audioProcessor;
    bool                isForDry;

    juce::Label titleLabel;
    EQBandRow   bands[3];

    static const char* BAND_NAMES[3];

    void setupBand(int idx,
                   const char* gainId,
                   const juce::String& name);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EQSection)
};


// =============================================================================
// BADTEditor — main plugin window
// =============================================================================
class BADTEditor : public juce::AudioProcessorEditor,
                   public juce::Timer
{
public:
    explicit BADTEditor(BADTAudioProcessor& processor);
    ~BADTEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    BADTAudioProcessor& audioProcessor;
    BADTLookAndFeel     lookAndFeel;

    // ===== Main modulation knobs (rotary) =====
    juce::Slider timeKnob, amplitudeKnob, pitchKnob;
    juce::Label  timeLabel, amplitudeLabel, pitchLabel;

    // ===== Per-stage A/P source toggles =====
    juce::ToggleButton timeAPButton, ampAPButton, pitchAPButton;

    // ===== Digital debug readouts =====
    DigitalDisplay timeDisplay, ampDisplay, pitchDisplay;

    // ===== LFO knobs =====
    juce::Slider lfoRateKnob, lfoDepthKnob;
    juce::Label  lfoRateLabel, lfoDepthLabel;

    // ===== Pan knobs (double-click → no-op now EQ is inline) =====
    juce::Slider dryPanKnob, wetPanKnob;
    juce::Label  dryPanLabel, wetPanLabel;

    // ===== Volume sliders (LinearVertical) =====
    juce::Slider dryVolSlider, wetVolSlider;
    juce::Label  dryVolLabel,  wetVolLabel;

    // ===== Stereo VU meters =====
    VUMeterComponent outputVUL, outputVUR;

    // ===== APVTS Attachments =====
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        timeAttach, ampAttach, pitchAttach,
        dryVolAttach, wetVolAttach,
        dryPanAttach, wetPanAttach,
        lfoRateAttach, lfoDepthAttach;

    // ===== Inline EQ sections =====
    EQSection dryEQ, wetEQ;

    // ===== Solo / bypass / invert buttons =====
    juce::ToggleButton soloDryButton, soloWetButton;
    juce::ToggleButton bypassTimeButton, bypassAmpButton, bypassPitchButton;
    juce::ToggleButton invertTimeButton, invertAmpButton, invertPitchButton;

    void timerCallback() override;

    void setupKnob(juce::Slider& knob, juce::Label& label,
                   const juce::String& text, bool small = false);
    void setupAPButton(juce::ToggleButton& btn, std::atomic<bool>& flag);
    void setupBypassButton(juce::ToggleButton& btn, std::atomic<bool>& flag);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BADTEditor)
};
