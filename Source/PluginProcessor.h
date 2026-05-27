// =============================================================================
// PluginProcessor.h  —  BADT
// =============================================================================
#pragma once

#include <JuceHeader.h>
#include <vector>
#include <cmath>
#include <algorithm>

#include <rubberband/RubberBandLiveShifter.h>  // Rubber Band Library (Breakfastquay)


// =============================================================================
// CircularBuffer
// =============================================================================
// Fixed-size ring buffer. Write advances a pointer; read looks N positions back.
// readDelayedInterpolated() uses linear interpolation to prevent zipper noise
// when the delay position is modulated.
class CircularBuffer
{
public:
    void prepare(int sizeInSamples)
    {
        buffer.assign(sizeInSamples, 0.0f);
        writeIndex = 0;
        bufferSize = sizeInSamples;
    }

    void write(float sample)
    {
        buffer[writeIndex] = sample;
        writeIndex = (writeIndex + 1) % bufferSize;
    }

    float readDelayed(int delaySamples) const
    {
        delaySamples = juce::jlimit(0, bufferSize - 1, delaySamples);
        int readIndex = (writeIndex - delaySamples - 1 + bufferSize) % bufferSize;
        return buffer[readIndex];
    }

    float readDelayedInterpolated(float delaySamples) const
    {
        delaySamples = juce::jlimit(0.0f, static_cast<float>(bufferSize - 2), delaySamples);
        int   intPart  = static_cast<int>(delaySamples);
        float fracPart = delaySamples - static_cast<float>(intPart);
        float sampleA  = readDelayed(intPart);
        float sampleB  = readDelayed(intPart + 1);
        return sampleA + fracPart * (sampleB - sampleA);
    }

    // Mean absolute amplitude of the last nSamples — our "SampleSum" 0..1.
    float getAverageAmplitude(int nSamples) const
    {
        nSamples = juce::jlimit(1, bufferSize, nSamples);
        float sum = 0.0f;
        for (int i = 0; i < nSamples; ++i)
        {
            int idx = (writeIndex - i - 1 + bufferSize) % bufferSize;
            sum += std::abs(buffer[idx]);
        }
        return sum / static_cast<float>(nSamples);
    }

    int getSize() const { return bufferSize; }

private:
    std::vector<float> buffer;
    int writeIndex = 0;
    int bufferSize = 0;
};


// =============================================================================
// SignalAnalyzer
// =============================================================================
// Two sensor modes:
//   computeSampleSum() — mean absolute amplitude  →  0..1
//   analyzePitch()     — YIN pitch estimate  →  Hz
//                        (normalise to 0..1 via PITCH_NORM_MIN/MAX_HZ constants)
class SignalAnalyzer
{
public:
    void prepare(double newSampleRate, int newAnalysisSamples)
    {
        sampleRate      = newSampleRate;
        analysisSamples = newAnalysisSamples;
        pitchWorkBuf.assign(static_cast<size_t>(newAnalysisSamples), 0.0f);
        yinBuf.assign(static_cast<size_t>(newAnalysisSamples / 2 + 1), 0.0f);
    }

    float computeSampleSum(const CircularBuffer& buf) const
    {
        return buf.getAverageAmplitude(analysisSamples);
    }

