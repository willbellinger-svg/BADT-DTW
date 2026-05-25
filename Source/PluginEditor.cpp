// =============================================================================
// PluginEditor.cpp  —  BADTVibe GUI Implementation
// =============================================================================
//
// This file implements every GUI class declared in PluginEditor.h.
// If the .h file is the blueprint ("what exists"), this file is the build
// manual ("how each thing works and looks").
// =============================================================================

#include "PluginEditor.h"

// =============================================================================
// LAYOUT CONSTANTS
// =============================================================================
//
// All pixel sizes and positions are gathered here so tweaking the look means
// changing one number rather than hunting through the whole file.
//
// JUCE coordinate origin: (0,0) = TOP-LEFT of window.
// X increases rightward; Y increases DOWNWARD.
//
static constexpr int WINDOW_WIDTH  = 560;
static constexpr int WINDOW_HEIGHT = 310;

// ── Main knob row (Row 1) ─────────────────────────────────────────────────────
static constexpr int ROW1_LABEL_Y  = 37;   // Top edge of the labels above knobs
static constexpr int ROW1_KNOB_Y   = 55;   // Top edge of the knobs themselves
static constexpr int LABEL_H       = 16;   // Height of any label strip

// TIME / AMPLITUDE / PITCH — the three large modulation knobs
static constexpr int MAIN_KNOB_W   = 90;
static constexpr int MAIN_KNOB_H   = 90;  // Total height: ~70px dial + 20px text box

// DRY VOL / WET VOL — smaller volume faders; also used for LFO rate knob
static constexpr int GAIN_KNOB_W   = 52;
static constexpr int GAIN_KNOB_H   = 80;  // Match pan knob height so the row is even

// Single output VU meter — one bar showing max(|L|, |R|) peak.
// Positioned on the right side of the main knob row.
// VU_H spans the full label+knob area height (ROW1_KNOB_Y−ROW1_LABEL_Y + MAIN_KNOB_H = 108).
static constexpr int VU_W          = 16;
static constexpr int VU_H          = (ROW1_KNOB_Y - ROW1_LABEL_Y) + MAIN_KNOB_H; // 108 px
static constexpr int X_VU_OUT      = WINDOW_WIDTH - VU_W - 8;                     // 536

// ── Pan knob row (Row 2) ──────────────────────────────────────────────────────
static constexpr int ROW2_LABEL_Y  = 178;
static constexpr int ROW2_KNOB_Y   = 196;
static constexpr int PAN_KNOB_W    = 70;
static constexpr int PAN_KNOB_H    = 80;

// ── Solo buttons ──────────────────────────────────────────────────────────────
// Placed directly beneath the DRY PAN and WET PAN knobs.
static constexpr int SOLO_Y        = ROW2_KNOB_Y + PAN_KNOB_H + 2;  // 278
static constexpr int SOLO_H        = 18;
static constexpr int SOLO_W        = PAN_KNOB_W;

// ── Bypass buttons ────────────────────────────────────────────────────────────
// Placed in the 30px gap between the main knob row and the pan row.
static constexpr int BYP_Y         = ROW1_KNOB_Y + MAIN_KNOB_H + 3;  // 148
static constexpr int BYP_H         = 18;
static constexpr int BYP_W         = MAIN_KNOB_W;

// ── Horizontal X positions ────────────────────────────────────────────────────
//
// Main row: three modulation knobs centred in the window.
//   Total knob width: 3 × 90 = 270 px
//   Left margin to first knob: (560 − 270) / 2 = 145 px
//
static constexpr int X_TIME     = 145;                     // First main knob
static constexpr int X_AMP      = X_TIME  + MAIN_KNOB_W;  // 235
static constexpr int X_PITCH    = X_AMP   + MAIN_KNOB_W;  // 325

// Pan row: pan knobs centred under their modulation knobs
static constexpr int X_DRY_PAN  = X_TIME  + (MAIN_KNOB_W - PAN_KNOB_W)  / 2;  // 155
static constexpr int X_WET_PAN  = X_PITCH + (MAIN_KNOB_W - PAN_KNOB_W)  / 2;  // 335

// Volume knobs: placed outside the pan knobs (dry vol on left, wet vol on right)
static constexpr int X_DRY_VOL  = X_DRY_PAN - GAIN_KNOB_W - 8;   //  95
static constexpr int X_WET_VOL  = X_WET_PAN + PAN_KNOB_W  + 8;   // 413

// LFO rate knob: centred under the AMP knob in the pan row
static constexpr int X_LFO_RATE = X_AMP + (MAIN_KNOB_W - GAIN_KNOB_W) / 2;   // 254

