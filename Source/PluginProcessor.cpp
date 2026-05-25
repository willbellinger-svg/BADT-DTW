// =============================================================================
// PluginProcessor.cpp  —  BADT
// =============================================================================
//
// All audio processing logic lives here.
// Pitch shifting via RubberBandLiveShifter (see PitchProcessor in PluginProcessor.h).
//
// =============================================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"

// =============================================================================
// Module-level constants
// =============================================================================

// Maximum dB range for the amplitude modulation effect (±9 dB).
static constexpr float MAX_GAIN_DB = 9.0f;

// Maximum pitch shift in cents (100 cents = 1 semitone).
static constexpr float MAX_PITCH_CENTS = 50.0f;

// Target time constant for sensor EMA smoothing, in milliseconds.
static constexpr double SENSOR_SMOOTH_MS = 25.0;

// =============================================================================
// Pitch sensor normalisation range.
//
// These constants map a raw Hz reading from analyzePitch() onto 0..1 so it can
// drive the same modulation paths as the amplitude SampleSum.
//
// HOW TO CALIBRATE:
//   1. Build and run the Standalone version.
//   2. Switch the GUI toggle to PITCH mode.
//   3. Play your audio sources for ~60 seconds — the GUI sensor readout shows
//      live Hz. Note the lowest and highest values you observe.
//   4. Report those numbers; update PITCH_NORM_MIN_HZ and PITCH_NORM_MAX_HZ.
//
// Defaults below are reasonable starting values for mixed-instrument content.
// A signal at or below MIN maps to 0.0; at or above MAX maps to 1.0.
//
// Calibrated so the typical 200–500 Hz range (voice, guitar, piano melody)
// maps to 0.20–0.50 — matching the amplitude sensor's output for the same
// material. This keeps the PITCH and AMP sensor modes feeling equally "hot":
//
//   150 Hz → 0.15  (low outlier: bass guitar open low-E ≈ 82 Hz would be 0.08)
//   200 Hz → 0.20  (lower end of typical melodic content)
//   500 Hz → 0.50  (upper end of typical melodic content)
//   700 Hz → 0.70  (high outlier: bright whistling, piccolo, etc.)
//
// If your source is unusually bass-heavy or treble-heavy, raise or lower MAX.
static constexpr float PITCH_NORM_MIN_HZ =    0.0f;
static constexpr float PITCH_NORM_MAX_HZ = 1000.0f;


// =============================================================================
// Constructor
// =============================================================================
//
// Runs once when the plugin is first instantiated by the DAW.
// The initialiser list (after the colon) sets up members that need arguments:
//   - AudioProcessor base class: declares our bus layout (stereo in/out)
//   - apvts: the parameter state tree (needs *this and the parameter list)
//
BADTAudioProcessor::BADTAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , apvts(*this, nullptr, "BADTParams", createParameterLayout())
{
}

BADTAudioProcessor::~BADTAudioProcessor() {}