    // YIN algorithm (de Cheveigné & Kawahara, 2002).
    // Computes the CMNDF and locates the first lag below a 0.15 threshold,
    // then refines with parabolic interpolation. More robust than ZCR or plain
    // autocorrelation on polyphonic or complex material.
    // Returns 0 Hz on silence or when no clear fundamental is detected.
    // Calibrate PITCH_NORM_MIN/MAX_HZ in PluginProcessor.cpp to match your source.
    float analyzePitch(const CircularBuffer& buf) const
    {
        const int N      = analysisSamples;
        const int maxLag = N / 2;

        for (int i = 0; i < N; ++i)
            pitchWorkBuf[i] = buf.readDelayed(N - 1 - i);

        // Energy gate — bail on silence / noise floor
        float energy = 0.0f;
        for (int i = 0; i < N; ++i)
            energy += pitchWorkBuf[i] * pitchWorkBuf[i];
        energy /= static_cast<float>(N);
        if (energy < 1e-4f) return 0.0f;

        // Difference function
        yinBuf[0] = 1.0f;
        for (int tau = 1; tau < maxLag; ++tau)
        {
            float sum = 0.0f;
            for (int j = 0; j < maxLag; ++j)
            {
                const float diff = pitchWorkBuf[j] - pitchWorkBuf[j + tau];
                sum += diff * diff;
            }
            yinBuf[tau] = sum;
        }

        // CMNDF (in-place)
        float cumSum = 0.0f;
        for (int tau = 1; tau < maxLag; ++tau)
        {
            cumSum += yinBuf[tau];
            yinBuf[tau] = (cumSum > 0.0f)
                ? (yinBuf[tau] * static_cast<float>(tau) / cumSum)
                : 1.0f;
        }

        // Threshold search + parabolic refinement
        const float threshold = 0.15f;
        int tauEstimate = -1;
        for (int tau = 2; tau < maxLag; ++tau)
        {
            if (yinBuf[tau] < threshold)
            {
                while (tau + 1 < maxLag && yinBuf[tau + 1] < yinBuf[tau])
                    ++tau;
                tauEstimate = tau;
                break;
            }
        }
        if (tauEstimate < 0) return 0.0f;

        const int x0 = (tauEstimate > 1)          ? tauEstimate - 1 : tauEstimate;
        const int x2 = (tauEstimate + 1 < maxLag) ? tauEstimate + 1 : tauEstimate;
        float betterTau;
        if (x0 == tauEstimate)
            betterTau = (yinBuf[tauEstimate] <= yinBuf[x2])
                        ? static_cast<float>(tauEstimate) : static_cast<float>(x2);
        else if (x2 == tauEstimate)
            betterTau = (yinBuf[tauEstimate] <= yinBuf[x0])
                        ? static_cast<float>(tauEstimate) : static_cast<float>(x0);
        else
        {
            const float s0 = yinBuf[x0], s1 = yinBuf[tauEstimate], s2 = yinBuf[x2];
            betterTau = static_cast<float>(tauEstimate)
                      + (s2 - s0) / (2.0f * (2.0f * s1 - s2 - s0));
        }

        if (betterTau < 1.0f) return 0.0f;
        return static_cast<float>(sampleRate) / betterTau;
    }

private:
    double sampleRate      = 44100.0;
    int    analysisSamples = 882;
    mutable std::vector<float> pitchWorkBuf;
    mutable std::vector<float> yinBuf;
};


// =============================================================================
// PitchProcessor  —  Rubber Band Library (Breakfastquay) wrapper
// =============================================================================
// Uses RubberBandLiveShifter for pitch-only shifting (no time stretch).
// OptionWindowShort gives ~50ms latency; quality is fine for the ±50 cent range.
//
// Block-size bridging: RubberBand requires exactly rbBlockSize samples per
// shift() call, but the DAW delivers variable-size blocks. Two FIFOs bridge
// the gap — inputFifo accumulates host samples, outputFifo holds processed
// samples. All memory is pre-allocated in prepare(); no audio-thread allocation
// after that point.
class PitchProcessor
{
public:
    PitchProcessor()  = default;
    ~PitchProcessor() = default;

    // Recreates the shifter for the current sample rate and pre-allocates FIFOs.
    // Must be called before processBlock().
    void prepare(double newSampleRate, int maxHostBlockSize)
    {
        shifter = std::make_unique<RubberBand::RubberBandLiveShifter>(
            static_cast<size_t>(newSampleRate),
            1,
            RubberBand::RubberBandLiveShifter::OptionWindowShort
        );

        rbBlockSize = static_cast<int>(shifter->getBlockSize());
        rbOutputBlock.assign(static_cast<size_t>(rbBlockSize), 0.0f);

        // Reserve for one steady-state host block plus a full RB block of headroom.
        const int reserveSize = maxHostBlockSize + rbBlockSize * 2 + 128;
        inputFifo.reserve(static_cast<size_t>(reserveSize));
        outputFifo.reserve(static_cast<size_t>(reserveSize));
        inputFifo.clear();
        outputFifo.clear();

        currentCents = 0.0f;
    }

    // RubberBand's fixed start-delay (typically ~6ms at 44100 Hz).
    // Report this to the DAW via setLatencySamples() and delay the dry path
    // by the same amount so wet and dry stay in sync.
    int getLatencySamples() const
    {
        if (!shifter) return 0;
        return static_cast<int>(shifter->getStartDelay());
    }

    void reset()
    {
        if (shifter) shifter->reset();
        inputFifo.clear();
        outputFifo.clear();
    }

