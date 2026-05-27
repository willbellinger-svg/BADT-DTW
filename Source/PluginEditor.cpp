// =============================================================================
// PluginEditor.cpp  —  BADT GUI
// =============================================================================
#include "PluginEditor.h"

// =============================================================================
// Layout constants
// Window: 700 × 175. Four vertical sections, left to right:
//   SEC1 TIME/AMP/PITCH  |  SEC2 LFO  |  SEC3 Wet/Dry+EQ  |  SEC4 VU
// =============================================================================
static constexpr int WINDOW_WIDTH  = 700;
static constexpr int WINDOW_HEIGHT = 175;

// Section X boundaries
static constexpr int SEC1_X = 8;    static constexpr int SEC1_W = 272;  // TIME/AMP/PITCH
static constexpr int SEC2_X = 288;  static constexpr int SEC2_W = 112;  // LFO
static constexpr int SEC3_X = 408;  static constexpr int SEC3_W = 246;  // Wet/Dry
static constexpr int SEC4_X = 658;  static constexpr int SEC4_W = 42;   // VU (bar only, scale outside)
static constexpr int VU_SCALE_X = SEC4_X + SEC4_W;  // dB scale labels sit right of the bars

// VU bar widths and channel positions
static constexpr int VU_BAR_W  = 16;
static constexpr int VU_BAR_H  = WINDOW_HEIGHT - 10;
static constexpr int VU_BAR_Y  = 6;
static constexpr int VU_L_X    = SEC4_X;
static constexpr int VU_R_X    = SEC4_X + VU_BAR_W + 4;

// Section 1 — modulation knob columns and row positions
static constexpr int KNOB_W     = 80;
static constexpr int KNOB_H     = 80;
static constexpr int COL_TIME   = SEC1_X + 8;
static constexpr int COL_AMP    = COL_TIME + KNOB_W + 6;
static constexpr int COL_PITCH  = COL_AMP  + KNOB_W + 6;
static constexpr int AP_BTN_Y   = 6;    static constexpr int AP_BTN_H  = 22;  static constexpr int AP_BTN_W  = KNOB_W;
static constexpr int KNOB_LBL_Y = AP_BTN_Y + AP_BTN_H + 2;  static constexpr int LBL_H = 13;
static constexpr int KNOB_Y     = KNOB_LBL_Y + LBL_H + 1;
static constexpr int BYP_Y      = KNOB_Y + KNOB_H + 2;
static constexpr int BYP_H      = 14;
static constexpr int BYP_W      = (KNOB_W - 2) / 2;  // BYP and INV share the row side by side
static constexpr int DBG_Y      = BYP_Y + BYP_H + 2;
static constexpr int DBG_H      = 16;

// Section 2 — LFO
static constexpr int LFO_KNOB_W = 48;
static constexpr int LFO_KNOB_H = 68;
static constexpr int LFO_LBL_Y  = 6;   static constexpr int LFO_LBL_H = 13;
static constexpr int LFO_KNOB1_Y = LFO_LBL_Y + LFO_LBL_H + 2;   // rate knob
static constexpr int LFO_KNOB2_Y = LFO_KNOB1_Y + LFO_KNOB_H + 4; // depth knob
static constexpr int LFO_X      = SEC2_X + (SEC2_W - LFO_KNOB_W) / 2;

// Section 3 — Wet/Dry
static constexpr int S3_DVOL_X  = SEC3_X + 4;
static constexpr int S3_DPAN_X  = S3_DVOL_X + 36 + 6;   // slider(36) + gap
static constexpr int S3_WPAN_X  = S3_DPAN_X + 66 + 10;
static constexpr int S3_WVOL_X  = S3_WPAN_X + 66 + 6;
static constexpr int S3_PAN_W   = 66;
static constexpr int S3_PAN_H   = 72;
static constexpr int S3_VOL_W   = 36;   // slider width
static constexpr int S3_VOL_H   = 88;   // slider height
static constexpr int S3_LBL_Y   = 6;
static constexpr int S3_CTRL_Y  = S3_LBL_Y + LBL_H + 2;
static constexpr int S3_SOLO_Y  = S3_CTRL_Y + S3_PAN_H + 4;
static constexpr int S3_SOLO_H  = 16;
static constexpr int S3_SOLO_W  = S3_PAN_W;
// EQ strip — sits beneath the solo buttons inside SEC3
static constexpr int S3_EQ_Y   = S3_SOLO_Y + S3_SOLO_H + 4;
static constexpr int S3_EQ_H   = WINDOW_HEIGHT - S3_EQ_Y - 4;