// ── Colour palette ────────────────────────────────────────────────────────────
// 32-bit ARGB: 0xAARRGGBB  (AA=alpha FF=opaque, RR/GG/BB=colour components)
static const juce::Colour BG_COLOUR         { 0xFF1A1A1A };  // Near-black background
static const juce::Colour TITLE_COLOUR      { 0xFFE0E0E0 };  // Light grey title text
static const juce::Colour LABEL_COLOUR      { 0xFFB0B0B0 };  // Medium grey labels
static const juce::Colour KNOB_FILL_COLOUR  { 0xFF2A6496 };  // Blue-grey arc fill
static const juce::Colour KNOB_TRACK_COLOUR { 0xFF444444 };  // Dark ring track
static const juce::Colour KNOB_THUMB_COLOUR { 0xFFD4A017 };  // Amber pointer
static const juce::Colour EQ_BG_COLOUR      { 0xFF141414 };  // EQ panel (slightly darker)
static const juce::Colour EQ_BORDER_COLOUR  { 0xFFD4A017 };  // Amber EQ border


// =============================================================================
// EQPanel — static member definition
// =============================================================================
//
// BAND_NAMES is declared "static const char*" in the .h — meaning it belongs
// to the class itself (one shared copy), not to any particular instance.
// Static members must be *defined* (given their actual values) in the .cpp.
//
const char* EQPanel::BAND_NAMES[3] = { "Low Shelf", "Bell Peak", "High Shelf" };


// =============================================================================
// EQPanel — Constructor
// =============================================================================
//
// Builds one EQ panel (for either the dry or wet signal path).
// Contains a title label, a close button, and 3 EQ bands.
//
// proc    — audio processor (gives access to apvts for parameter binding)
// isDry   — true = Dry EQ panel, false = Wet EQ panel
// onClose — called when the user clicks [X]
//
EQPanel::EQPanel(BADTAudioProcessor& proc, bool isDry,
                 std::function<void()> onClose)
    : audioProcessor(proc),
      isForDry(isDry),
      closeCallback(onClose)
{
    // ── Title label ───────────────────────────────────────────────────────────
    titleLabel.setText(isForDry ? "DRY EQ" : "WET EQ",
                       juce::dontSendNotification);
    // juce::dontSendNotification: suppresses change-listener notifications
    // (nothing is listening yet at construction time, so this is safe/correct).
    titleLabel.setFont(juce::Font(juce::FontOptions().withHeight(15.0f).withStyle("Bold")));
    titleLabel.setColour(juce::Label::textColourId, TITLE_COLOUR);
    titleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel);

    // ── Close button ──────────────────────────────────────────────────────────
    closeButton.setButtonText("X");
    closeButton.setColour(juce::TextButton::buttonColourId,   juce::Colour(0xFF333333));
    closeButton.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xFFCC4444));
    // juce::Button::onClick is a std::function<void()> that JUCE fires on click.
    // We forward it to the callback supplied by the editor.
    closeButton.onClick = closeCallback;
    addAndMakeVisible(closeButton);

    // ── EQ bands ──────────────────────────────────────────────────────────────
    // setupBand() wires one band column (Freq + Q + Gain) to its APVTS parameters.
    // isDry selects between "D_EQ*" (dry) and "W_EQ*" (wet) parameter IDs.
    if (isForDry)
    {
        setupBand(0,
                  BADTAudioProcessor::PARAM_D_EQ1_FREQ,
                  BADTAudioProcessor::PARAM_D_EQ1_Q,
                  BADTAudioProcessor::PARAM_D_EQ1_GAIN,
                  BAND_NAMES[0]);   // "Low Shelf"

        setupBand(1,
                  BADTAudioProcessor::PARAM_D_EQ2_FREQ,
                  BADTAudioProcessor::PARAM_D_EQ2_Q,
                  BADTAudioProcessor::PARAM_D_EQ2_GAIN,
                  BAND_NAMES[1]);   // "Bell Peak"

        setupBand(2,
                  BADTAudioProcessor::PARAM_D_EQ3_FREQ,
                  BADTAudioProcessor::PARAM_D_EQ3_Q,
                  BADTAudioProcessor::PARAM_D_EQ3_GAIN,
                  BAND_NAMES[2]);   // "High Shelf"
    }
    else
    {
        setupBand(0,
                  BADTAudioProcessor::PARAM_W_EQ1_FREQ,
                  BADTAudioProcessor::PARAM_W_EQ1_Q,
                  BADTAudioProcessor::PARAM_W_EQ1_GAIN,
                  BAND_NAMES[0]);

        setupBand(1,
                  BADTAudioProcessor::PARAM_W_EQ2_FREQ,
                  BADTAudioProcessor::PARAM_W_EQ2_Q,
                  BADTAudioProcessor::PARAM_W_EQ2_GAIN,
                  BAND_NAMES[1]);

        setupBand(2,
                  BADTAudioProcessor::PARAM_W_EQ3_FREQ,
                  BADTAudioProcessor::PARAM_W_EQ3_Q,
                  BADTAudioProcessor::PARAM_W_EQ3_GAIN,
                  BAND_NAMES[2]);
    }
}