    // Converts cents → frequency ratio (2^(cents/1200)) and passes to RubberBand.
    // Call once per block before processBlock().
    void setPitchCents(float cents)
    {
        currentCents = cents;
        if (!shifter) return;
        const double ratio = std::pow(2.0, static_cast<double>(cents) / 1200.0);
        shifter->setPitchScale(ratio);
    }

    // In-place pitch shift. Handles FIFO bridging transparently.
    // Outputs silence for the first block or two while the pipeline warms up.
    void processBlock(float* samples, int numSamples)
    {
        if (!shifter) return;

        inputFifo.insert(inputFifo.end(), samples, samples + numSamples);

        while (static_cast<int>(inputFifo.size()) >= rbBlockSize)
        {
            const float* inPtrs[1]  = { inputFifo.data() };
            float*       outPtrs[1] = { rbOutputBlock.data() };
            shifter->shift(inPtrs, outPtrs);
            inputFifo.erase(inputFifo.begin(), inputFifo.begin() + rbBlockSize);
            outputFifo.insert(outputFifo.end(), rbOutputBlock.begin(), rbOutputBlock.end());
        }

        if (static_cast<int>(outputFifo.size()) >= numSamples)
        {
            std::copy(outputFifo.begin(), outputFifo.begin() + numSamples, samples);
            outputFifo.erase(outputFifo.begin(), outputFifo.begin() + numSamples);
        }
        else
        {
            std::fill(samples, samples + numSamples, 0.0f);
        }
    }

private:
    std::unique_ptr<RubberBand::RubberBandLiveShifter> shifter;
    int                rbBlockSize = 512;
    std::vector<float> rbOutputBlock;
    std::vector<float> inputFifo;
    std::vector<float> outputFifo;
    float              currentCents = 0.0f;
};