// Colours
static const juce::Colour BG_COLOUR         { 0xFFF5F6F8 };
static const juce::Colour BG_LIGHT          { 0xFFE8EAED };
static const juce::Colour TITLE_COLOUR      { 0xFF2C2C2E };
static const juce::Colour LABEL_COLOUR      { 0xFF555555 };
static const juce::Colour ACCENT_COLOUR     { 0xFFD4A017 };
static const juce::Colour DIVIDER_COLOUR    { 0xFFD0D4D9 };
static const juce::Colour SECTION_BG        { 0xFFE8EAED };


// =============================================================================
// EQSection
// =============================================================================
const char* EQSection::BAND_NAMES[3] = { "LO", "MID", "HI" };

EQSection::EQSection(BADTAudioProcessor& proc, bool isDry)
    : audioProcessor(proc), isForDry(isDry)
{
    titleLabel.setText(isDry ? "DRY EQ" : "WET EQ", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions().withHeight(11.0f).withStyle("Bold")));
    titleLabel.setColour(juce::Label::textColourId, ACCENT_COLOUR);
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel);

    if (isDry)
    {
        setupBand(0, BADTAudioProcessor::PARAM_D_EQ1_GAIN, BAND_NAMES[0]);
        setupBand(1, BADTAudioProcessor::PARAM_D_EQ2_GAIN, BAND_NAMES[1]);
        setupBand(2, BADTAudioProcessor::PARAM_D_EQ3_GAIN, BAND_NAMES[2]);
    }
    else
    {
        setupBand(0, BADTAudioProcessor::PARAM_W_EQ1_GAIN, BAND_NAMES[0]);
        setupBand(1, BADTAudioProcessor::PARAM_W_EQ2_GAIN, BAND_NAMES[1]);
        setupBand(2, BADTAudioProcessor::PARAM_W_EQ3_GAIN, BAND_NAMES[2]);
    }
}

void EQSection::setupBand(int idx, const char* gId,
                          const juce::String& name)
{
    auto& b = bands[idx];

    b.bandLabel.setText(name, juce::dontSendNotification);
    b.bandLabel.setFont(juce::Font(juce::FontOptions().withHeight(10.0f).withStyle("Bold")));
    b.bandLabel.setColour(juce::Label::textColourId, TITLE_COLOUR);
    b.bandLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(b.bandLabel);

    b.gainSlider.setSliderStyle(juce::Slider::LinearVertical);
    b.gainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    addAndMakeVisible(b.gainSlider);

    b.gainAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, gId, b.gainSlider);
}

void EQSection::paint(juce::Graphics& g)
{
    // Light metallic background
    g.fillAll(juce::Colour(0xFFE8EAED));
    
    // Border
    g.setColour(DIVIDER_COLOUR);
    g.drawRect(getLocalBounds().toFloat().reduced(0.5f), 1.0f);
}

void EQSection::resized()
{
    auto area = getLocalBounds().reduced(3, 2);
    titleLabel.setBounds(juce::Rectangle<int>());
    const int sliderW = area.getWidth() / 3;
    for (int i = 0; i < 3; ++i)
    {
        auto col = area.removeFromLeft(sliderW).reduced(1, 0);
        bands[i].bandLabel.setBounds(col.removeFromTop(10));
        bands[i].gainLabel.setBounds(juce::Rectangle<int>());
        bands[i].gainSlider.setBounds(col);
    }
}


