// =============================================================================
// PluginProcessor.cpp  —  BADT
// =============================================================================
#include "PluginProcessor.h"
#include "PluginEditor.h"

// =============================================================================
// Constants  —  edit here to retune the plugin's audio behaviour
// =============================================================================

static constexpr float  MAX_GAIN_DB      = 9.0f;    // ±9 dB amplitude modulation range
static constexpr float  MAX_PITCH_CENTS  = 50.0f;   // ±50 cents pitch shift range

// Sensor EMA — amplitude and pitch sensors both use a 25ms symmetric smoothing window.
static constexpr double SENSOR_SMOOTH_MS = 25.0;

// TIME channel: slow block-level smoother. Higher = lazier delay position tracking.
static constexpr double TIME_CHAN_SMOOTH_MS = 200.0;

// AMP channel: asymmetric envelope.
// Fast attack (20ms): catches transients.
// Slow release (400ms): holds gain level up for a natural trailing tail.
static constexpr double AMP_CHAN_ATTACK_MS  =  20.0;
static constexpr double AMP_CHAN_RELEASE_MS = 400.0;

// Block-level EMA pre-smoother for the delay target (runs before per-sample chase).
static constexpr double DELAY_SMOOTH_MS = 15.0;

// Per-sample delay chase rate — adapted from Chris Johnson's Airwindows ADT.
// The read position glides rather than snapping, producing a smooth pitch sweep.
// 0.0003 ≈ 75ms time constant at 44100 Hz. Smaller = slower/more subtle.
static constexpr float  DELAY_CHASE_RATE = 0.0003f;

// Pitch sensor normalisation: linear map Hz → 0..1 for modulation driving.
// Calibrate these to your source material (switch a stage to 'P' and observe the readout).
static constexpr float PITCH_NORM_MIN_HZ =    0.0f;
static constexpr float PITCH_NORM_MAX_HZ = 1000.0f;


// =============================================================================
// Constructor / Destructor
// =============================================================================
BADTAudioProcessor::BADTAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , apvts(*this, nullptr, "BADTParams", createParameterLayout())
{
}

BADTAudioProcessor::~BADTAudioProcessor() {}


// =============================================================================
// createParameterLayout  —  all user-facing parameters
// =============================================================================
// All ranges and defaults live here. Edit them here to retune the plugin.
juce::AudioProcessorValueTreeState::ParameterLayout
BADTAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // TIME (Ts)  —  delay depth.  Range: -1..+1.
    // Wet delay = |Ts × sampleSum| × maxDelaySamples (≈35ms at TS=1, signal=full).
    // Sign no longer inverts the dynamic mapping — use the per-stage INV button instead.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_TIME, 1 }, "Time (Ts)",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.001f, 1.0f),
        0.0f));

    // AMPLITUDE  —  wet-path gain modulation depth.  Range: 0..1.
    // Centre (0.5) = 0 dB.  Full deflection = ±MAX_GAIN_DB (currently ±9 dB).
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_AMP, 1 }, "Amplitude",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f, 1.0f),
        0.5f));

    // PITCH  —  pitch shift depth.  Range: ±MAX_PITCH_CENTS (currently ±50 cents).
    // Actual shift = sampleSum × pitchCtrl, so quiet passages get less shift.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_PITCH, 1 }, "Pitch (cents)",
        juce::NormalisableRange<float>(-MAX_PITCH_CENTS, MAX_PITCH_CENTS, 0.1f, 1.0f),
        0.0f));

    // Input / output gain trims.  ±24 dB, default 0 dB.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_IN_GAIN, 1 }, "Input Gain (dB)",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f, 1.0f),
        0.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_OUT_GAIN, 1 }, "Output Gain (dB)",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f, 1.0f),
        0.0f));

    // Stereo pan positions.  -1 = hard left, 0 = centre, +1 = hard right.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_DRY_PAN, 1 }, "Dry Pan",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f, 1.0f),
        0.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_WET_PAN, 1 }, "Wet Pan",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f, 1.0f),
        0.0f));

    // Path volume faders.  Range: -24..+6 dB.  Wet defaults to -10 dB.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_DRY_VOL, 1 }, "Dry Volume (dB)",
        juce::NormalisableRange<float>(-24.0f, 6.0f, 0.1f, 1.0f),
        0.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_WET_VOL, 1 }, "Wet Volume (dB)",
        juce::NormalisableRange<float>(-24.0f, 6.0f, 0.1f, 1.0f),
        -10.0f));

    // LFO.  Rate: 0..4 Hz.  Depth: 0..1 (fraction of ½ × maxDelaySamples).
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_LFO_RATE, 1 }, "LFO Rate (Hz)",
        juce::NormalisableRange<float>(0.0f, 4.0f, 0.01f, 1.0f),
        0.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ PARAM_LFO_DEPTH, 1 }, "LFO Depth",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f, 1.0f),
        0.0f));

    // 3-band EQ for dry and wet paths independently.
    // Band 1: low shelf   (20 Hz–2 kHz, ±12 dB)
    // Band 2: bell peak   (100 Hz–10 kHz, ±12 dB)
    // Band 3: high shelf  (1 kHz–20 kHz, ±12 dB)
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
// isBusesLayoutSupported
// =============================================================================
bool BADTAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    auto in = layouts.getMainInputChannelSet();
    return (in == juce::AudioChannelSet::mono() ||
            in == juce::AudioChannelSet::stereo());
}