// =============================================================================
// EQPanel::setupBand() — configure one band column
// =============================================================================
//
// Sets up the three sliders (Freq, Q, Gain) for a single EQ band and binds
// them to their APVTS parameters so knob moves update the audio processor.
//
// bandIndex — 0/1/2, selects which EQBandRow in bands[] to populate
// freqId    — APVTS parameter ID string for the frequency slider
// qId       — APVTS parameter ID string for the Q slider
// gainId    — APVTS parameter ID string for the gain slider
// bandName  — header text ("Low Shelf", "Bell Peak", or "High Shelf")
//
void EQPanel::setupBand(int bandIndex,
                        const char* freqId, const char* qId, const char* gainId,
                        const juce::String& bandName)
{
    auto& band = bands[bandIndex];  // Reference alias — avoids writing bands[i] everywhere

    // ── Band header label ─────────────────────────────────────────────────────
    band.bandLabel.setText(bandName, juce::dontSendNotification);
    band.bandLabel.setFont(juce::Font(juce::FontOptions().withHeight(12.0f).withStyle("Bold")));
    band.bandLabel.setColour(juce::Label::textColourId, TITLE_COLOUR);
    band.bandLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(band.bandLabel);

    // ── Helper lambda: apply consistent styling to one EQ knob + its label ───
    //
    // A lambda is an anonymous function written inline. We write it once here
    // and call it three times (once per knob) instead of repeating the same
    // ~15 lines three times.
    //
    // [this] — the lambda can access 'this' (the EQPanel), needed for addAndMakeVisible.
    // auto& knob, auto& lbl — sliders/labels passed by reference so changes are real.
    //
    auto configKnob = [this](juce::Slider& knob, juce::Label& lbl,
                              const juce::String& lblText)
    {
        knob.setSliderStyle(juce::Slider::RotaryVerticalDrag);

        // Text box: 60px wide, 14px tall (compact for the smaller EQ panel knobs).
        knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 14);

        // Colour scheme matches the main plugin knobs for visual consistency.
        knob.setColour(juce::Slider::rotarySliderFillColourId,    KNOB_FILL_COLOUR);
        knob.setColour(juce::Slider::rotarySliderOutlineColourId,  KNOB_TRACK_COLOUR);
        knob.setColour(juce::Slider::thumbColourId,                KNOB_THUMB_COLOUR);
        knob.setColour(juce::Slider::textBoxTextColourId,          LABEL_COLOUR);
        knob.setColour(juce::Slider::textBoxOutlineColourId,       juce::Colours::transparentBlack);

        lbl.setText(lblText, juce::dontSendNotification);
        lbl.setFont(juce::Font(juce::FontOptions().withHeight(11.0f)));
        lbl.setColour(juce::Label::textColourId, LABEL_COLOUR);
        lbl.setJustificationType(juce::Justification::centred);

        addAndMakeVisible(knob);
        addAndMakeVisible(lbl);
    };

    configKnob(band.freqKnob, band.freqLabel, "FREQ");
    configKnob(band.qKnob,    band.qLabel,    "Q");
    configKnob(band.gainKnob, band.gainLabel, "GAIN");

    // ── SliderAttachments ─────────────────────────────────────────────────────
    //
    // SliderAttachment is the bridge between a Slider widget and an APVTS parameter.
    // Once created, moving the slider updates the parameter value, DAW automation
    // moves the slider, and saved presets restore the slider position automatically.
    //
    // Arguments: (apvts reference, parameter ID string, slider to bind)
    //
    band.freqAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, freqId, band.freqKnob);

    band.qAttach    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, qId,    band.qKnob);

    band.gainAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, gainId, band.gainKnob);
}


// =============================================================================
// EQPanel::paint() — background and decorative border
// =============================================================================
void EQPanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    // Slightly darker than the main plugin background — the EQ reads as a
    // distinct layer sitting on top of the main UI.
    g.fillAll(EQ_BG_COLOUR);

    // Amber rounded-rectangle border.
    // reduced(1.0f) insets the rect by 1px so the stroke stays within bounds.
    g.setColour(EQ_BORDER_COLOUR.withAlpha(0.8f));
    g.drawRoundedRectangle(bounds.reduced(1.0f), 3.0f, 1.5f);

    // Thin divider line below the title/close row
    g.setColour(EQ_BORDER_COLOUR.withAlpha(0.35f));
    g.drawLine(8.0f, 30.0f, bounds.getWidth() - 8.0f, 30.0f, 1.0f);
}


