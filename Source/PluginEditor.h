// =============================================================================
// PluginEditor.h  —  BADTVibe GUI
// =============================================================================
//
// The PluginEditor is the visual window you see when you open the plugin.
// It is separate from the PluginProcessor because:
//   - The DAW can destroy/recreate the GUI at any time (user closes the window)
//   - Audio processing must continue even with no GUI visible
//   - Keeping UI and DSP code separate makes both easier to maintain
//
// GUI LAYOUT (top to bottom):
//
//   ┌─────────────────────────────────────────────────────────┐
//   │                    BADT                           [OUT] │  Title + VU
//   ├─────────────────────────────────────────────────────────┤
//   │          │  TIME  │  AMP   │ PITCH │                    │  Main row
//   ├─────────────────────────────────────────────────────────┤
//   │ DVOL│DRY PAN│     LFO     │WET PAN│WVOL│               │  Pan row
//   │     │(dbl-clk             │(dbl-clk    │               │
//   │     │→ Dry EQ)            │→ Wet EQ)   │               │
//   └─────────────────────────────────────────────────────────┘
//
//   Double-clicking a pan knob opens the EQ for that path in its own
//   floating OS window (addToDesktop) — not an overlay.
//
// =============================================================================
#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"


// =============================================================================
// CLASS: VUMeterComponent
// =============================================================================
//
// A simple vertical level meter that reads from an atomic<float> and repaints
// itself at 24 fps via a timer.
//
// The audio thread continuously writes the current peak level (0.0–1.0) to
// the atomic. The GUI timer reads it and decays toward zero over time,
// creating a "peak hold with decay" behaviour.
//
// COLOUR BANDS:
//   Green  — up to -12 dBFS (normal operating level)
//   Yellow — -12 to -3 dBFS (approaching headroom limit)
//   Red    — above -3 dBFS  (danger zone / clipping imminent)
//
class VUMeterComponent : public juce::Component,
                         public juce::Timer
{
public:
    // level: reference to the atomic float that the audio thread writes.
    // We store a reference — this is safe because the processor outlives the editor.
    explicit VUMeterComponent(std::atomic<float>& level)
        : audioLevel(level), displayLevel(0.0f)
    {
        // startTimerHz(24): call timerCallback() 24 times per second.
        // 24 fps is smooth enough for a VU display without wasting CPU.
        startTimerHz(24);
    }

    ~VUMeterComponent() override
    {
        stopTimer(); // Always stop the timer before the component is destroyed
    }

    void timerCallback() override
    {
        // Read the latest peak from the audio thread (atomic load, lock-free).
        float newLevel = audioLevel.load(std::memory_order_relaxed);

        // Instant attack: jump to a new peak immediately if it's higher.
        // Slow decay:     multiply by 0.92 each frame (≈ 12 frames to halve).
        // This "VU-style" behaviour makes transients clearly visible.
        if (newLevel > displayLevel)
            displayLevel = newLevel;
        else
            displayLevel *= 0.92f;

        repaint(); // Ask JUCE to redraw this component
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.fillAll(juce::Colour(0xFF111111)); // Dark background

        if (displayLevel > 0.0001f) // Skip drawing if essentially silent
        {
            // Map linear 0–1 amplitude to a fraction of the bar height.
            // juce::jlimit clamps the value to [0, 1] to prevent overflow.
            float fraction = juce::jlimit(0.0f, 1.0f, displayLevel);

            // Choose colour based on level.
            // Thresholds chosen to match -12 dBFS and -3 dBFS.
            //   -12 dBFS = 10^(-12/20) ≈ 0.251 linear amplitude
            //   -3 dBFS  = 10^(-3/20)  ≈ 0.708 linear amplitude
            juce::Colour barColour;
            if      (fraction < 0.25f) barColour = juce::Colour(0xFF22CC22); // Green
            else if (fraction < 0.71f) barColour = juce::Colour(0xFFDDCC00); // Yellow
            else                       barColour = juce::Colour(0xFFDD2222); // Red

            float barHeight = fraction * bounds.getHeight();

            // Draw bar from the bottom upward.
            g.setColour(barColour);
            g.fillRect(bounds.getX(),
                       bounds.getBottom() - barHeight,
                       bounds.getWidth(),
                       barHeight);
        }

        // Thin border around the meter
        g.setColour(juce::Colour(0xFF444444));
        g.drawRect(bounds, 1.0f);
    }

private:
    std::atomic<float>& audioLevel;  // Written by audio thread
    float               displayLevel; // Decayed display value (GUI thread only)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VUMeterComponent)
};


// =============================================================================
// CLASS: DoubleClickKnob
// =============================================================================
//
// A Slider subclass whose double-click fires a callback instead of resetting
// the value to default (the standard JUCE double-click behaviour).
//
// We use this for the DRY PAN and WET PAN knobs — double-clicking them opens
// the corresponding EQ panel instead of resetting the pan position.
//
// "std::function<void()>" is a type that can hold any callable (lambda, function
// pointer, member function pointer). We use it so the caller can set the
// double-click action without us needing to know what it does.
//
class DoubleClickKnob : public juce::Slider
{
public:
    DoubleClickKnob() : juce::Slider() {}

    // Intercept on mouseDown rather than mouseDoubleClick.
    //
    // WHY: In JUCE 8, Slider delegates event handling to a private Pimpl object
    // that may start a drag before mouseDoubleClick is dispatched.  By catching
    // the second click at mouseDown time (getNumberOfClicks() == 2) we guarantee
    // we see it before any drag logic runs.
    //
    // On a single click (getNumberOfClicks() == 1) we fall through to the normal
    // Slider::mouseDown so the knob still drags correctly.
    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.getNumberOfClicks() >= 2)
        {
            if (onDoubleClick) onDoubleClick();
            return; // don't start a drag on the second click
        }
        juce::Slider::mouseDown(event);
    }

    // Keep mouseDoubleClick as a belt-and-braces fallback.
    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (onDoubleClick) onDoubleClick();
    }

    // Set this from the editor to define what happens on double-click.
    std::function<void()> onDoubleClick;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DoubleClickKnob)
};