// =============================================================================
// prepareToPlay
// =============================================================================
// Reinitialises all DSP state. Called before playback and on settings changes.
// All memory allocation happens here — never inside processBlock().
void BADTAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    analysisSamples = static_cast<int>(sampleRate * 0.020);  // 20ms analysis window
    maxDelaySamples = static_cast<int>(sampleRate * 0.035);  // 35ms max delay

    // Prepare the pitch shifter first so we can query its latency.
    // RubberBand's start delay (~6ms at 44100 Hz) is compensated by reading the
    // dry path from pitchLatencySamples back in the circular buffer.
    pitchProcessor.prepare(sampleRate, samplesPerBlock);
    pitchProcessor.reset();
    pitchLatencySamples = pitchProcessor.getLatencySamples();
    setLatencySamples(pitchLatencySamples);

    delayBuffer.prepare(maxDelaySamples + pitchLatencySamples + 8);
    analyzer.prepare(sampleRate, analysisSamples);

    delayReadPos = 0.0f;
    lfoPhase     = 0.0f;

    // EQ filters run mono (dry and wet are processed before panning).
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    spec.numChannels      = 1;

    dryEqBand1.prepare(spec); dryEqBand2.prepare(spec); dryEqBand3.prepare(spec);
    wetEqBand1.prepare(spec); wetEqBand2.prepare(spec); wetEqBand3.prepare(spec);
    dryEqBand1.reset();       dryEqBand2.reset();       dryEqBand3.reset();
    wetEqBand1.reset();       wetEqBand2.reset();       wetEqBand3.reset();

    // Brickwall limiter on wet output — threshold -0.1 dBFS, 50ms release.
    wetLimiter.prepare(spec);
    wetLimiter.setThreshold(-0.1f);
    wetLimiter.setRelease(50.0f);

    // Force EQ coefficient recalculation on first block.
    for (auto& v : prevDryEqParams) v = std::numeric_limits<float>::quiet_NaN();
    for (auto& v : prevWetEqParams) v = std::numeric_limits<float>::quiet_NaN();

    dryWorkBuffer.assign(static_cast<size_t>(samplesPerBlock), 0.0f);
    wetWorkBuffer.assign(static_cast<size_t>(samplesPerBlock), 0.0f);

    // EMA alphas — formula: alpha = 1 - exp(-blockSize / tau_samples)
    // Each alpha is recomputed here because it depends on both tau and sample rate.
    const double tauSamples = (SENSOR_SMOOTH_MS / 1000.0) * sampleRate;
    sensorSmoothAlpha = static_cast<float>(
        1.0 - std::exp(-static_cast<double>(samplesPerBlock) / tauSamples));

    const double timeChanTau = (TIME_CHAN_SMOOTH_MS / 1000.0) * sampleRate;
    timeChanAlpha = static_cast<float>(
        1.0 - std::exp(-static_cast<double>(samplesPerBlock) / timeChanTau));

    const double ampAttackTau  = (AMP_CHAN_ATTACK_MS  / 1000.0) * sampleRate;
    const double ampReleaseTau = (AMP_CHAN_RELEASE_MS / 1000.0) * sampleRate;
    ampChanAttackAlpha  = static_cast<float>(1.0 - std::exp(-static_cast<double>(samplesPerBlock) / ampAttackTau));
    ampChanReleaseAlpha = static_cast<float>(1.0 - std::exp(-static_cast<double>(samplesPerBlock) / ampReleaseTau));

    const double delayTauSamples = (DELAY_SMOOTH_MS / 1000.0) * sampleRate;
    delaySmoothAlpha = 1.0 - std::exp(-static_cast<double>(samplesPerBlock) / delayTauSamples);

    smoothedDelayTarget = 0.0f;
    smoothedAmplitude   = 0.0f;
    smoothedPitchHz     = 0.0f;
    smoothedTimeSS      = 0.0f;
    smoothedAmpSS       = 0.0f;
}