// =============================================================================
// BADTEditor — Constructor
// =============================================================================
BADTEditor::BADTEditor(BADTAudioProcessor& p)
    : AudioProcessorEditor(&p),
      audioProcessor(p),
      outputVUL(p.outputLevelL),
      outputVUR(p.outputLevelR),
      dryEQ(p, true),
      wetEQ(p, false)
{
    setLookAndFeel(&lookAndFeel);
    setSize(WINDOW_WIDTH, WINDOW_HEIGHT);

    // ── Modulation knobs ────────────────────────────────────────────────────
    setupKnob(timeKnob,      timeLabel,      "TIME");
    setupKnob(amplitudeKnob, amplitudeLabel, "AMP");
    setupKnob(pitchKnob,     pitchLabel,     "PITCH");
    timeKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    amplitudeKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    pitchKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);

    // ── A/P source buttons ───────────────────────────────────────────────────
    setupAPButton(timeAPButton,  audioProcessor.timeUsePitch);
    setupAPButton(ampAPButton,   audioProcessor.ampUsePitch);
    setupAPButton(pitchAPButton, audioProcessor.pitchUsePitch);

    // ── Digital debug readouts ───────────────────────────────────────────────
    addAndMakeVisible(timeDisplay);
    addAndMakeVisible(ampDisplay);
    addAndMakeVisible(pitchDisplay);

    // ── Bypass buttons ───────────────────────────────────────────────────────
    setupBypassButton(bypassTimeButton,  audioProcessor.bypassTime);
    setupBypassButton(bypassAmpButton,   audioProcessor.bypassAmp);
    setupBypassButton(bypassPitchButton, audioProcessor.bypassPitch);

    // ── Invert buttons ───────────────────────────────────────────────────────
    auto setupInvert = [&](juce::ToggleButton& btn, std::atomic<bool>& flag)
    {
        btn.setButtonText("INV");
        btn.setComponentID("inv");
        btn.onStateChange = [&btn, &flag]() {
            flag.store(btn.getToggleState(), std::memory_order_relaxed);
        };
        addAndMakeVisible(btn);
    };
    setupInvert(invertTimeButton,  audioProcessor.invertTime);
    setupInvert(invertAmpButton,   audioProcessor.invertAmp);
    setupInvert(invertPitchButton, audioProcessor.invertPitch);

    // ── LFO knobs ────────────────────────────────────────────────────────────
    setupKnob(lfoRateKnob,  lfoRateLabel,  "RATE",  true);
    setupKnob(lfoDepthKnob, lfoDepthLabel, "DEPTH", true);
    lfoRateKnob.setComponentID("lfo-rate");
    lfoDepthKnob.setComponentID("lfo-depth");

    // ── Pan knobs ────────────────────────────────────────────────────────────
    setupKnob(dryPanKnob, dryPanLabel, "DRY PAN", true);
    setupKnob(wetPanKnob, wetPanLabel, "WET PAN", true);

    // ── Volume sliders (LinearVertical) ──────────────────────────────────────
    auto setupVolSlider = [&](juce::Slider& s, juce::Label& lbl, const juce::String& txt)
    {
        s.setSliderStyle(juce::Slider::LinearVertical);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, S3_VOL_W, 13);
        s.setColour(juce::Slider::textBoxTextColourId,       LABEL_COLOUR);
        s.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xFF1A1A1A));
        lbl.setText(txt, juce::dontSendNotification);
        lbl.setFont(juce::Font(juce::FontOptions().withHeight(10.0f).withStyle("Bold")));
        lbl.setColour(juce::Label::textColourId, LABEL_COLOUR);
        lbl.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(s);
        addAndMakeVisible(lbl);
    };
    setupVolSlider(dryVolSlider, dryVolLabel, "DVOL");
    setupVolSlider(wetVolSlider, wetVolLabel, "WVOL");

    // ── Solo buttons ─────────────────────────────────────────────────────────
    auto setupSolo = [&](juce::ToggleButton& btn, const juce::String& txt,
                          std::atomic<bool>& flag)
    {
        btn.setButtonText(txt);
        btn.setComponentID("solo");
        btn.onStateChange = [&btn, &flag]() {
            flag.store(btn.getToggleState(), std::memory_order_relaxed);
        };
        addAndMakeVisible(btn);
    };
    setupSolo(soloDryButton, "SOLO DRY", audioProcessor.soloDry);
    setupSolo(soloWetButton, "SOLO WET", audioProcessor.soloWet);

    // ── VU meters ────────────────────────────────────────────────────────────
    addAndMakeVisible(outputVUL);
    addAndMakeVisible(outputVUR);

    // ── Inline EQ sections ───────────────────────────────────────────────────
    addAndMakeVisible(dryEQ);
    addAndMakeVisible(wetEQ);

    // ── APVTS Attachments ────────────────────────────────────────────────────
    timeAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_TIME,      timeKnob);
    ampAttach  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_AMP,       amplitudeKnob);
    pitchAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_PITCH,     pitchKnob);
    dryVolAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_DRY_VOL,   dryVolSlider);
    wetVolAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_WET_VOL,   wetVolSlider);
    dryPanAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_DRY_PAN,   dryPanKnob);
    wetPanAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_WET_PAN,   wetPanKnob);
    lfoRateAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_LFO_RATE,  lfoRateKnob);
    lfoDepthAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, BADTAudioProcessor::PARAM_LFO_DEPTH, lfoDepthKnob);

    startTimerHz(24);
}