// =============================================================================
// EQPanel::resized() — 3-column band layout
// =============================================================================
//
// Divides the panel into three equal columns (one per EQ band).
// Within each column the three knobs (Freq, Q, Gain) sit side by side.
//
//   ┌─── DRY EQ ──────────────────────────────────────────[X]─┐
//   │   Low Shelf       │    Bell Peak      │   High Shelf      │
//   │ FREQ  Q  GAIN │ FREQ  Q  GAIN │ FREQ  Q  GAIN │
//   └──────────────────────────────────────────────────────────┘
//
void EQPanel::resized()
{
    auto area = getLocalBounds();

    // ── Title / close-button row (top 30 px) ──────────────────────────────────
    auto titleRow = area.removeFromTop(30);
    // removeFromRight() takes a 28-px strip on the right; withSizeKeepingCentre
    // keeps the button centred (28×22) within that strip.
    closeButton.setBounds(titleRow.removeFromRight(28).withSizeKeepingCentre(28, 22));
    titleLabel .setBounds(titleRow);  // Remaining strip holds the title text

    // ── Three band columns ────────────────────────────────────────────────────
    const int colWidth    = area.getWidth() / 3;
    const int bandHeaderH = 20;  // "Low Shelf" / "Bell Peak" / "High Shelf" header
    const int subLabelH   = 16;  // "FREQ" / "Q" / "GAIN" sub-label strip
    const int eqKnobH     = 110; // Each EQ knob (96px dial + 14px text box)

    for (int i = 0; i < 3; ++i)
    {
        // removeFromLeft() returns a colWidth-wide slice and shrinks 'area'.
        // reduced(4, 8) adds 4px horizontal and 8px vertical padding inside.
        auto col   = area.removeFromLeft(colWidth).reduced(4, 8);
        auto& band = bands[i];

        // Band header spans the full column width
        band.bandLabel.setBounds(col.removeFromTop(bandHeaderH));

        // Sub-labels row: divide remaining column width into 3 equal sub-columns.
        // We compute subW once, before removeFromLeft() modifies 'col'.
        const int subW = col.getWidth() / 3;

        auto subLblRow = col.removeFromTop(subLabelH);
        band.freqLabel.setBounds(subLblRow.removeFromLeft(subW));
        band.qLabel   .setBounds(subLblRow.removeFromLeft(subW));
        band.gainLabel.setBounds(subLblRow);  // Remainder goes to gainLabel

        // Knob row: same 3-way horizontal split
        auto knobRow = col.removeFromTop(eqKnobH);
        band.freqKnob.setBounds(knobRow.removeFromLeft(subW));
        band.qKnob   .setBounds(knobRow.removeFromLeft(subW));
        band.gainKnob.setBounds(knobRow);
    }
}


