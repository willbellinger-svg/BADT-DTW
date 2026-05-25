// =============================================================================
// PluginProcessor.h  —  BADT
// =============================================================================
#pragma once

#include <JuceHeader.h>
#include <vector>
#include <cmath>
#include <algorithm>

#include <signalsmith-stretch/signalsmith-stretch.h>


// =============================================================================
// CLASS: CircularBuffer
// =============================================================================
// Fixed-size ring buffer for audio delay and analysis.
// Write advances a pointer; read looks N positions back with optional
// fractional-sample linear interpolation.
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

    // Linear interpolation between two adjacent integer delay positions.
    // Prevents zipper noise when delay time is modulated.
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
// CLASS: SignalAnalyzer
// =============================================================================
// Two sensor modes:
//   computeSampleSum()  — mean absolute amplitude, returns 0..1 directly.
//   analyzePitch()      — autocorrelation fundamental estimate, returns Hz
//                         (caller normalises to 0..1 using known min/max).
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

    // AMPLITUDE SENSOR: mean absolute amplitude of the last analysisSamples.
    float computeSampleSum(const CircularBuffer& buf) const
    {
        return buf.getAverageAmplitude(analysisSamples);
    }

    // FREQUENCY SENSOR (YIN algorithm): returns an estimated fundamental in Hz.
    // Returns 0 if the signal is too quiet or no clear pitch is found.
    //
    // YIN (de Cheveigné & Kawahara 2002) computes a cumulative mean-normalised
    // difference function (CMNDF) and finds the first lag where it dips below a
    // threshold (0.15), then refines the estimate with parabolic interpolation.
    // It reliably tracks the fundamental even on complex polyphonic audio where
    // ZCR and plain autocorrelation give spurious readings.
    //
    // HOW TO CALIBRATE PITCH_NORM_MIN/MAX_HZ:
    //   Switch GUI to PITCH mode, play your audio, note min/max Hz in the readout.
    //   Update those constants in PluginProcessor.cpp.
    float analyzePitch(const CircularBuffer& buf) const
    {
        const int N       = analysisSamples;
        const int maxLag  = N / 2;

        // Copy last N samples into work buffer, oldest first.
        for (int i = 0; i < N; ++i)
            pitchWorkBuf[i] = buf.readDelayed(N - 1 - i);

        // Energy gate: bail on silence / noise floor.
        float energy = 0.0f;
        for (int i = 0; i < N; ++i)
            energy += pitchWorkBuf[i] * pitchWorkBuf[i];
        energy /= static_cast<float>(N);
        if (energy < 1e-4f) return 0.0f;

        // Difference function: d(tau) = sum_{j=0}^{maxLag-1} (x[j] - x[j+tau])^2
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

        // Cumulative mean normalised difference function (CMNDF) — in-place.
        // yinBuf[tau] = d(tau) * tau / sum_{j=1}^{tau} d(j)
        float cumSum = 0.0f;
        for (int tau = 1; tau < maxLag; ++tau)
        {
            cumSum += yinBuf[tau];
            yinBuf[tau] = (cumSum > 0.0f)
                ? (yinBuf[tau] * static_cast<float>(tau) / cumSum)
                : 1.0f;
        }

        // Threshold search: first tau where CMNDF dips below 0.15 and is a local min.
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

        // Parabolic interpolation for sub-sample period accuracy.
        const int x0 = (tauEstimate > 1)           ? tauEstimate - 1 : tauEstimate;
        const int x2 = (tauEstimate + 1 < maxLag)  ? tauEstimate + 1 : tauEstimate;
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
    mutable std::vector<float> pitchWorkBuf; // N samples: input copy for analysis
    mutable std::vector<float> yinBuf;       // N/2 samples: CMNDF scratch space
};


// =============================================================================
// CLASS: PitchProcessor  (signalsmith-stretch wrapper)
// =============================================================================
class PitchProcessor
{
public:
    void prepare(double newSampleRate, int newMaxBlockSize)
    {
        stretcher.presetDefault(1, static_cast<float>(newSampleRate));
        stretcher.reset();
        outputBuf.resize(static_cast<size_t>(newMaxBlockSize));
    }

    void reset() { stretcher.reset(); }

    void setPitchCents(float cents)
    {
        currentCents = cents;
        stretcher.setTransposeSemitones(cents / 100.0f);
    }