// =============================================================================
// releaseResources
// =============================================================================
void BADTAudioProcessor::releaseResources()
{
    pitchProcessor.reset();
}


// =============================================================================
// updateEQIfNeeded  —  rebuild biquad coefficients if EQ knobs have changed
// =============================================================================
// Skips if nothing moved (saves CPU). Called once per block for dry and wet paths.
void BADTAudioProcessor::updateEQIfNeeded(bool isDry)
{
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

    float freq[3], q[3], gain[3];
    for (int b = 0; b < 3; ++b)
    {
        freq[b] = *apvts.getRawParameterValue(freqIds[b]);
        q[b]    = *apvts.getRawParameterValue(qIds[b]);
        gain[b] = *apvts.getRawParameterValue(gainIds[b]);
    }

    bool changed = false;
    for (int b = 0; b < 3; ++b)
    {
        if (freq[b] != prevParams[b*3+0] ||
            q[b]    != prevParams[b*3+1] ||
            gain[b] != prevParams[b*3+2])
        { changed = true; break; }
    }
    if (!changed) return;

    for (int b = 0; b < 3; ++b)
    {
        prevParams[b*3+0] = freq[b];
        prevParams[b*3+1] = q[b];
        prevParams[b*3+2] = gain[b];
    }

    double sr = currentSampleRate;
    auto toLinear = [](float dB) { return juce::Decibels::decibelsToGain(dB); };

    // Band 1: low shelf  |  Band 2: bell peak  |  Band 3: high shelf
    auto b1 = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sr, freq[0], q[0], toLinear(gain[0]));
    auto b2 = juce::dsp::IIR::Coefficients<float>::makePeakFilter(sr, freq[1], q[1], toLinear(gain[1]));
    auto b3 = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, freq[2], q[2], toLinear(gain[2]));

    if (isDry) { dryEqBand1.coefficients = b1; dryEqBand2.coefficients = b2; dryEqBand3.coefficients = b3; }
    else       { wetEqBand1.coefficients = b1; wetEqBand2.coefficients = b2; wetEqBand3.coefficients = b3; }
}