// =============================================================================
// BADTEditor — Constructor
// =============================================================================
//
// Runs exactly once when the plugin window opens. We must:
//   1. Call the base-class constructor (AudioProcessorEditor).
//   2. Initialise VU meter members — they have no default constructor; the
//      atomic<float>& reference they need must be passed in the initialiser list.
//   3. Set up all visible controls.
//   4. Create SliderAttachments (after the sliders exist, before resized() runs).
//   5. Create EQ panels as hidden child components.
//
BADTEditor::BADTEditor(BADTAudioProcessor& p)
    : AudioProcessorEditor(&p),   // JUCE base class: pass processor pointer
      audioProcessor(p),          // Store our own reference to the processor
      // VUMeterComponent has no default constructor — the atomic<float>& must be
      // passed here in the initialiser list.  We only have one VU now.
      outputVUL (p.outputLevelL)
{
    setSize(WINDOW_WIDTH, WINDOW_HEIGHT);

    // ── Main modulation knobs ─────────────────────────────────────────────────
    setupKnob(timeKnob,      timeLabel,      "TIME");
    setupKnob(amplitudeKnob, amplitudeLabel, "AMPLITUDE");
    setupKnob(pitchKnob,     pitchLabel,     "PITCH");

    // Tooltips appear when the user hovers the mouse over a knob for a moment.
    timeKnob     .setTooltip("Time modulation depth (Ts). "
                              "Controls how much the signal level shifts the delay time. "
                              "Range: -1 to +1 (up to 200 ms delay).");
    amplitudeKnob.setTooltip("Amplitude modulation depth. "
                              "Controls how much the signal level affects gain. "
                              "Max: +/-6 dB.");
    pitchKnob    .setTooltip("Pitch modulation depth. "
                              "Controls how much the signal level shifts the pitch. "
                              "Max: +/-50 cents.");

    // ── Volume faders ─────────────────────────────────────────────────────────
    // smallKnob=true: uses compact text box dimensions.
    // These sit next to their respective pan knobs in Row 2.
    setupKnob(dryVolKnob, dryVolLabel, "DRY VOL", true);
    setupKnob(wetVolKnob, wetVolLabel, "WET VOL", true);

    dryVolKnob.setTooltip("Dry signal volume. Range: -24 to +6 dB.");
    wetVolKnob.setTooltip("Wet signal volume. Range: -24 to +6 dB. Default -6 dB sits the effect under the dry.");

    // ── Pan knobs ─────────────────────────────────────────────────────────────
    //
    // dryPanKnob and wetPanKnob are DoubleClickKnob — a subclass of juce::Slider.
    // setupKnob() accepts juce::Slider& (the base type), so it works on them fine.
    // We just need to pass them as their actual type to set the onDoubleClick callback.
    //
    setupKnob(dryPanKnob, dryPanLabel, "DRY PAN", true);
    setupKnob(wetPanKnob, wetPanLabel, "WET PAN", true);

    dryPanKnob.setTooltip("Dry signal pan position. Double-click to open the Dry EQ panel.");
    wetPanKnob.setTooltip("Wet signal pan position. Double-click to open the Wet EQ panel.");

    // ── Solo buttons ─────────────────────────────────────────────────────────
    // SOLO DRY: suppress wet path — hear only the unprocessed dry signal.
    // SOLO WET: suppress dry path — hear only the processed wet signal.
    // Positioned beneath the DRY PAN and WET PAN knobs respectively.
    auto setupSolo = [&](juce::ToggleButton& btn, const juce::String& text,
                          std::atomic<bool>& flag)
    {
        btn.setButtonText(text);
        btn.setColour(juce::ToggleButton::textColourId, LABEL_COLOUR);
        btn.setColour(juce::ToggleButton::tickColourId, KNOB_THUMB_COLOUR);
        btn.setColour(juce::ToggleButton::tickDisabledColourId,
                      juce::Colour(0xFF444444));
        btn.onStateChange = [&btn, &flag]() {
            flag.store(btn.getToggleState(), std::memory_order_relaxed);
        };
        addAndMakeVisible(btn);
    };
    setupSolo(soloDryButton, "SOLO DRY", audioProcessor.soloDry);
    setupSolo(soloWetButton, "SOLO WET", audioProcessor.soloWet);

    // ── Bypass buttons ────────────────────────────────────────────────────────
    // One button per modulation effect. When toggled on, the processor skips that
    // effect for the wet signal — useful for A/B comparisons during sound design.
    // Red tick colour signals "active = disabled", matching the destructive intent.
    auto setupBypass = [&](juce::ToggleButton& btn, std::atomic<bool>& flag)
    {
        btn.setButtonText("BYP");
        btn.setColour(juce::ToggleButton::textColourId,         LABEL_COLOUR);
        btn.setColour(juce::ToggleButton::tickColourId,         juce::Colour(0xFFCC4444)); // red = bypassed
        btn.setColour(juce::ToggleButton::tickDisabledColourId, juce::Colour(0xFF444444));
        btn.onStateChange = [&btn, &flag]() {
            flag.store(btn.getToggleState(), std::memory_order_relaxed);
        };
        addAndMakeVisible(btn);
    };
    setupBypass(bypassTimeButton,  audioProcessor.bypassTime);
    setupBypass(bypassAmpButton,   audioProcessor.bypassAmp);
    setupBypass(bypassPitchButton, audioProcessor.bypassPitch);

    // ── LFO rate knob ─────────────────────────────────────────────────────────
    setupKnob(lfoRateKnob, lfoRateLabel, "LFO RATE", true);
    lfoRateKnob.setTooltip("LFO rate (0-5 Hz). Oscillates the wet delay position at block "
                           "rate — adds slow-to-fast wobble without pitch artifacts.");

    // ── Sensor mode toggle ────────────────────────────────────────────────────
    // Off = amplitude mode (default).  On = pitch mode.
    // The button text updates to show the active mode.
    sensorModeLabel.setText("SENSOR", juce::dontSendNotification);
    sensorModeLabel.setFont(juce::Font(juce::FontOptions().withHeight(10.0f)));
    sensorModeLabel.setColour(juce::Label::textColourId, LABEL_COLOUR);
    sensorModeLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(sensorModeLabel);

    sensorModeButton.setButtonText("AMP");
    sensorModeButton.setColour(juce::ToggleButton::textColourId,   TITLE_COLOUR);
    sensorModeButton.setColour(juce::ToggleButton::tickColourId,   KNOB_THUMB_COLOUR);
    sensorModeButton.setColour(juce::ToggleButton::tickDisabledColourId,
                               juce::Colour(0xFF444444));
    sensorModeButton.onStateChange = [this]()
    {
        const bool pitchActive = sensorModeButton.getToggleState();
        audioProcessor.usePitchSensor.store(pitchActive, std::memory_order_relaxed);
        // Update button label to reflect current mode.
        sensorModeButton.setButtonText(pitchActive ? "PITCH" : "AMP");
    };
    addAndMakeVisible(sensorModeButton);

    // ── Live sensor value readout ─────────────────────────────────────────────
    // Updated every frame by timerCallback(). Shows amplitude (0.000–1.000)
    // or pitch (Hz) depending on mode — use this during calibration.
    sensorValueLabel.setText("0.000", juce::dontSendNotification);
    sensorValueLabel.setFont(juce::Font(juce::FontOptions().withHeight(10.0f)));
    sensorValueLabel.setColour(juce::Label::textColourId,
                               juce::Colour(0xFF88CCFF)); // light blue = data
    sensorValueLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(sensorValueLabel);

    // Start the 24fps timer that updates the sensor readout.
    startTimerHz(24);

    // Wire up the double-click callbacks.
    //
    // [this] is a "capture": the lambda function can access 'this' (the editor object).
    // The lambda runs later (when the user double-clicks), so 'this' must still exist —
    // it will, because the editor outlives its child components.
    //
    dryPanKnob.onDoubleClick = [this]() { showEQPanel(true);  };
    wetPanKnob.onDoubleClick = [this]() { showEQPanel(false); };

    // ── Single output VU meter ────────────────────────────────────────────────
    // Constructed in the initialiser list. Just register it as a child component.
    addAndMakeVisible(outputVUL);

    // ── SliderAttachments for the main controls ───────────────────────────────
    //
    // A SliderAttachment syncs a Slider widget to an APVTS parameter.
    //   • Moving the slider → updates the parameter (audio thread reads it).
    //   • DAW automation   → moves the slider automatically.
    //   • Preset load      → restores the slider to the saved position.
    //
    // Rules:
    //   • Create AFTER the slider exists (it does — it's a class member).
    //   • Destroy BEFORE the slider — the unique_ptr is declared after the
    //     slider in PluginEditor.h, so C++ destroys it first automatically.
    //
    timeAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_TIME,     timeKnob);

    ampAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_AMP,      amplitudeKnob);

    pitchAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_PITCH,    pitchKnob);

    dryVolAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_DRY_VOL, dryVolKnob);

    wetVolAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_WET_VOL, wetVolKnob);

    dryPanAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_DRY_PAN,  dryPanKnob);

    wetPanAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_WET_PAN,  wetPanKnob);

    lfoRateAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_LFO_RATE, lfoRateKnob);

    // ── EQ floating windows ───────────────────────────────────────────────────
    //
    // The EQ panels are NOT added as child components.  Instead, showEQPanel()
    // calls addToDesktop() the first time they are opened, which makes each one
    // a real operating-system window that floats in front of the plugin.
    //
    // Until the user double-clicks a pan knob the panels exist in memory but
    // are invisible and have no OS window peer.
    //
    dryEQPanel = std::make_unique<EQPanel>(
        audioProcessor,
        true,                         // isDry = true
        [this]() { hideEQPanels(); }  // onClose: called when user clicks [X]
    );

    wetEQPanel = std::make_unique<EQPanel>(
        audioProcessor,
        false,                        // isDry = false
        [this]() { hideEQPanels(); }
    );
}