// =============================================================================
// Destructor
// =============================================================================
BADTEditor::~BADTEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}


// =============================================================================
// setupKnob
// =============================================================================
void BADTEditor::setupKnob(juce::Slider& knob, juce::Label& label,
                            const juce::String& labelText, bool small)
{
    knob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false,
                         small ? 50 : KNOB_W,
                         small ? 13 : 16);
    label.setText(labelText, juce::dontSendNotification);
    label.setFont(juce::Font(juce::FontOptions().withHeight(10.0f).withStyle("Bold")));
    label.setColour(juce::Label::textColourId, LABEL_COLOUR);
    label.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(knob);
    addAndMakeVisible(label);
}


// =============================================================================
// setupAPButton
// =============================================================================
void BADTEditor::setupAPButton(juce::ToggleButton& btn, std::atomic<bool>& flag)
{
    btn.setComponentID("ap");
    btn.setButtonText("");   // drawn manually in LookAndFeel
    btn.onStateChange = [&btn, &flag]() {
        flag.store(btn.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible(btn);
}


// =============================================================================
// setupBypassButton
// =============================================================================
void BADTEditor::setupBypassButton(juce::ToggleButton& btn, std::atomic<bool>& flag)
{
    btn.setButtonText("BYP");
    btn.setComponentID("byp");
    btn.onStateChange = [&btn, &flag]() {
        flag.store(btn.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible(btn);
}


// =============================================================================
// paint — background, title, section dividers
// =============================================================================
void BADTEditor::paint(juce::Graphics& g)
{
    // Subtle white brushed-metal background
    juce::ColourGradient bgGrad(juce::Colour(0xFFF5F6F8), 0.0f, 0.0f, 
                                 juce::Colour(0xFFE4E7EA), 0.0f, static_cast<float>(getHeight()), false);
    g.setGradientFill(bgGrad);
    g.fillAll();

    // Add a 3D bevel highlight across the very top edge of the plugin
    g.setColour(juce::Colours::white);
    g.drawLine(0.0f, 1.0f, static_cast<float>(getWidth()), 1.0f, 2.0f);

    // Title
    g.setColour(TITLE_COLOUR);
    g.setFont(juce::Font(juce::FontOptions().withHeight(18.0f).withStyle("Bold")));
    g.drawFittedText("BADT", 0, 0, WINDOW_WIDTH, 22,
                     juce::Justification::centred, 1);

    // Section dividers (vertical)
    g.setColour(DIVIDER_COLOUR);
    g.drawVerticalLine(SEC2_X - 4, 2.0f, (float)WINDOW_HEIGHT - 2);
    g.drawVerticalLine(SEC3_X - 4, 2.0f, (float)WINDOW_HEIGHT - 2);
    g.drawVerticalLine(SEC4_X - 4, 2.0f, (float)WINDOW_HEIGHT - 2);
}


// =============================================================================
// resized — position every child component
// =============================================================================
void BADTEditor::resized()
{
    // ── Section 1: TIME / AMP / PITCH ────────────────────────────────────────
    const int cols[] = { COL_TIME, COL_AMP, COL_PITCH };
    juce::Slider*       knobs[]   = { &timeKnob,          &amplitudeKnob,    &pitchKnob          };
    juce::Label*        lbls[]    = { &timeLabel,          &amplitudeLabel,   &pitchLabel         };
    juce::ToggleButton* apBtns[]  = { &timeAPButton,       &ampAPButton,      &pitchAPButton      };
    juce::ToggleButton* bypBtns[] = { &bypassTimeButton,   &bypassAmpButton,  &bypassPitchButton  };
    juce::ToggleButton* invBtns[] = { &invertTimeButton,   &invertAmpButton,  &invertPitchButton  };

    for (int i = 0; i < 3; ++i)
    {
        const int cx = cols[i];
        apBtns[i] ->setBounds(cx,             AP_BTN_Y,   AP_BTN_W,  AP_BTN_H);
        lbls[i]   ->setBounds(cx,             KNOB_LBL_Y, KNOB_W,    LBL_H);
        knobs[i]  ->setBounds(cx,             KNOB_Y,     KNOB_W,    KNOB_H);
        bypBtns[i]->setBounds(cx,             BYP_Y,      BYP_W,     BYP_H);
        invBtns[i]->setBounds(cx + BYP_W + 2, BYP_Y,      BYP_W,     BYP_H);
    }
    timeDisplay .setBounds(COL_TIME,  DBG_Y, KNOB_W, DBG_H);
    ampDisplay  .setBounds(COL_AMP,   DBG_Y, KNOB_W, DBG_H);
    pitchDisplay.setBounds(COL_PITCH, DBG_Y, KNOB_W, DBG_H);

    // ── Section 2: LFO ───────────────────────────────────────────────────────
    lfoRateLabel .setBounds(LFO_X, LFO_LBL_Y,   LFO_KNOB_W, LFO_LBL_H);
    lfoRateKnob  .setBounds(LFO_X, LFO_KNOB1_Y, LFO_KNOB_W, LFO_KNOB_H);
    lfoDepthLabel.setBounds(LFO_X, LFO_KNOB1_Y + LFO_KNOB_H + 2, LFO_KNOB_W, LFO_LBL_H);
    lfoDepthKnob .setBounds(LFO_X, LFO_KNOB2_Y, LFO_KNOB_W, LFO_KNOB_H);

    // ── Section 3: Wet/Dry controls ──────────────────────────────────────────
    // Labels
    dryVolLabel.setBounds(S3_DVOL_X, S3_LBL_Y, S3_VOL_W, LBL_H);
    dryPanLabel.setBounds(S3_DPAN_X, S3_LBL_Y, S3_PAN_W, LBL_H);
    wetPanLabel.setBounds(S3_WPAN_X, S3_LBL_Y, S3_PAN_W, LBL_H);
    wetVolLabel.setBounds(S3_WVOL_X, S3_LBL_Y, S3_VOL_W, LBL_H);

    // Controls
    dryVolSlider.setBounds(S3_DVOL_X, S3_CTRL_Y, S3_VOL_W, S3_VOL_H);
    dryPanKnob  .setBounds(S3_DPAN_X, S3_CTRL_Y, S3_PAN_W, S3_PAN_H);
    wetPanKnob  .setBounds(S3_WPAN_X, S3_CTRL_Y, S3_PAN_W, S3_PAN_H);
    wetVolSlider.setBounds(S3_WVOL_X, S3_CTRL_Y, S3_VOL_W, S3_VOL_H);

    // Solo buttons
    soloDryButton.setBounds(S3_DPAN_X, S3_SOLO_Y, S3_SOLO_W, S3_SOLO_H);
    soloWetButton.setBounds(S3_WPAN_X, S3_SOLO_Y, S3_SOLO_W, S3_SOLO_H);

    // ── Section 4: VU meters ─────────────────────────────────────────────────
    outputVUL.setBounds(VU_L_X, VU_BAR_Y, VU_BAR_W + 22, VU_BAR_H);  // includes scale
    outputVUR.setBounds(VU_R_X, VU_BAR_Y, VU_BAR_W + 22, VU_BAR_H);

    // ── EQ — both sections inside SEC3, beneath the solo buttons ─────────────
    const int eqHalf = (SEC3_W - 4) / 2;
    dryEQ.setBounds(SEC3_X,              S3_EQ_Y, eqHalf, S3_EQ_H);
    wetEQ.setBounds(SEC3_X + eqHalf + 4, S3_EQ_Y, eqHalf, S3_EQ_H);
}


// =============================================================================
// timerCallback — 24fps updates for debug labels
// =============================================================================
void BADTEditor::timerCallback()
{
    const float delayMs    = audioProcessor.displayDelayMs.load(std::memory_order_relaxed);
    const float ampDB      = audioProcessor.displayAmpDB.load(std::memory_order_relaxed);
    const float pitchCents = audioProcessor.displayPitchCents.load(std::memory_order_relaxed);

    timeDisplay.setText(juce::String(delayMs, 1) + " ms");

    juce::String ampStr = (ampDB >= 0.0f ? "+" : "") + juce::String(ampDB, 1) + " dB";
    ampDisplay.setText(ampStr);

    juce::String pitchStr = (pitchCents >= 0.0f ? "+" : "")
                          + juce::String(static_cast<int>(pitchCents)) + " \xc2\xa2";
    pitchDisplay.setText(pitchStr);
}