// =============================================================================
// createParameterLayout()  —  define every knob/slider this plugin exposes
// =============================================================================
//
// Called once from the constructor. Returns a ParameterLayout that APVTS uses
// to create the actual parameter objects.
//
// Each AudioParameterFloat has:
//   - ParameterID{ id string, version }
//   - Display name (shown in DAW automation lanes)
//   - NormalisableRange{ min, max, step, skew }
//       skew < 1.0 = more range at the low end (good for frequency knobs)
//       skew = 1.0 = linear
//   - Default value
//
juce::AudioProcessorValueTreeState::ParameterLayout
BADTAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // ------------------------------------------------------------------
    // TIME (Ts)  —  controls delay depth
    //
    // The delay applied to the wet signal = |SampleSum × Ts| × 20ms.
    // Positive Ts: louder signal → more delay (grows with amplitude).
    // Negative Ts: louder signal → less delay (compresses with amplitude).
    // Zero:        no delay (wet signal plays in sync with dry).
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_TIME, 1 }, "Time (Ts)",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.001f, 1.0f),
        0.0f));

    // ------------------------------------------------------------------
    // AMPLITUDE  —  controls amplitude modulation depth
    //
    // Maps to ±MAX_GAIN_DB (currently ±6 dB) of gain on the wet path.
    // 0.5 = centre = 0 dB (no change).
    // The effect scales with SampleSum, so quiet signals get little change.
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_AMP, 1 }, "Amplitude",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f, 1.0f),
        0.5f));

    // ------------------------------------------------------------------
    // PITCH  —  controls pitch modulation depth
    //
    // Range: -MAX_PITCH_CENTS to +MAX_PITCH_CENTS (currently ±50 cents).
    // The actual shift = SampleSum × pitchControl, so loud passages get
    // more pitch variation than quiet ones.
    //
    // TO CHANGE THE MAXIMUM RANGE:
    //   Edit MAX_PITCH_CENTS at the top of this file.
    //   That constant controls both the slider range AND the computation.
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_PITCH, 1 }, "Pitch (cents)",
        juce::NormalisableRange<float>(-MAX_PITCH_CENTS, MAX_PITCH_CENTS, 0.1f, 1.0f),
        0.0f));

    // ------------------------------------------------------------------
    // INPUT GAIN  —  applied to the signal before any processing.
    // Range: -24 to +24 dB.  Default: 0 dB (unity gain, no change).
    // Use this to adjust the level going into the delay buffer and analysis.
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_IN_GAIN, 1 }, "Input Gain (dB)",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f, 1.0f),
        0.0f));

    // ------------------------------------------------------------------
    // OUTPUT GAIN  —  applied to the final mixed stereo output.
    // Range: -24 to +24 dB.  Default: 0 dB.
    // Use this as a master trim after all processing.
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_OUT_GAIN, 1 }, "Output Gain (dB)",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f, 1.0f),
        0.0f));

    // ------------------------------------------------------------------
    // DRY PAN  —  stereo position of the unprocessed signal.
    // -1.0 = hard left, 0.0 = centre, +1.0 = hard right.
    // Double-click in the GUI to open the Dry EQ panel.
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_DRY_PAN, 1 }, "Dry Pan",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f, 1.0f),
        0.0f));

    // ------------------------------------------------------------------
    // WET PAN  —  stereo position of the processed (delayed/pitched) signal.
    // Double-click in the GUI to open the Wet EQ panel.
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_WET_PAN, 1 }, "Wet Pan",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f, 1.0f),
        0.0f));

    // ------------------------------------------------------------------
    // DRY VOL  —  volume fader for the unprocessed dry signal.
    // Range: -24 to +6 dB.  Default: 0 dB (unity gain).
    // Placed next to the DRY PAN knob in the GUI.
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_DRY_VOL, 1 }, "Dry Volume (dB)",
        juce::NormalisableRange<float>(-24.0f, 6.0f, 0.1f, 1.0f),
        0.0f));

    // ------------------------------------------------------------------
    // WET VOL  —  volume fader for the processed (delayed/pitched) signal.
    // Range: -24 to +6 dB.  Default: -6 dB (sits under the dry by default).
    // Replaces the old hard-coded -6 dB offset — now freely adjustable.
    // Placed next to the WET PAN knob in the GUI.
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_WET_VOL, 1 }, "Wet Volume (dB)",
        juce::NormalisableRange<float>(-24.0f, 6.0f, 0.1f, 1.0f),
        -6.0f));

    // ------------------------------------------------------------------
    // LFO RATE  —  sinusoidal oscillation of the wet delay position.
    // 0 Hz = off (no LFO).  Up to 5 Hz for slow-to-fast wobble.
    // Applied at block rate so it does not introduce pitch artifacts.
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_LFO_RATE, 1 }, "LFO Rate (Hz)",
        juce::NormalisableRange<float>(0.0f, 5.0f, 0.01f, 1.0f),
        0.0f));

    // ------------------------------------------------------------------
    // DRY EQ  —  3-band EQ applied to the dry (unprocessed) signal path.
    //
    // Band 1: LOW SHELF  — boosts/cuts everything below the chosen frequency.
    //   Freq range: 20–2000 Hz (skew 0.3 = more range in the bass, less in treble)
    //   Q:     how sharp the shelf transition is (0.707 = smooth Butterworth shape)
    //   Gain:  ±12 dB
    //
    // Band 2: BELL PEAK  — boosts/cuts a narrow or wide band around a centre frequency.
    //   Freq range: 100–10000 Hz
    //   Q:     bandwidth (low Q = wide bell, high Q = narrow notch/peak)
    //   Gain:  ±12 dB
    //
    // Band 3: HIGH SHELF — boosts/cuts everything above the chosen frequency.
    //   Freq range: 1000–20000 Hz
    //   Gain:  ±12 dB
    // ------------------------------------------------------------------
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_D_EQ1_FREQ, 1 }, "Dry Low Freq",
        juce::NormalisableRange<float>(20.0f, 2000.0f, 1.0f, 0.3f), 100.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_D_EQ1_Q,    1 }, "Dry Low Q",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.5f),   0.707f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_D_EQ1_GAIN, 1 }, "Dry Low Gain",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f, 1.0f),  0.0f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_D_EQ2_FREQ, 1 }, "Dry Mid Freq",
        juce::NormalisableRange<float>(100.0f, 10000.0f, 1.0f, 0.3f), 1000.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_D_EQ2_Q,    1 }, "Dry Mid Q",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.5f),    1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_D_EQ2_GAIN, 1 }, "Dry Mid Gain",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f, 1.0f),   0.0f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_D_EQ3_FREQ, 1 }, "Dry High Freq",
        juce::NormalisableRange<float>(1000.0f, 20000.0f, 1.0f, 0.3f), 8000.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_D_EQ3_Q,    1 }, "Dry High Q",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.5f),      0.707f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_D_EQ3_GAIN, 1 }, "Dry High Gain",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f, 1.0f),     0.0f));

    // Wet EQ — same parameter structure, separate instances.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_W_EQ1_FREQ, 1 }, "Wet Low Freq",
        juce::NormalisableRange<float>(20.0f, 2000.0f, 1.0f, 0.3f), 100.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_W_EQ1_Q,    1 }, "Wet Low Q",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.5f),   0.707f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_W_EQ1_GAIN, 1 }, "Wet Low Gain",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f, 1.0f),  0.0f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_W_EQ2_FREQ, 1 }, "Wet Mid Freq",
        juce::NormalisableRange<float>(100.0f, 10000.0f, 1.0f, 0.3f), 1000.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_W_EQ2_Q,    1 }, "Wet Mid Q",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.5f),    1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_W_EQ2_GAIN, 1 }, "Wet Mid Gain",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f, 1.0f),   0.0f));

    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_W_EQ3_FREQ, 1 }, "Wet High Freq",
        juce::NormalisableRange<float>(1000.0f, 20000.0f, 1.0f, 0.3f), 8000.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_W_EQ3_Q,    1 }, "Wet High Q",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f, 0.5f),      0.707f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_W_EQ3_GAIN, 1 }, "Wet High Gain",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f, 1.0f),     0.0f));

    return layout;
}