// =============================================================================
// BADTEditor — Destructor
// =============================================================================
//
// All unique_ptr members (SliderAttachments, EQ panels) are automatically
// destroyed in reverse declaration order when the editor is destroyed.
// We don't need to do anything here explicitly.
//
BADTEditor::~BADTEditor()
{
    stopTimer();

    // Remove EQ panels from the desktop before the unique_ptrs destroy them.
    // JUCE's Component destructor also does this, but being explicit avoids any
    // ordering issue where the close callback fires during destruction.
    if (dryEQPanel && dryEQPanel->isOnDesktop()) dryEQPanel->removeFromDesktop();
    if (wetEQPanel && wetEQPanel->isOnDesktop()) wetEQPanel->removeFromDesktop();
}


// =============================================================================
// setupKnob() — apply consistent styling to any knob + its label
// =============================================================================
//
// Centralising the styling here means: if you want to change the knob colour
// scheme, you change it in ONE place and every knob updates.
//
// knob      — the Slider to configure (passed by reference so changes stick)
// label     — the matching Label above the knob
// labelText — text to display ("TIME", "AMPLITUDE", etc.)
// smallKnob — true = compact text box (for gain and pan knobs)
//
void BADTEditor::setupKnob(juce::Slider& knob, juce::Label& label,
                                const juce::String& labelText, bool smallKnob)
{
    // ── Rotary drag style ─────────────────────────────────────────────────────
    // RotaryVerticalDrag: drag UP to increase value, drag DOWN to decrease.
    knob.setSliderStyle(juce::Slider::RotaryVerticalDrag);

    // ── Text box ──────────────────────────────────────────────────────────────
    // TextBoxBelow: a small numeric readout sits beneath the dial.
    // isReadOnly=false: clicking the text box lets the user type an exact value.
    // Width/height vary between large and small knobs.
    knob.setTextBoxStyle(juce::Slider::TextBoxBelow,
                         false,
                         smallKnob ? GAIN_KNOB_W : MAIN_KNOB_W,
                         smallKnob ? 14          : 20);

    // ── Colour IDs ────────────────────────────────────────────────────────────
    // JUCE sliders expose named "ColourId" constants for each paintable part.
    // rotarySliderFillColourId    → the arc from min-value to current value
    // rotarySliderOutlineColourId → the background track arc (full range)
    // thumbColourId               → the pointer line indicating current value
    // textBoxTextColourId         → the numeric value text
    // textBoxOutlineColourId      → border around the text box (transparent = hidden)
    knob.setColour(juce::Slider::rotarySliderFillColourId,    KNOB_FILL_COLOUR);
    knob.setColour(juce::Slider::rotarySliderOutlineColourId,  KNOB_TRACK_COLOUR);
    knob.setColour(juce::Slider::thumbColourId,                KNOB_THUMB_COLOUR);
    knob.setColour(juce::Slider::textBoxTextColourId,          LABEL_COLOUR);
    knob.setColour(juce::Slider::textBoxOutlineColourId,       juce::Colours::transparentBlack);

    // ── Label ─────────────────────────────────────────────────────────────────
    label.setText(labelText, juce::dontSendNotification);
    label.setFont(juce::Font(juce::FontOptions().withHeight(12.0f).withStyle("Bold")));
    label.setColour(juce::Label::textColourId, LABEL_COLOUR);
    label.setJustificationType(juce::Justification::centred);

    // ── Register with the window ──────────────────────────────────────────────
    // addAndMakeVisible(): makes this component a child of the editor and shows it.
    addAndMakeVisible(knob);
    addAndMakeVisible(label);
}