// =============================================================================
// processBlock  —  main audio loop
// =============================================================================
void BADTAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    // Guard against oversized blocks (rare but possible with some hosts).
    if (numSamples > static_cast<int>(dryWorkBuffer.size()))
    {
        dryWorkBuffer.resize(static_cast<size_t>(numSamples), 0.0f);
        wetWorkBuffer.resize(static_cast<size_t>(numSamples), 0.0f);
    }

    // --- Parameter reads ---
    const float ts           = *apvts.getRawParameterValue(PARAM_TIME);
    const float ampControl   = *apvts.getRawParameterValue(PARAM_AMP);
    const float pitchCtrl    = *apvts.getRawParameterValue(PARAM_PITCH);
    const float dryPan       = *apvts.getRawParameterValue(PARAM_DRY_PAN);
    const float wetPan       = *apvts.getRawParameterValue(PARAM_WET_PAN);
    const float dryVolDB     = *apvts.getRawParameterValue(PARAM_DRY_VOL);
    const float wetVolDB     = *apvts.getRawParameterValue(PARAM_WET_VOL);
    const float dryVolLinear = juce::Decibels::decibelsToGain(dryVolDB);
    const float wetVolLinear = juce::Decibels::decibelsToGain(wetVolDB);

    updateEQIfNeeded(true);
    updateEQIfNeeded(false);

    // -------------------------------------------------------------------------
    // Step 1: Sensor analysis
    //
    // Reads the previous block's buffer — one block of latency (~10ms) is inaudible.
    // Both sensors run every block regardless of per-stage A/P selection.
    // -------------------------------------------------------------------------

    // Amplitude sensor — plain symmetric EMA. Output is shared by all three stages;
    // per-channel shaping is applied later, after selection and inversion.
    {
        const float rawAmp = analyzer.computeSampleSum(delayBuffer);
        smoothedAmplitude += sensorSmoothAlpha * (rawAmp - smoothedAmplitude);
    }
    const float ampDbVal     = 20.0f * std::log10(std::max(smoothedAmplitude, 1e-6f));
    const float ampSampleSum = juce::jlimit(0.0f, 1.0f, (ampDbVal + 40.0f) / 40.0f);

    // Pitch sensor — symmetric EMA. Holds last known pitch during silence.
    {
        const float rawHz = analyzer.analyzePitch(delayBuffer);
        if (rawHz > 0.0f)
            smoothedPitchHz = sensorSmoothAlpha * rawHz + (1.0f - sensorSmoothAlpha) * smoothedPitchHz;
        else
            smoothedPitchHz *= (1.0f - sensorSmoothAlpha);
    }
    const float pitchSampleSum = normalizePitch(smoothedPitchHz);

    // Per-stage A/P source selection
    const float timeSS  = timeUsePitch.load(std::memory_order_relaxed)  ? pitchSampleSum : ampSampleSum;
    const float ampSS   = ampUsePitch.load(std::memory_order_relaxed)   ? pitchSampleSum : ampSampleSum;
    const float pitchSS = pitchUsePitch.load(std::memory_order_relaxed) ? pitchSampleSum : ampSampleSum;

    // Per-stage inversion: maps sampleSum → (1 − sampleSum)
    const float effectiveTimeSS  = invertTime.load(std::memory_order_relaxed)  ? (1.0f - timeSS)  : timeSS;
    const float effectiveAmpSS   = invertAmp.load(std::memory_order_relaxed)   ? (1.0f - ampSS)   : ampSS;
    const float effectivePitchSS = invertPitch.load(std::memory_order_relaxed) ? (1.0f - pitchSS) : pitchSS;

    // Per-channel smoothing — applied after selection/inversion, before use.
    // TIME: slow EMA so the delay head drifts lazily rather than chasing every transient.
    smoothedTimeSS += timeChanAlpha * (effectiveTimeSS - smoothedTimeSS);
    // AMP: asymmetric attack/release — gain rises quickly and falls slowly.
    const float ampChanAlpha = (effectiveAmpSS > smoothedAmpSS) ? ampChanAttackAlpha : ampChanReleaseAlpha;
    smoothedAmpSS += ampChanAlpha * (effectiveAmpSS - smoothedAmpSS);

    // -------------------------------------------------------------------------
    // Step 2: Block-level modulation values
    // -------------------------------------------------------------------------

    const float rawDelayTarget = bypassTime.load(std::memory_order_relaxed)
                                 ? 0.0f
                                 : computeTargetDelaySamples(ts, smoothedTimeSS);

    // Pre-smooth the target before the per-sample chase to reduce high-frequency jitter.
    smoothedDelayTarget += static_cast<float>(delaySmoothAlpha) * (rawDelayTarget - smoothedDelayTarget);

    const float displaySign = (ts < 0.0f) ? -1.0f : 1.0f;
    displayDelayMs.store(displaySign * delayReadPos / static_cast<float>(currentSampleRate) * 1000.0f,
                         std::memory_order_relaxed);

    // LFO — half-depth capped at ½ × maxDelaySamples to avoid read-pos going below 0.
    const float lfoRate      = *apvts.getRawParameterValue(PARAM_LFO_RATE);
    const float lfoDepth     = *apvts.getRawParameterValue(PARAM_LFO_DEPTH);
    const float lfoHalfDepth = lfoDepth * static_cast<float>(maxDelaySamples) * 0.5f;

    const float amplitudeGain = bypassAmp.load(std::memory_order_relaxed)
                                ? 1.0f
                                : computeAmplitudeGain(ampControl, smoothedAmpSS);
    displayAmpDB.store(20.0f * std::log10(std::max(amplitudeGain, 1e-6f)), std::memory_order_relaxed);

    const float pitchCents = bypassPitch.load(std::memory_order_relaxed)
                             ? 0.0f
                             : (effectivePitchSS * pitchCtrl);
    pitchProcessor.setPitchCents(pitchCents);
    displayPitchCents.store(pitchCents, std::memory_order_relaxed);

    float dryL, dryR, wetL, wetR;
    computePanGains(dryPan, dryL, dryR);
    computePanGains(wetPan, wetL, wetR);

    // -------------------------------------------------------------------------
    // Step 3: Per-sample loop
    // -------------------------------------------------------------------------
    for (int i = 0; i < numSamples; ++i)
    {
        float monoSample = sumToMono(buffer, i);
        delayBuffer.write(monoSample);

        // Dry path reads pitchLatencySamples back so it stays in time-sync
        // with the wet path (which has RubberBand's built-in start delay).
        dryWorkBuffer[static_cast<size_t>(i)] = delayBuffer.readDelayed(pitchLatencySamples);

        // LFO
        if (lfoRate > 0.0f)
        {
            lfoPhase += lfoRate / static_cast<float>(currentSampleRate);
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
        }
        const float lfoOffset = (lfoRate > 0.0f)
            ? std::sin(juce::MathConstants<float>::twoPi * lfoPhase) * lfoHalfDepth
            : 0.0f;

        // Per-sample position chase — adapted from Chris Johnson's Airwindows ADT.
        // Never snaps to a new position; glides toward the target, producing a
        // smooth pitch sweep instead of a click on direction changes.
        delayReadPos += DELAY_CHASE_RATE * (smoothedDelayTarget - delayReadPos);
        const float maxPos    = static_cast<float>(maxDelaySamples - 1);
        const float pos       = juce::jlimit(0.0f, maxPos, delayReadPos + lfoOffset);
        wetWorkBuffer[static_cast<size_t>(i)] = delayBuffer.readDelayedInterpolated(pos);
    }

    // --- Step 4: Dry EQ ---
    for (int i = 0; i < numSamples; ++i)
    {
        float s = dryWorkBuffer[static_cast<size_t>(i)];
        s = dryEqBand1.processSample(s);
        s = dryEqBand2.processSample(s);
        s = dryEqBand3.processSample(s);
        dryWorkBuffer[static_cast<size_t>(i)] = s;
    }

    // --- Step 5: Wet amplitude gain ---
    for (int i = 0; i < numSamples; ++i)
        wetWorkBuffer[static_cast<size_t>(i)] *= amplitudeGain;

    // --- Step 6: Pitch shift ---
    pitchProcessor.processBlock(wetWorkBuffer.data(), numSamples);

    // --- Step 7: Wet EQ ---
    for (int i = 0; i < numSamples; ++i)
    {
        float s = wetWorkBuffer[static_cast<size_t>(i)];
        s = wetEqBand1.processSample(s);
        s = wetEqBand2.processSample(s);
        s = wetEqBand3.processSample(s);
        wetWorkBuffer[static_cast<size_t>(i)] = s;
    }

    // --- Step 7b: Wet limiter — safety net for extreme settings ---
    {
        float* wetPtr = wetWorkBuffer.data();
        juce::dsp::AudioBlock<float> wetBlock(&wetPtr, 1, static_cast<size_t>(numSamples));
        wetLimiter.process(juce::dsp::ProcessContextReplacing<float>(wetBlock));
    }

    // --- Step 8: Pan, mix, write stereo output ---
    const bool  isSoloDry = soloDry.load(std::memory_order_relaxed);
    const bool  isSoloWet = soloWet.load(std::memory_order_relaxed);
    const float dryMix    = (isSoloWet && !isSoloDry) ? 0.0f : 1.0f;
    const float wetMix    = (isSoloDry && !isSoloWet) ? 0.0f : 1.0f;

    float blockPeakL = 0.0f, blockPeakR = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float dry = dryWorkBuffer[static_cast<size_t>(i)];
        const float wet = wetWorkBuffer[static_cast<size_t>(i)];

        const float outLeft  = dry * dryMix * dryL * dryVolLinear + wet * wetMix * wetL * wetVolLinear;
        const float outRight = dry * dryMix * dryR * dryVolLinear + wet * wetMix * wetR * wetVolLinear;

        buffer.setSample(0, i, outLeft);
        buffer.setSample(1, i, outRight);

        if (std::abs(outLeft)  > blockPeakL) blockPeakL = std::abs(outLeft);
        if (std::abs(outRight) > blockPeakR) blockPeakR = std::abs(outRight);
    }

    outputLevelL.store(blockPeakL, std::memory_order_relaxed);
    outputLevelR.store(blockPeakR, std::memory_order_relaxed);
}