// =============================================================================
// isBusesLayoutSupported()
// =============================================================================
//
// DAW calls this to check whether a particular channel configuration is OK.
// We accept mono or stereo input, and always output stereo.
//
bool BADTAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    auto in = layouts.getMainInputChannelSet();
    return (in == juce::AudioChannelSet::mono() ||
            in == juce::AudioChannelSet::stereo());
}


// =============================================================================
// prepareToPlay()  —  initialise everything that depends on sample rate / block size
// =============================================================================
//
// Called by the DAW before playback starts, and again if settings change.
// This is the place to allocate buffers and configure DSP objects —
// NEVER do these things in processBlock() (allocation on audio thread = glitches).
//
void BADTAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    // Analysis window and max delay: both 20ms.
    // The YIN pitch detector and amplitude sensor both look at the last 20ms of audio.
    // The max delay matches so a fully-driven Time knob can shift the wet signal by up
    // to 20ms relative to dry — clearly audible as a pre/post echo on transients.
    analysisSamples = static_cast<int>(sampleRate * 0.020);
    maxDelaySamples = analysisSamples;

    // Buffer is 30ms: 20ms for the delay read + 10ms headroom so the interpolation
    // (which reads one sample ahead) never goes out of bounds.
    int bufferSamples = static_cast<int>(sampleRate * 0.030) + 2;

    // --- Circular buffer ---
    delayBuffer.prepare(bufferSamples);

    // --- Signal analyser ---
    analyzer.prepare(sampleRate, analysisSamples);

    // --- Pitch shifter ---
    pitchProcessor.prepare(sampleRate, samplesPerBlock);
    pitchProcessor.reset();

    // --- Dual-head crossfader ---
    // Crossfade length: ~10ms worth of samples. Short enough to be inaudible
    // as a blend, long enough to completely mask the click from a delay snap.
    xfadeLengthSamples = static_cast<int>(sampleRate * 0.010);
    delayHeadA       = 0.0f;
    delayHeadB       = 0.0f;
    xfadeSamplesLeft = 0;
    lfoPhase         = 0.0f;

    // --- EQ filter setup ---
    // juce::dsp::ProcessSpec describes the audio format our filters will process.
    // numChannels=1 because we run EQ on mono signals (dry and wet before panning).
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    spec.numChannels      = 1;

    dryEqBand1.prepare(spec); dryEqBand2.prepare(spec); dryEqBand3.prepare(spec);
    wetEqBand1.prepare(spec); wetEqBand2.prepare(spec); wetEqBand3.prepare(spec);

    // Brickwall limiter on the wet mono channel.
    // Threshold -0.1 dBFS ≈ 0.989 linear — essentially a hard ceiling.
    // Release 50ms: gain recovers over 50ms after a transient, avoiding pumping.
    wetLimiter.prepare(spec);
    wetLimiter.setThreshold(-0.1f);
    wetLimiter.setRelease(50.0f);

    // Reset filter state (clears any leftover samples from a previous session).
    dryEqBand1.reset(); dryEqBand2.reset(); dryEqBand3.reset();
    wetEqBand1.reset(); wetEqBand2.reset(); wetEqBand3.reset();

    // Force coefficient update on first block by invalidating the cache.
    // Setting all to NaN ensures the "has this changed?" check in updateEQIfNeeded()
    // always triggers on the first block.
    for (auto& v : prevDryEqParams) v = std::numeric_limits<float>::quiet_NaN();
    for (auto& v : prevWetEqParams) v = std::numeric_limits<float>::quiet_NaN();

    // --- Working buffers ---
    dryWorkBuffer.assign(static_cast<size_t>(samplesPerBlock), 0.0f);
    wetWorkBuffer.assign(static_cast<size_t>(samplesPerBlock), 0.0f);

    sampleSum = 0.0f;

    // Compute EMA alpha for the chosen smoothing time constant.
    // EMA advances once per block, so the time constant is expressed in blocks:
    //   tau_samples = (SENSOR_SMOOTH_MS / 1000) * sampleRate
    //   alpha = 1 - exp(-samplesPerBlock / tau_samples)
    // Larger block size → larger alpha (faster tracking per update), which is
    // appropriate because each block already covers a longer slice of time.
    const double tauSamples = (SENSOR_SMOOTH_MS / 1000.0) * sampleRate;
    sensorSmoothAlpha = static_cast<float>(
        1.0 - std::exp(-static_cast<double>(samplesPerBlock) / tauSamples));

    // Reset smoothed values so there's no stale carry-over from a previous session.
    smoothedAmplitude = 0.0f;
    smoothedPitchHz   = 0.0f;
}



// =============================================================================
// releaseResources()
// =============================================================================
//
// Called when the DAW stops playback. Safe to clear state here.
// std::vector and member objects clean themselves up automatically in their
// destructors — nothing extra to do for our buffers.
//
void BADTAudioProcessor::releaseResources()
{
    pitchProcessor.reset();
}