// =============================================================================
// paint() — static background, title, separator line
// =============================================================================
//
// JUCE calls paint() every time the component needs redrawing (window shown,
// exposed, or when repaint() is called).
// Everything drawn here sits BEHIND all child components (knobs, labels, etc.).
//
void BADTEditor::paint(juce::Graphics& g)
{
    // fillAll() fills every pixel of this component with the given colour.
    g.fillAll(BG_COLOUR);

    // ── Title text ────────────────────────────────────────────────────────────
    g.setColour(TITLE_COLOUR);
    g.setFont(juce::Font(juce::FontOptions().withHeight(20.0f).withStyle("Bold")));
    // drawFittedText() clips the text to the rectangle and shrinks the font if needed.
    // Arguments: (text, x, y, width, height, justification, maxLines)
    g.drawFittedText("BADT",
                     0, 5, WINDOW_WIDTH, 25,
                     juce::Justification::centred,
                     1);

    // ── Amber separator line below title ─────────────────────────────────────
    g.setColour(KNOB_THUMB_COLOUR.withAlpha(0.5f));
    g.drawLine(20.0f, 33.0f, static_cast<float>(WINDOW_WIDTH - 20), 33.0f, 1.0f);

    // ── Dim separator between solo-button strip and pan row ───────────────────
    g.setColour(juce::Colour(0xFF333333));
    g.drawLine(0.0f, 172.0f, static_cast<float>(WINDOW_WIDTH), 172.0f, 1.0f);
}


// =============================================================================
// resized() — position every child component within the window
// =============================================================================
//
// JUCE calls this once after construction and again whenever the window is
// resized. setBounds(x, y, w, h) positions and sizes each component.
// All layout decisions belong here, not in the constructor.
//
void BADTEditor::resized()
{
    // ── Single output VU meter (right side of main row) ───────────────────────
    outputVUL.setBounds(X_VU_OUT, ROW1_LABEL_Y, VU_W, VU_H);

    // ── Main modulation knobs ─────────────────────────────────────────────────
    timeLabel     .setBounds(X_TIME,  ROW1_LABEL_Y, MAIN_KNOB_W, LABEL_H);
    amplitudeLabel.setBounds(X_AMP,   ROW1_LABEL_Y, MAIN_KNOB_W, LABEL_H);
    pitchLabel    .setBounds(X_PITCH, ROW1_LABEL_Y, MAIN_KNOB_W, LABEL_H);

    timeKnob     .setBounds(X_TIME,  ROW1_KNOB_Y, MAIN_KNOB_W, MAIN_KNOB_H);
    amplitudeKnob.setBounds(X_AMP,   ROW1_KNOB_Y, MAIN_KNOB_W, MAIN_KNOB_H);
    pitchKnob    .setBounds(X_PITCH, ROW1_KNOB_Y, MAIN_KNOB_W, MAIN_KNOB_H);

    // ── Bypass buttons — in the gap between main knobs and pan row ───────────
    bypassTimeButton .setBounds(X_TIME,  BYP_Y, BYP_W, BYP_H);
    bypassAmpButton  .setBounds(X_AMP,   BYP_Y, BYP_W, BYP_H);
    bypassPitchButton.setBounds(X_PITCH, BYP_Y, BYP_W, BYP_H);

    // ── Volume faders — outside their respective pan knobs ───────────────────
    dryVolLabel.setBounds(X_DRY_VOL, ROW2_LABEL_Y, GAIN_KNOB_W, LABEL_H);
    wetVolLabel.setBounds(X_WET_VOL, ROW2_LABEL_Y, GAIN_KNOB_W, LABEL_H);
    dryVolKnob .setBounds(X_DRY_VOL, ROW2_KNOB_Y,  GAIN_KNOB_W, GAIN_KNOB_H);
    wetVolKnob .setBounds(X_WET_VOL, ROW2_KNOB_Y,  GAIN_KNOB_W, GAIN_KNOB_H);

    // ── Pan knobs ─────────────────────────────────────────────────────────────
    // DRY PAN centred under TIME knob; WET PAN centred under PITCH knob.
    dryPanLabel.setBounds(X_DRY_PAN, ROW2_LABEL_Y, PAN_KNOB_W, LABEL_H);
    wetPanLabel.setBounds(X_WET_PAN, ROW2_LABEL_Y, PAN_KNOB_W, LABEL_H);
    dryPanKnob .setBounds(X_DRY_PAN, ROW2_KNOB_Y,  PAN_KNOB_W, PAN_KNOB_H);
    wetPanKnob .setBounds(X_WET_PAN, ROW2_KNOB_Y,  PAN_KNOB_W, PAN_KNOB_H);

    // ── LFO rate knob — centred under AMP in the pan row ─────────────────────
    lfoRateLabel.setBounds(X_LFO_RATE, ROW2_LABEL_Y, GAIN_KNOB_W, LABEL_H);
    lfoRateKnob .setBounds(X_LFO_RATE, ROW2_KNOB_Y,  GAIN_KNOB_W, GAIN_KNOB_H);

    // ── Solo buttons — beneath their respective pan knobs ────────────────────
    soloDryButton.setBounds(X_DRY_PAN, SOLO_Y, SOLO_W, SOLO_H);
    soloWetButton.setBounds(X_WET_PAN, SOLO_Y, SOLO_W, SOLO_H);

    // ── Sensor mode toggle + readout ─────────────────────────────────────────
    // Positioned in the title bar area, right-aligned.
    sensorModeLabel .setBounds(340, 8, 58, 16);
    sensorModeButton.setBounds(402, 6, 48, 20);
    sensorValueLabel.setBounds(454, 8, 96, 16);

    // EQ panels are floating OS windows — they are NOT child components, so no
    // setBounds() call here.  They are sized and positioned in showEQPanel().
}