// =============================================================================
// CLASS: EQBandRow
// =============================================================================
//
// Helper struct holding three Sliders (Freq, Q, Gain) for one EQ band,
// plus their SliderAttachments and labels. Grouping them together avoids
// 27 separate member variables in EQPanel (9 bands × 3 params each = 27).
//
// std::unique_ptr<T> is a "smart pointer" that automatically deletes the object
// when the struct is destroyed — no manual delete needed.
//
struct EQBandRow
{
    juce::Slider freqKnob, qKnob, gainKnob;
    juce::Label  bandLabel, freqLabel, qLabel, gainLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        freqAttach, qAttach, gainAttach;
};


// =============================================================================
// CLASS: EQPanel
// =============================================================================
//
// A full-width overlay component showing a 3-band parametric EQ for either
// the dry or wet signal path. Appears when a pan knob is double-clicked.
//
// Layout (3 columns = 3 bands):
//
//   ┌──────── DRY EQ ─────────────────────────────────────[X]─┐
//   │  LOW SHELF     │    BELL PEAK     │   HIGH SHELF         │
//   │  FREQ  Q  GAIN │  FREQ  Q  GAIN  │  FREQ  Q  GAIN       │
//   └──────────────────────────────────────────────────────────┘
//
// The panel sits on top of the main plugin UI and is hidden until needed.
//
class EQPanel : public juce::Component
{
public:
    // proc:   reference to the processor (gives us access to apvts)
    // isDry:  true = Dry EQ path, false = Wet EQ path
    // onClose: called when the user clicks the X button
    EQPanel(BADTAudioProcessor& proc, bool isDry,
            std::function<void()> onClose);
    ~EQPanel() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    BADTAudioProcessor& audioProcessor;
    bool                    isForDry;
    std::function<void()>   closeCallback;

    juce::Label      titleLabel;
    juce::TextButton closeButton;

    // 3 bands, each with Freq/Q/Gain knobs + labels
    EQBandRow bands[3];

    // Names shown above each column
    static const char* BAND_NAMES[3];

    // Helper to set up one band's controls
    void setupBand(int bandIndex,
                   const char* freqId, const char* qId, const char* gainId,
                   const juce::String& bandName);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EQPanel)
};


// =============================================================================
// CLASS: BADTEditor  —  the main plugin window
// =============================================================================
//
// Hosts all the knobs, labels, VU meters, and the EQ panel overlay.
//
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

    // ===== Main modulation knobs =====
    juce::Slider timeKnob;
    juce::Slider amplitudeKnob;
    juce::Slider pitchKnob;
    juce::Label  timeLabel, amplitudeLabel, pitchLabel;

    // ===== LFO rate knob =====
    juce::Slider lfoRateKnob;
    juce::Label  lfoRateLabel;

    // ===== Pan knobs (DoubleClickKnob to intercept double-click → EQ) =====
    DoubleClickKnob dryPanKnob, wetPanKnob;
    juce::Label     dryPanLabel, wetPanLabel;

    // ===== Per-path volume faders (next to their pan knobs) =====
    juce::Slider dryVolKnob, wetVolKnob;
    juce::Label  dryVolLabel, wetVolLabel;

    // ===== Single output VU meter =====
    VUMeterComponent outputVUL;

    // ===== APVTS Slider Attachments =====
    // Each attachment binds a Slider widget to an APVTS parameter.
    // It MUST be destroyed before the Slider it's attached to.
    // Declaring them AFTER the sliders in the header ensures correct destruction order.
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        timeAttach, ampAttach, pitchAttach,
        dryVolAttach, wetVolAttach,
        dryPanAttach, wetPanAttach,
        lfoRateAttach;

    // ===== EQ panel overlays =====
    std::unique_ptr<EQPanel> dryEQPanel;
    std::unique_ptr<EQPanel> wetEQPanel;

    // ===== Solo buttons =====
    // SOLO DRY: suppress wet path so only the dry signal is heard.
    // SOLO WET: suppress dry path so only the wet (processed) signal is heard.
    // Positioned directly beneath their respective pan knobs.
    juce::ToggleButton soloDryButton, soloWetButton;

    // ===== Bypass buttons =====
    // One per modulation effect — temporarily removes that effect from the signal
    // chain without changing the knob position.  Positioned below each main knob.
    juce::ToggleButton bypassTimeButton, bypassAmpButton, bypassPitchButton;

    // ===== Sensor mode toggle + readout =====
    // The toggle switches the processor between amplitude and pitch sensor modes.
    // The readout label shows the raw sensor value (amplitude 0-1 or pitch Hz)
    // updated every ~42ms via a Timer so you can calibrate normalisation constants.
    juce::ToggleButton sensorModeButton;
    juce::Label        sensorModeLabel;   // static "SENSOR:" caption
    juce::Label        sensorValueLabel;  // live readout of rawSensorValue

    // Timer fires at 24fps to update the sensor readout label.
    // Inheriting from Timer here avoids adding a separate Timer member object.
    void timerCallback();
    // (BADTEditor inherits juce::Timer — see class declaration update below)

    void showEQPanel(bool showDry);
    void hideEQPanels();
    void setupKnob(juce::Slider& knob, juce::Label& label,
                   const juce::String& labelText, bool smallKnob = false);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BADTEditor)
};