// =============================================================================
// updateEQIfNeeded()  —  rebuild filter coefficients when parameters change
// =============================================================================
//
// Called once per processBlock() for dry and wet paths separately.
// Checks whether any EQ knob has moved since the last block.
// If nothing changed, we skip the coefficient rebuild (saves CPU).
//
// HOW BIQUAD COEFFICIENTS WORK:
//   A biquad filter is defined by 5 numbers (a0, a1, a2, b0, b1, b2).
//   These numbers determine the filter's frequency response shape.
//   juce::dsp::IIR::Coefficients provides factory functions that calculate
//   the correct numbers for common filter types (shelf, peak, etc.).
//
// WHY DECIBELS TO GAIN:
//   The IIR factory functions want gain as a LINEAR multiplier (1.0 = no change,
//   2.0 = double amplitude = +6 dBFS, 0.5 = half amplitude = -6 dBFS).
//   Our knobs give dB values (-12 to +12), so we convert:
//   gainLinear = 10^(gainDB / 20)
//
void BADTAudioProcessor::updateEQIfNeeded(bool isDry)
{
    // Pick the right set of parameter IDs and filter objects based on path.
    const char* freqIds[3] = { isDry ? PARAM_D_EQ1_FREQ : PARAM_W_EQ1_FREQ,
                                isDry ? PARAM_D_EQ2_FREQ : PARAM_W_EQ2_FREQ,
                                isDry ? PARAM_D_EQ3_FREQ : PARAM_W_EQ3_FREQ };
    const char* qIds[3]    = { isDry ? PARAM_D_EQ1_Q    : PARAM_W_EQ1_Q,
                                isDry ? PARAM_D_EQ2_Q    : PARAM_W_EQ2_Q,
                                isDry ? PARAM_D_EQ3_Q    : PARAM_W_EQ3_Q };
    const char* gainIds[3] = { isDry ? PARAM_D_EQ1_GAIN : PARAM_W_EQ1_GAIN,
                                isDry ? PARAM_D_EQ2_GAIN : PARAM_W_EQ2_GAIN,
                                isDry ? PARAM_D_EQ3_GAIN : PARAM_W_EQ3_GAIN };

    float* prevParams = isDry ? prevDryEqParams : prevWetEqParams;

    // Read current knob values.
    float freq[3], q[3], gain[3];
    for (int b = 0; b < 3; ++b)
    {
        freq[b] = *apvts.getRawParameterValue(freqIds[b]);
        q[b]    = *apvts.getRawParameterValue(qIds[b]);
        gain[b] = *apvts.getRawParameterValue(gainIds[b]);
    }

    // Check whether anything changed. If not, bail out early.
    bool changed = false;
    for (int b = 0; b < 3; ++b)
    {
        if (freq[b] != prevParams[b*3+0] ||
            q[b]    != prevParams[b*3+1] ||
            gain[b] != prevParams[b*3+2])
        {
            changed = true;
            break;
        }
    }
    if (!changed) return;

    // Save new values to cache.
    for (int b = 0; b < 3; ++b)
    {
        prevParams[b*3+0] = freq[b];
        prevParams[b*3+1] = q[b];
        prevParams[b*3+2] = gain[b];
    }

    double sr = currentSampleRate;

    // Helper: convert dB to linear gain. juce::Decibels::decibelsToGain does 10^(dB/20).
    auto toLinear = [](float dB) { return juce::Decibels::decibelsToGain(dB); };

    // Band 1: Low Shelf
    // makeLowShelf(sampleRate, cutoffFreq, Q, gainLinear)
    // Shapes everything below cutoffFreq by gainLinear.
    auto b1coeff = juce::dsp::IIR::Coefficients<float>::makeLowShelf(sr, freq[0], q[0], toLinear(gain[0]));

    // Band 2: Bell/Peak filter
    // makePeakFilter(sampleRate, centreFreq, Q, gainLinear)
    // Boosts/cuts a bell-shaped region around centreFreq.
    // High Q = narrow bell; low Q = wide, gentle boost/cut.
    auto b2coeff = juce::dsp::IIR::Coefficients<float>::makePeakFilter(sr, freq[1], q[1], toLinear(gain[1]));

    // Band 3: High Shelf
    // makeHighShelf(sampleRate, cutoffFreq, Q, gainLinear)
    // Shapes everything above cutoffFreq by gainLinear.
    auto b3coeff = juce::dsp::IIR::Coefficients<float>::makeHighShelf(sr, freq[2], q[2], toLinear(gain[2]));

    // Assign new coefficients to the correct filter objects.
    // The filter's 'coefficients' member is a ReferenceCountedObjectPtr.
    // Assigning to it atomically swaps in the new coefficients on the audio thread.
    if (isDry)
    {
        dryEqBand1.coefficients = b1coeff;
        dryEqBand2.coefficients = b2coeff;
        dryEqBand3.coefficients = b3coeff;
    }
    else
    {
        wetEqBand1.coefficients = b1coeff;
        wetEqBand2.coefficients = b2coeff;
        wetEqBand3.coefficients = b3coeff;
    }
}