// =============================================================================
// sumToMono
// =============================================================================
// All internal processing (delay, analysis, pitch) runs on a mono mix.
// Stereo is recreated at output via the pan system.
float BADTAudioProcessor::sumToMono(const juce::AudioBuffer<float>& buf,
                                        int sampleIndex) const
{
    const int numCh = buf.getNumChannels();
    if (numCh == 0) return 0.0f;
    if (numCh == 1) return buf.getSample(0, sampleIndex);
    float sum = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
        sum += buf.getSample(ch, sampleIndex);
    return sum / static_cast<float>(numCh);
}


// =============================================================================
// computeTargetDelaySamples
// =============================================================================
// delay = |Ts| × sampleSum × maxDelaySamples.
// |Ts| keeps the knob magnitude mapped to depth regardless of sign;
// the per-stage INV button handles the inverted dynamic relationship.
float BADTAudioProcessor::computeTargetDelaySamples(float ts, float sampleSum) const
{
    const float maxSamples = static_cast<float>(maxDelaySamples - 1);
    return juce::jlimit(0.0f, maxSamples,
                        std::abs(ts) * sampleSum * static_cast<float>(maxDelaySamples));
}


// =============================================================================
// computeAmplitudeGain
// =============================================================================
// targetDB    = (ampControl - 0.5) × 2 × MAX_GAIN_DB   (knob centre = 0 dB)
// effectiveDB = targetDB × sampleSum                    (scales with signal level)
// return      = 10^(effectiveDB / 20)
float BADTAudioProcessor::computeAmplitudeGain(float ampControl, float sampleSum) const
{
    const float targetDB    = (ampControl - 0.5f) * 2.0f * MAX_GAIN_DB;
    const float effectiveDB = targetDB * sampleSum;
    return std::pow(10.0f, effectiveDB / 20.0f);
}


// =============================================================================
// computePanGains
// =============================================================================
// Constant-power: L = √2·cos(θ), R = √2·sin(θ),  θ maps pan -1..+1 → 0..π/2.
// The √2 factor ensures centre pan delivers unity gain rather than -3 dB.
void BADTAudioProcessor::computePanGains(float panPosition,
                                             float& leftGain,
                                             float& rightGain) const
{
    panPosition = juce::jlimit(-1.0f, 1.0f, panPosition);
    const float theta = (panPosition + 1.0f) * 0.5f * juce::MathConstants<float>::halfPi;
    leftGain  = juce::MathConstants<float>::sqrt2 * std::cos(theta);
    rightGain = juce::MathConstants<float>::sqrt2 * std::sin(theta);
}


// =============================================================================
// normalizePitch
// =============================================================================
// Linear map: PITCH_NORM_MIN_HZ → 0,  PITCH_NORM_MAX_HZ → 1.
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
// createEditor  +  plugin factory
// =============================================================================
juce::AudioProcessorEditor* BADTAudioProcessor::createEditor()
{
    return new BADTEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BADTAudioProcessor();
}