// =============================================================================
// BADTAudioProcessor
// =============================================================================
class BADTAudioProcessor : public juce::AudioProcessor
{
public:
    BADTAudioProcessor();
    ~BADTAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi()  const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.035; }

    int  getNumPrograms()    override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---- APVTS parameter IDs ----
    juce::AudioProcessorValueTreeState apvts;

    static constexpr const char* PARAM_TIME     = "TIME_CTRL";
    static constexpr const char* PARAM_AMP      = "AMP_CTRL";
    static constexpr const char* PARAM_PITCH    = "PITCH_CTRL";
    static constexpr const char* PARAM_IN_GAIN  = "IN_GAIN";
    static constexpr const char* PARAM_OUT_GAIN = "OUT_GAIN";
    static constexpr const char* PARAM_DRY_PAN  = "DRY_PAN";
    static constexpr const char* PARAM_WET_PAN  = "WET_PAN";
    static constexpr const char* PARAM_DRY_VOL  = "DRY_VOL";
    static constexpr const char* PARAM_WET_VOL  = "WET_VOL";

    static constexpr const char* PARAM_D_EQ1_FREQ = "D_EQ1_FREQ";
    static constexpr const char* PARAM_D_EQ1_Q    = "D_EQ1_Q";
    static constexpr const char* PARAM_D_EQ1_GAIN = "D_EQ1_GAIN";
    static constexpr const char* PARAM_D_EQ2_FREQ = "D_EQ2_FREQ";
    static constexpr const char* PARAM_D_EQ2_Q    = "D_EQ2_Q";
    static constexpr const char* PARAM_D_EQ2_GAIN = "D_EQ2_GAIN";
    static constexpr const char* PARAM_D_EQ3_FREQ = "D_EQ3_FREQ";
    static constexpr const char* PARAM_D_EQ3_Q    = "D_EQ3_Q";
    static constexpr const char* PARAM_D_EQ3_GAIN = "D_EQ3_GAIN";

    static constexpr const char* PARAM_W_EQ1_FREQ = "W_EQ1_FREQ";
    static constexpr const char* PARAM_W_EQ1_Q    = "W_EQ1_Q";
    static constexpr const char* PARAM_W_EQ1_GAIN = "W_EQ1_GAIN";
    static constexpr const char* PARAM_W_EQ2_FREQ = "W_EQ2_FREQ";
    static constexpr const char* PARAM_W_EQ2_Q    = "W_EQ2_Q";
    static constexpr const char* PARAM_W_EQ2_GAIN = "W_EQ2_GAIN";
    static constexpr const char* PARAM_W_EQ3_FREQ = "W_EQ3_FREQ";
    static constexpr const char* PARAM_W_EQ3_Q    = "W_EQ3_Q";
    static constexpr const char* PARAM_W_EQ3_GAIN = "W_EQ3_GAIN";

    static constexpr const char* PARAM_LFO_RATE  = "LFO_RATE";
    static constexpr const char* PARAM_LFO_DEPTH = "LFO_DEPTH";

    // ---- VU levels (written by audio thread, read by GUI timer) ----
    std::atomic<float> outputLevelL { 0.0f };
    std::atomic<float> outputLevelR { 0.0f };

    // ---- Per-block effect values for debug display ----
    std::atomic<float> displayDelayMs    { 0.0f };
    std::atomic<float> displayAmpDB      { 0.0f };
    std::atomic<float> displayPitchCents { 0.0f };

    // ---- GUI → audio thread flags (all atomic for lock-free access) ----

    std::atomic<bool> soloDry { false };  // suppress wet path
    std::atomic<bool> soloWet { false };  // suppress dry path

    // Per-stage A/P source: false = amplitude sensor, true = pitch sensor.
    std::atomic<bool> timeUsePitch  { false };
    std::atomic<bool> ampUsePitch   { false };
    std::atomic<bool> pitchUsePitch { false };

    // Per-stage bypass: skips the effect entirely (unity/zero output).
    std::atomic<bool> bypassTime  { false };
    std::atomic<bool> bypassAmp   { false };
    std::atomic<bool> bypassPitch { false };

    // Per-stage invert: maps sampleSum → (1 − sampleSum), so the effect is
    // strongest during silence and fades as amplitude rises.
    std::atomic<bool> invertTime  { false };
    std::atomic<bool> invertAmp   { false };
    std::atomic<bool> invertPitch { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    CircularBuffer delayBuffer;
    SignalAnalyzer analyzer;
    PitchProcessor pitchProcessor;

    // Per-sample chase read head (technique from Chris Johnson's Airwindows ADT).
    // Never snaps to a new position — always glides, producing a smooth pitch sweep.
    float delayReadPos = 0.0f;

    float lfoPhase = 0.0f;

    juce::dsp::IIR::Filter<float> dryEqBand1, dryEqBand2, dryEqBand3;
    juce::dsp::IIR::Filter<float> wetEqBand1, wetEqBand2, wetEqBand3;
    float prevDryEqParams[9] { 0.0f };  // cached param values for change detection
    float prevWetEqParams[9] { 0.0f };

    juce::dsp::Limiter<float> wetLimiter;  // safety brickwall on wet output

    double currentSampleRate   = 44100.0;
    int    currentBlockSize    = 512;
    int    maxDelaySamples     = 882;   // 35ms at 44100 Hz
    int    analysisSamples     = 882;   // 20ms analysis window
    int    pitchLatencySamples = 0;     // RubberBand start delay (reported to DAW)

    // Amplitude sensor — plain symmetric EMA, shared by all three channels.
    float smoothedAmplitude = 0.0f;

    // Pitch sensor EMA.
    float smoothedPitchHz   = 0.0f;
    float sensorSmoothAlpha = 0.1f;  // recomputed in prepareToPlay (25ms tau)

    // TIME channel: slow smoother on effectiveTimeSS.
    // The delay head drifts gradually rather than reacting to every transient.
    float smoothedTimeSS = 0.0f;
    float timeChanAlpha  = 0.0f;     // recomputed in prepareToPlay (200ms tau)

    // AMP channel: asymmetric attack/release on effectiveAmpSS.
    // Fast attack (20ms) catches transients; slow release (400ms) creates a gain tail.
    float smoothedAmpSS       = 0.0f;
    float ampChanAttackAlpha  = 0.0f;  // recomputed in prepareToPlay
    float ampChanReleaseAlpha = 0.0f;

    // Block-level EMA pre-smoother for the delay target (runs before per-sample chase).
    double delaySmoothAlpha    = 0.0;   // recomputed in prepareToPlay (15ms tau)
    float  smoothedDelayTarget = 0.0f;

    std::vector<float> dryWorkBuffer;
    std::vector<float> wetWorkBuffer;

    float sumToMono(const juce::AudioBuffer<float>& buf, int sampleIndex) const;
    float computeTargetDelaySamples(float ts, float sampleSum) const;
    float computeAmplitudeGain(float ampControl, float sampleSum) const;
    void  computePanGains(float panPosition, float& leftGain, float& rightGain) const;
    void  updateEQIfNeeded(bool isDry);
    float normalizePitch(float hz) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BADTAudioProcessor)
};