// =============================================================================
// timerCallback() — 24fps update for the sensor readout label
// =============================================================================
//
// Reads the processor's rawSensorValue atomic and formats it as a string.
// In amplitude mode it shows a 0.000–1.000 value.
// In pitch mode it shows Hz (e.g. "440.0 Hz") — use this for calibration.
//
void BADTEditor::timerCallback()
{
    const float raw      = audioProcessor.rawSensorValue.load(std::memory_order_relaxed);
    const bool  isPitch  = audioProcessor.usePitchSensor.load(std::memory_order_relaxed);

    juce::String display;
    if (isPitch)
        display = juce::String(raw, 1) + " Hz";
    else
        display = juce::String(raw, 3);

    sensorValueLabel.setText(display, juce::dontSendNotification);
}


// =============================================================================
// showEQPanel() — open one EQ panel as a floating OS window
// =============================================================================
//
// Called by dryPanKnob.onDoubleClick (showDry=true)
// and wetPanKnob.onDoubleClick (showDry=false).
//
// The first time a panel is shown we call addToDesktop(), which asks the OS
// to create a real native window for it.  addToDesktop() is a no-op on
// subsequent calls (JUCE skips it when a peer already exists), so we can
// safely call it every time.
//
// The window is positioned directly below the plugin window.  The EQ panel
// is NOT a child component of the editor — it is its own OS-level window.
//
void BADTEditor::showEQPanel(bool showDry)
{
    EQPanel* toShow = showDry ? dryEQPanel.get() : wetEQPanel.get();
    EQPanel* toHide = showDry ? wetEQPanel.get() : dryEQPanel.get();

    if (!toShow) return;

    // Hide the other panel if it is currently open.
    if (toHide) toHide->setVisible(false);

    if (!toShow->isOnDesktop())
    {
        // windowIsTemporary: the OS does not show the window in the taskbar.
        // windowHasDropShadow: adds a shadow (ignored on some hosts/platforms).
        //
        // Passing getPeer()->getNativeHandle() makes this window a child of
        // the plugin window at the OS level — it follows the plugin if the DAW
        // moves its host window and stays on top of it.
        void* nativeParent = (getPeer() != nullptr)
                             ? getPeer()->getNativeHandle()
                             : nullptr;

        toShow->addToDesktop(
            juce::ComponentPeer::windowIsTemporary
            | juce::ComponentPeer::windowHasDropShadow,
            nativeParent
        );

        // Size the EQ window: same width as the plugin, ~200px tall.
        toShow->setSize(WINDOW_WIDTH, 200);

        // Place it directly below the plugin window.
        // getScreenBounds() gives this component's rectangle in screen pixels.
        auto pluginScreen = getScreenBounds();
        toShow->setTopLeftPosition(pluginScreen.getX(), pluginScreen.getBottom());
    }

    toShow->setVisible(true);
    toShow->toFront(false);
}


// =============================================================================
// hideEQPanels() — close both EQ panels
// =============================================================================
//
// Called by each EQPanel's [X] close button via the onClose callback.
// Setting both to invisible restores the main knob view.
//
void BADTEditor::hideEQPanels()
{
    if (dryEQPanel) dryEQPanel->setVisible(false);
    if (wetEQPanel) wetEQPanel->setVisible(false);
}