    void processBlock(float* samples, int numSamples)
    {
        // Skip the stretcher entirely when pitch is zero — no audible effect and
        // avoids triggering signalsmith-stretch's STFT paths before any pitch is set.
        if (currentCents == 0.0f)
        {
            stretcher.reset();
            return;
        }
        if (numSamples > static_cast<int>(outputBuf.size()))
            outputBuf.resize(static_cast<size_t>(numSamples));
        float* in[1]  = { samples };
        float* out[1] = { outputBuf.data() };
        stretcher.process(in, numSamples, out, numSamples);
        std::copy(outputBuf.begin(), outputBuf.begin() + numSamples, samples);
    }

private:
    signalsmith::stretch::SignalsmithStretch<float> stretcher;
    std::vector<float> outputBuf;
    float currentCents = 0.0f;
};


// =============================================================================
// CLASS: BADTAudioProcessor
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
    double getTailLengthSeconds() const override { return 0.02; }

    int  getNumPrograms()    override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---- APVTS parameter IDs ----
    juce::AudioProcessorValueTreeState apvts;

    static constexpr const char* PARAM_TIME  = "TIME_CTRL";
    static constexpr const char* PARAM_AMP   = "AMP_CTRL";
    static constexpr const char* PARAM_PITCH = "PITCH_CTRL";
    static constexpr const char* PARAM_IN_GAIN  = "IN_GAIN";
    static constexpr const char* PARAM_OUT_GAIN = "OUT_GAIN";
    static constexpr const char* PARAM_DRY_PAN = "DRY_PAN";
    static constexpr const char* PARAM_WET_PAN = "WET_PAN";

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

    static constexpr const char* PARAM_LFO_RATE = "LFO_RATE";

    // ---- VU levels (written by audio thread, read by GUI) ----
    std::atomic<float> inputLevelL  { 0.0f };
    std::atomic<float> inputLevelR  { 0.0f };
    std::atomic<float> outputLevelL { 0.0f };
    std::atomic<float> outputLevelR { 0.0f };

    // ---- Solo path flags (written by GUI, read by audio thread) ----
    // soloDry = suppress wet path (hear dry only)
    // soloWet = suppress dry path (hear wet only)
    // If both are active simultaneously, both paths play (cancels out).
    std::atomic<bool> soloDry { false };
    std::atomic<bool> soloWet { false };

    // ---- Sensor mode flag (written by GUI, read by audio thread) ----
    // false = amplitude mode (default)
    // true  = pitch mode
    std::atomic<bool> usePitchSensor { false };

    // ---- Raw sensor value for GUI display (written by audio thread) ----
    // Amplitude mode: stores sampleSum (0..1)
    // Pitch mode:     stores raw Hz before normalisation
    std::atomic<float> rawSensorValue { 0.0f };

    // ---- Bypass flags (written by GUI, read by audio thread) ----
    // When true, the corresponding effect is skipped — wet signal is not delayed,
    // amplitude-modulated, or pitch-shifted respectively.
    std::atomic<bool> bypassTime  { false };
    std::atomic<bool> bypassAmp   { false };
    std::atomic<bool> bypassPitch { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    CircularBuffer  delayBuffer;
    SignalAnalyzer  analyzer;
    PitchProcessor  pitchProcessor;

    float blockDelaySamples = 0.0f; // delay position used for the current block
    float prevDelaySamples  = 0.0f; // delay position from the previous block (crossfade)
    float lfoPhase          = 0.0f; // LFO phase accumulator 0..1

    juce::dsp::IIR::Filter<float> dryEqBand1, dryEqBand2, dryEqBand3;
    juce::dsp::IIR::Filter<float> wetEqBand1, wetEqBand2, wetEqBand3;
    float prevDryEqParams[9] { 0.0f };
    float prevWetEqParams[9] { 0.0f };

    // Brickwall limiter on the wet channel — always active, prevents runaway
    // amplitude modulation from clipping the output.
    juce::dsp::Limiter<float> wetLimiter;

    double currentSampleRate = 44100.0;
    int    currentBlockSize  = 512;
    int    maxDelaySamples   = 882;
    int    analysisSamples   = 882;
    float  sampleSum         = 0.0f;

    // EMA state for sensor smoothing (audio thread only — no atomics needed).
    float smoothedAmplitude = 0.0f; // Smoothed 0..1 amplitude value
    float smoothedPitchHz   = 0.0f; // Smoothed Hz value (held on silence)
    float sensorSmoothAlpha = 0.1f; // EMA coefficient — recomputed in prepareToPlay()

    std::vector<float> dryWorkBuffer;
    std::vector<float> wetWorkBuffer;

    float sumToMono(const juce::AudioBuffer<float>& buf, int sampleIndex) const;
    float computeTargetDelaySamples(float ts) const;
    float computeAmplitudeGain(float ampControl) const;
    void  computePanGains(float panPosition, float& leftGain, float& rightGain) const;
    void  updateEQIfNeeded(bool isDry);

    // Normalise a raw pitch Hz reading to 0..1 using calibration constants.
    float normalizePitch(float hz) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BADTAudioProcessor)
};