// =============================================================================
// processBlock()  —  THE AUDIO PROCESSING HEART OF THE PLUGIN
// =============================================================================
//
// Called by the DAW for every chunk of audio (typically 64–2048 samples).
// MUST complete faster than the duration of the audio block, every time.
//
// RULES ON THE AUDIO THREAD (violating these causes glitches and dropouts):
//   ✗ No memory allocation  (new, delete, vector::push_back, etc.)
//   ✗ No locks or mutexes
//   ✗ No file or network I/O
//   ✗ No OS calls that can block
//   ✓ Read atomics (thread-safe, lock-free)
//   ✓ Use pre-allocated buffers
//   ✓ Arithmetic — floating-point operations are always safe
//
void BADTAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& /*midiMessages*/)
{
    // Tells the CPU to flush tiny "denormal" floats to zero automatically.
    // Denormals appear in reverb/delay tails and can make the CPU 100× slower
    // processing them in software. This one line prevents that issue entirely.
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    // Guard: some hosts deliver a block larger than the samplesPerBlock reported
    // to prepareToPlay() without calling prepareToPlay() again. Resize on the
    // audio thread only in this rare case to avoid an out-of-bounds crash.
    if (numSamples > static_cast<int>(dryWorkBuffer.size()))
    {
        dryWorkBuffer.resize(static_cast<size_t>(numSamples), 0.0f);
        wetWorkBuffer.resize(static_cast<size_t>(numSamples), 0.0f);
    }

    // -----------------------------------------------------------------------
    // Read all parameter values for this block.
    //
    // apvts.getRawParameterValue(id) returns a pointer (std::atomic<float>*).
    // We dereference it immediately with * to get the float value.
    // Reading atomics is lock-free and safe on the audio thread.
    // -----------------------------------------------------------------------
    const float ts         = *apvts.getRawParameterValue(PARAM_TIME);
    const float ampControl = *apvts.getRawParameterValue(PARAM_AMP);
    const float pitchCtrl  = *apvts.getRawParameterValue(PARAM_PITCH);
    const float dryPan     = *apvts.getRawParameterValue(PARAM_DRY_PAN);
    const float wetPan     = *apvts.getRawParameterValue(PARAM_WET_PAN);

    // Per-path volume from the DRY VOL / WET VOL knobs (dB → linear).
    // dryVolLinear scales the dry signal in the final mix.
    // wetVolLinear replaces the old hard-coded -6 dB offset on the wet path.
    const float dryVolLinear = juce::Decibels::decibelsToGain(
        *apvts.getRawParameterValue(PARAM_DRY_VOL));
    const float wetVolLinear = juce::Decibels::decibelsToGain(
        *apvts.getRawParameterValue(PARAM_WET_VOL));

    // -----------------------------------------------------------------------
    // Update EQ filter coefficients if knobs have changed.
    // This is safe on the audio thread — it only does arithmetic, no allocation.
    // -----------------------------------------------------------------------
    updateEQIfNeeded(true);   // Dry path EQ
    updateEQIfNeeded(false);  // Wet path EQ

    // -----------------------------------------------------------------------
    // STEP 1: Analyse the PREVIOUS block's audio to compute SampleSum.
    //
    // We use last block's buffer state because:
    //   - We haven't written the current block yet
    //   - One block of analysis latency (~10ms) is inaudible
    // SampleSum is our "how loud was the signal recently?" value (0.0–1.0).
    // It drives the depth of all three effects (time, amplitude, pitch).
    // -----------------------------------------------------------------------
    // SENSOR MODE: choose amplitude or pitch as the modulation source.
    // usePitchSensor is an atomic<bool> written by the GUI toggle button.
    if (usePitchSensor.load(std::memory_order_relaxed))
    {
        // Raw Hz from autocorrelation — can jump erratically between blocks due to
        // octave errors, harmonic confusion, and snapping to 0 on silence.
        const float rawHz = analyzer.analyzePitch(delayBuffer);

        // Hold the last smoothed value when the detector finds no pitch (silence/noise).
        // This prevents the display from snapping to zero between notes.
        // Only advance the EMA when we have a real pitch reading (rawHz > 0).
        if (rawHz > 0.0f)
            smoothedPitchHz = sensorSmoothAlpha * rawHz
                            + (1.0f - sensorSmoothAlpha) * smoothedPitchHz;
        else
            // No frequency detected (silence / below energy threshold): pull the
            // smoothed value toward zero at the same rate as the EMA above.
            // This prevents stale non-zero readings persisting on silence.
            smoothedPitchHz *= (1.0f - sensorSmoothAlpha);

        // GUI readout shows smoothed Hz — much more stable than raw autocorrelation.
        rawSensorValue.store(smoothedPitchHz, std::memory_order_relaxed);

        // Drive modulation from the smoothed, normalised value.
        sampleSum = normalizePitch(smoothedPitchHz);

        // DBG: shows both raw and smoothed Hz — useful for calibration.
        DBG("BADT pitch: raw=" + juce::String(rawHz, 1) + " Hz  smoothed=" +
            juce::String(smoothedPitchHz, 1) + " Hz  norm=" + juce::String(sampleSum, 3));
    }
    else
    {
        // AMPLITUDE MODE (default): mean absolute amplitude of the last 20ms.
        const float rawAmp = analyzer.computeSampleSum(delayBuffer);

        // EMA adds temporal continuity between blocks — reduces jumpiness.
        smoothedAmplitude = sensorSmoothAlpha * rawAmp
                          + (1.0f - sensorSmoothAlpha) * smoothedAmplitude;

        // Map amplitude to 0-1 using the dB range −40..0 dBFS so modulation varies
        // noticeably across real dynamic range (≈ 0.35 quiet to 0.85 loud).
        const float ampDB = 20.0f * std::log10(std::max(smoothedAmplitude, 1e-6f));
        sampleSum = juce::jlimit(0.0f, 1.0f, (ampDB + 40.0f) / 40.0f);
        rawSensorValue.store(sampleSum, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // STEP 2: Derive per-block modulation values.
    //
    // These are computed once per block (not per sample) for efficiency.
    // The analysis window is 20ms, so block-rate updates are indistinguishable
    // from sample-rate updates to human ears.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Amplitude-driven delay — dual-head crossfade (no pitch shift).
    //
    // We compute the target delay once per block and SNAP the read head to it
    // immediately. If the position changed, we start a crossfade: head A reads
    // the new position, head B reads the old position. Both are constant during
    // the crossfade, so neither causes a pitch change. The blend just hides the
    // click from the instantaneous position jump.
    //
    // If a new change arrives before the previous crossfade completes, we
    // compute the current in-progress blend value and use that as the new B,
    // so the transition is always smooth regardless of how fast amplitude moves.
    // -----------------------------------------------------------------------
    const float ampDelayTarget = bypassTime.load(std::memory_order_relaxed)
                                 ? 0.0f
                                 : computeTargetDelaySamples(ts);

    if (std::abs(ampDelayTarget - delayHeadA) > 0.5f)
    {
        // If mid-crossfade, compute the current blend position as the new B
        // so we don't snap the outgoing fade to a stale position.
        if (xfadeSamplesLeft > 0)
        {
            const float t = static_cast<float>(xfadeSamplesLeft)
                          / static_cast<float>(xfadeLengthSamples);
            delayHeadB = delayHeadA * (1.0f - t) + delayHeadB * t;
        }
        else
        {
            delayHeadB = delayHeadA;
        }

        delayHeadA       = ampDelayTarget;
        xfadeSamplesLeft = xfadeLengthSamples;
    }

    // LFO: read rate constant once per block, evaluated per sample below.
    // The LFO adds a sinusoidal offset to the read position — this intentionally
    // creates vibrato (mild pitch modulation) which is the chorusing character of
    // the effect. Unlike the Time knob, the LFO's job IS to wobble pitch slightly.
    const float lfoRate      = *apvts.getRawParameterValue(PARAM_LFO_RATE);
    const float lfoHalfDepth = static_cast<float>(maxDelaySamples) * 0.5f;

    // bypassAmp: skip amplitude modulation — use unity gain (1.0 = no change).
    const float amplitudeGain = bypassAmp.load(std::memory_order_relaxed)
                                ? 1.0f
                                : computeAmplitudeGain(ampControl);

    // bypassPitch: skip pitch shifting — set 0 cents (no shift applied).
    const float pitchCents = bypassPitch.load(std::memory_order_relaxed)
                             ? 0.0f
                             : (sampleSum * pitchCtrl);
    pitchProcessor.setPitchCents(pitchCents);

    // Pan gains for dry and wet paths (constant-power panning).
    float dryL, dryR, wetL, wetR;
    computePanGains(dryPan, dryL, dryR);
    computePanGains(wetPan, wetL, wetR);

    // -----------------------------------------------------------------------
    // STEP 3: Per-sample loop.
    //
    // For each sample in this block:
    //   a) Sum stereo input to mono
    //   b) Write to circular delay buffer
    //   c) Pull DRY sample (no delay) into dryWorkBuffer
    //   d) Pull WET sample (delay position modulated per sample) into wetWorkBuffer
    // -----------------------------------------------------------------------
    for (int i = 0; i < numSamples; ++i)
    {
        // (a) Sum to mono. If input is stereo, average L and R.
        //     This halves the amplitude (+3 dB per channel normally handled by convention).
        float monoSample = sumToMono(buffer, i);

        // (b) Write the mono sample into the circular buffer.
        //     This feeds both the delay read (wet path) and the analysis window.
        delayBuffer.write(monoSample);

        // (c) Dry path: take the current sample directly (no delay).
        dryWorkBuffer[static_cast<size_t>(i)] = monoSample;

        // (f) Wet path: dual-head crossfader + LFO.

        // LFO: advance phase one sample and compute sinusoidal offset.
        if (lfoRate > 0.0f)
        {
            lfoPhase += lfoRate / static_cast<float>(currentSampleRate);
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
        }
        const float lfoOffset = (lfoRate > 0.0f)
            ? std::sin(juce::MathConstants<float>::twoPi * lfoPhase) * lfoHalfDepth
            : 0.0f;

        // Read from head A (current target) and head B (outgoing position).
        // The LFO offset is added to both so the vibrato effect is preserved
        // consistently across the crossfade.
        const float maxPos = static_cast<float>(maxDelaySamples - 1);
        const float posA   = juce::jlimit(0.0f, maxPos, delayHeadA + lfoOffset);
        const float sampleA = delayBuffer.readDelayedInterpolated(posA);

        float wetSample;
        if (xfadeSamplesLeft > 0)
        {
            // Crossfade in progress: blend A (fading in) and B (fading out).
            // t counts from 1.0 down to 0.0 as the crossfade runs.
            // At t=1.0: all B (old position). At t=0.0: all A (new position).
            const float posB    = juce::jlimit(0.0f, maxPos, delayHeadB + lfoOffset);
            const float sampleB = delayBuffer.readDelayedInterpolated(posB);
            const float t       = static_cast<float>(xfadeSamplesLeft)
                                / static_cast<float>(xfadeLengthSamples);
            wetSample = sampleA * (1.0f - t) + sampleB * t;
            --xfadeSamplesLeft;
        }
        else
        {
            wetSample = sampleA;
        }

        wetWorkBuffer[static_cast<size_t>(i)] = wetSample;
    }


    // -----------------------------------------------------------------------
    // STEP 4: Apply dry path EQ.
    //
    // processSample() runs the biquad filter equation on one sample.
    // It updates the filter's internal memory (the "I" in IIR = Infinite Impulse
    // Response: the filter remembers past samples to compute the current output).
    // We chain Band 1 → Band 2 → Band 3, each feeding the next.
    // -----------------------------------------------------------------------
    for (int i = 0; i < numSamples; ++i)
    {
        float s = dryWorkBuffer[static_cast<size_t>(i)];
        s = dryEqBand1.processSample(s);  // Low shelf
        s = dryEqBand2.processSample(s);  // Bell peak
        s = dryEqBand3.processSample(s);  // High shelf
        dryWorkBuffer[static_cast<size_t>(i)] = s;
    }

    // -----------------------------------------------------------------------
    // STEP 5: Apply wet path amplitude gain.
    //
    // This scales the wet signal up or down based on the current signal loudness
    // and the amplitude knob position. Loud signal → bigger change; quiet → smaller.
    // -----------------------------------------------------------------------
    for (int i = 0; i < numSamples; ++i)
        wetWorkBuffer[static_cast<size_t>(i)] *= amplitudeGain;

    // -----------------------------------------------------------------------
    // STEP 6: Apply pitch shifting to the wet buffer.
    //
    // RubberBand processes the whole block at once.
    // setPitchCents() was called above with the current shift amount.
    // -----------------------------------------------------------------------
    pitchProcessor.processBlock(wetWorkBuffer.data(), numSamples);

    // -----------------------------------------------------------------------
    // STEP 7: Apply wet path EQ.
    // Same biquad chain as dry, but with the wet path filter objects.
    // -----------------------------------------------------------------------
    for (int i = 0; i < numSamples; ++i)
    {
        float s = wetWorkBuffer[static_cast<size_t>(i)];
        s = wetEqBand1.processSample(s);
        s = wetEqBand2.processSample(s);
        s = wetEqBand3.processSample(s);
        wetWorkBuffer[static_cast<size_t>(i)] = s;
    }

    // -----------------------------------------------------------------------
    // STEP 7b: Brickwall limiter on wet channel.
    //
    // Runs after wet EQ. The limiter is always active — it's a safety net for
    // extreme amplitude or pitch settings that could otherwise clip the output.
    //
    // AudioBlock is a non-owning view over existing memory — no allocation.
    // We pass float** (a pointer to our mono buffer pointer) as required by the API.
    {
        float* wetPtr = wetWorkBuffer.data();
        juce::dsp::AudioBlock<float> wetBlock(&wetPtr, 1, static_cast<size_t>(numSamples));
        wetLimiter.process(juce::dsp::ProcessContextReplacing<float>(wetBlock));
    }

    // -----------------------------------------------------------------------
    // STEP 8: Pan, mix, apply output gain, and write stereo output.
    //
    // For each sample:
    //   Left  channel = (dry × dryL + wet × wetL) × outputGain
    //   Right channel = (dry × dryR + wet × wetR) × outputGain
    //
    // The constant-power pan gains (dryL, dryR, wetL, wetR) were computed above.
    // -----------------------------------------------------------------------
    // Read solo flags once per block (atomic, lock-free).
    // SOLO DRY = suppress wet path.  SOLO WET = suppress dry path.
    // If both are active simultaneously, both paths play (they cancel out).
    const bool isSoloDry = soloDry.load(std::memory_order_relaxed);
    const bool isSoloWet = soloWet.load(std::memory_order_relaxed);
    // A path is muted when the OTHER solo is on exclusively.
    const float dryMix = (isSoloWet && !isSoloDry) ? 0.0f : 1.0f;
    const float wetMix = (isSoloDry && !isSoloWet) ? 0.0f : 1.0f;

    float blockOutputPeak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float dry = dryWorkBuffer[static_cast<size_t>(i)];
        const float wet = wetWorkBuffer[static_cast<size_t>(i)];

        // dryMix / wetMix are 0 when the other path is soloed, 1 otherwise.
        // dryVolLinear / wetVolLinear come from the DRY VOL and WET VOL knobs.
        const float outLeft  = dry * dryMix * dryL * dryVolLinear + wet * wetMix * wetL * wetVolLinear;
        const float outRight = dry * dryMix * dryR * dryVolLinear + wet * wetMix * wetR * wetVolLinear;

        buffer.setSample(0, i, outLeft);
        buffer.setSample(1, i, outRight);

        // Track the highest peak across both channels for the single output VU meter.
        const float peak = std::max(std::abs(outLeft), std::abs(outRight));
        if (peak > blockOutputPeak) blockOutputPeak = peak;
    }

    outputLevelL.store(blockOutputPeak, std::memory_order_relaxed);
}


// =============================================================================
// sumToMono()  —  collapse all input channels to a single float
// =============================================================================
//
// Why mono-sum before processing?
//   Delay, analysis, and pitch shifting are all mono operations.
//   Doing them once on a mono mix uses half the CPU of processing stereo.
//   We recreate stereo at the output via the pan system.
//
float BADTAudioProcessor::sumToMono(const juce::AudioBuffer<float>& buf,
                                        int sampleIndex) const
{
    const int numCh = buf.getNumChannels();
    if (numCh == 0) return 0.0f;
    if (numCh == 1) return buf.getSample(0, sampleIndex);

    // Average L and R to keep level consistent with the mono source.
    // (L + R) / 2 prevents doubling when both channels are identical and in phase.
    float sum = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
        sum += buf.getSample(ch, sampleIndex);
    return sum / static_cast<float>(numCh);
}


// =============================================================================
// computeTargetDelaySamples()  —  calculate the delay read position in samples
// =============================================================================
//
// FORMULA:
//   n = SampleSum × Ts            (dimensionless, range ≈ -1 to +1)
//   delaySamples = |n| × maxDelaySamples
//
// WHY ABS VALUE?
//   A negative result would mean "read from the future" — physically impossible.
//   Instead, the SIGN of Ts controls the direction of modulation:
//     Positive Ts: louder signal → MORE delay (grows with amplitude)
//     Negative Ts: louder signal → LESS delay (shrinks with amplitude)
//   Both flavours produce interesting wobble; the sign is an artistic choice.
//
// The returned value is the TARGET for smoothedDelaySamples.
// The actual read position ramps toward this target over DELAY_SMOOTH_TIME_SECS.
//
float BADTAudioProcessor::computeTargetDelaySamples(float ts) const
{
    const float n        = sampleSum * ts;
    const float fraction = std::abs(n);                                     // 0.0–1.0
    const float samples  = fraction * static_cast<float>(maxDelaySamples);  // 0–882 samples
    return juce::jlimit(0.0f, static_cast<float>(maxDelaySamples - 1), samples);
}


// =============================================================================
// computeAmplitudeGain()  —  convert SampleSum + knob position to linear gain
// =============================================================================
//
// DESIGN:
//   The effect should be signal-driven: loud passages get more amplitude change,
//   quiet passages get less. This gives the "breathing compressor" character.
//
// FORMULA (step by step):
//
//   Step 1 — SampleSum (0–1 linear) → dB:
//     sampleSumDB = 20 × log10(max(SampleSum, 0.001))
//     Ranges from -60 dB (near-silence) to 0 dB (full amplitude).
//
//   Step 2 — Normalise [-60, 0] dB → [0, 1]:
//     normalised = (sampleSumDB + 60) / 60
//     0 = very quiet signal, 1 = loud signal
//
//   Step 3 — Knob (0–1) → target gain in dB:
//     targetDB = (ampControl - 0.5) × 2 × MAX_GAIN_DB
//     ampControl 0.5 → 0 dB (unity, no change)
//     ampControl 1.0 → +MAX_GAIN_DB (currently +6 dB boost)
//     ampControl 0.0 → -MAX_GAIN_DB (currently -6 dB cut)
//
//   Step 4 — Scale by signal loudness:
//     effectiveDB = targetDB × normalised
//     Quiet signal → effectiveDB ≈ 0 (almost no change)
//     Loud signal  → effectiveDB ≈ targetDB (full effect)
//
//   Step 5 — dB → linear gain:
//     gain = 10^(effectiveDB / 20)
//
float BADTAudioProcessor::computeAmplitudeGain(float ampControl) const
{
    // sampleSum is already dB-normalized 0-1; multiply directly for signal-driven scaling.
    const float targetDB    = (ampControl - 0.5f) * 2.0f * MAX_GAIN_DB;
    const float effectiveDB = targetDB * sampleSum;
    return std::pow(10.0f, effectiveDB / 20.0f);
}


// =============================================================================
// computePanGains()  —  constant-power stereo panning
// =============================================================================
//
// THEORY:
//   Simple linear panning (L=1-p, R=p) makes centre-panned signals 3 dB quieter
//   because total power L² + R² dips at centre.
//
//   Constant-power panning uses sine/cosine so that L² + R² = 1 always:
//     leftGain  = (√2/2) × (cosθ + sinθ)   — equivalent to cos(θ − π/4)
//     rightGain = (√2/2) × (cosθ − sinθ)   — equivalent to cos(θ + π/4)
//
//   where θ maps pan position (−1 to +1) to angle (0 to π/2).
//   The √2/2 factor (≈ 0.707) normalises so L² + R² = 1 at all pan positions.
//
//   This is the formula you specified:
//     Aamp = (√2/2)(cosθ + sinθ)
//     Bamp = (√2/2)(cosθ − sinθ)
//
void BADTAudioProcessor::computePanGains(float panPosition,
                                             float& leftGain,
                                             float& rightGain) const
{
    panPosition = juce::jlimit(-1.0f, 1.0f, panPosition);

    // Map pan (−1 to +1) → angle θ (0 to π/2).
    const float halfPi = juce::MathConstants<float>::halfPi;
    const float theta  = (panPosition + 1.0f) * 0.5f * halfPi;

    // Standard constant-power pan law: L = cos(θ), R = sin(θ).
    // At centre (θ=π/4): L = R = √2/2 ≈ 0.707  ✓
    // Hard left (θ=0):   L = 1,  R = 0          ✓
    // Hard right (θ=π/2): L = 0, R = 1          ✓
    leftGain  = std::cos(theta);
    rightGain = std::sin(theta);
}


// =============================================================================
// normalizePitch()  —  map raw Hz to 0..1 using calibration range
// =============================================================================
//
// Linear mapping: 0 at PITCH_NORM_MIN_HZ, 1 at PITCH_NORM_MAX_HZ.
// Values outside the range are clamped. A 0 Hz input (silence) returns 0.
//
float BADTAudioProcessor::normalizePitch(float hz) const
{
    if (hz <= 0.0f) return 0.0f;
    const float range = PITCH_NORM_MAX_HZ - PITCH_NORM_MIN_HZ;
    return juce::jlimit(0.0f, 1.0f, (hz - PITCH_NORM_MIN_HZ) / range);
}


// =============================================================================
// State save / load
// =============================================================================

void BADTAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void BADTAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}


// =============================================================================
// createEditor()  +  plugin factory function
// =============================================================================

juce::AudioProcessorEditor* BADTAudioProcessor::createEditor()
{
    return new BADTEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BADTAudioProcessor();
}
