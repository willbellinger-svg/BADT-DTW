// =============================================================================
// PluginProcessor.h  —  BADT
// =============================================================================
#pragma once

#include <JuceHeader.h>
#include <vector>
#include <cmath>
#include <algorithm>

#include <rubberband/RubberBandLiveShifter.h>


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
// CLASS: PitchProcessor  (RubberBandLiveShifter wrapper)
// =============================================================================
//
// What this does:
//   Wraps RubberBandLiveShifter to provide simple in-place pitch shifting on a
//   mono float buffer. You call setPitchCents() once per block, then processBlock()
//   to apply the shift.
//
// Why RubberBandLiveShifter?
//   The Rubber Band Library's LiveShifter is designed specifically for pitch-only
//   shifting in real time — it does NOT change playback speed (time stretching).
//   It handles polyphonic and complex audio sources far better than SoundTouch
//   because it uses a high-quality phase-vocoder algorithm with a smarter
//   transient-handling system.
//
// The block-size bridging problem:
//   RubberBandLiveShifter requires EXACTLY getBlockSize() samples per shift()
//   call. However, the DAW may deliver any block size (64, 128, 512, 2048 …).
//   We bridge the gap with two FIFO queues:
//     inputFifo  — accumulates incoming host samples until we have a full RB block
//     outputFifo — holds processed samples until the host asks for them
//
// Real-time safety:
//   All memory is pre-allocated in prepare(). After that, no allocation happens
//   inside processBlock() — essential to prevent audio glitches on the audio thread.
//   We use std::vector with reserve() so insert() never triggers reallocation.
//
class PitchProcessor
{
public:
    PitchProcessor()  = default;
    ~PitchProcessor() = default;

    // -------------------------------------------------------------------------
    // prepare()  —  called before playback starts (or when sample rate changes)
    // -------------------------------------------------------------------------
    void prepare(double newSampleRate, int maxHostBlockSize)
    {
        // Create a new LiveShifter for this sample rate.
        //
        // RubberBandLiveShifter(sampleRate, channels, options):
        //   sampleRate — audio sample rate in Hz (e.g. 44100, 48000)
        //   channels   — 1 because BADT sums to mono before pitch shifting
        //   options    — OptionWindowShort: smaller FFT window = ~50ms latency
        //                (vs ~100ms for OptionWindowMedium).
        //                Quality is excellent for the ±50 cent range used here.
        //
        // std::make_unique<T>(...) is the C++14 way to create a heap object and
        // store it in a smart pointer. When this PitchProcessor is destroyed,
        // the shifter is automatically deleted — no manual delete needed.
        shifter = std::make_unique<RubberBand::RubberBandLiveShifter>(
            static_cast<size_t>(newSampleRate),
            1,
            RubberBand::RubberBandLiveShifter::OptionWindowShort
        );

        // Query the block size RubberBand requires for every shift() call.
        // This is fixed for the lifetime of this shifter (determined by the
        // FFT window size and sample rate). Typically 256 at 44100 Hz.
        rbBlockSize = static_cast<int>(shifter->getBlockSize());

        // Allocate a scratch buffer for one block of RubberBand output.
        // assign(n, 0.0f) fills a vector with n zeros.
        rbOutputBlock.assign(static_cast<size_t>(rbBlockSize), 0.0f);

        // Pre-allocate both FIFOs so no allocation can happen on the audio thread.
        //
        // How much to reserve:
        //   In steady state, inputFifo holds at most (rbBlockSize - 1) leftover
        //   samples from the last block. The new host block adds maxHostBlockSize.
        //   So the peak size is (rbBlockSize - 1) + maxHostBlockSize.
        //   We add rbBlockSize * 2 extra for safety, plus a 128-sample margin.
        //
        // reserve() sets aside memory without changing the vector's logical size.
        // After this, insert(end(), ...) will not trigger any reallocation as long
        // as we stay within the reserved capacity — which we always do here.
        const int reserveSize = maxHostBlockSize + rbBlockSize * 2 + 128;
        inputFifo.reserve(static_cast<size_t>(reserveSize));
        outputFifo.reserve(static_cast<size_t>(reserveSize));
        inputFifo.clear();
        outputFifo.clear();

        currentCents = 0.0f;
    }

    // -------------------------------------------------------------------------
    // reset()  —  flush internal state when playback stops
    // -------------------------------------------------------------------------
    void reset()
    {
        // RubberBand's reset() clears its internal FFT overlap-add state and
        // delay lines so stale audio from a previous session doesn't bleed in.
        if (shifter) shifter->reset();

        // Empty both FIFOs so no old samples queue up into the next session.
        inputFifo.clear();
        outputFifo.clear();
    }

    // -------------------------------------------------------------------------
    // setPitchCents()  —  set the pitch shift amount
    // -------------------------------------------------------------------------
    // Call this once per block before processBlock().
    // cents: positive = shift up, negative = shift down.
    //        100 cents = 1 semitone.  0 cents = no shift.
    void setPitchCents(float cents)
    {
        currentCents = cents;
        if (!shifter) return;

        // Convert cents to a linear frequency ratio for RubberBand.
        //
        // RubberBand uses ratios:
        //   1.0 = unison (no change)
        //   2.0 = one octave up   (frequency × 2)
        //   0.5 = one octave down (frequency ÷ 2)
        //
        // Formula: ratio = 2 ^ (cents / 1200)
        //   Why 1200? — 1 octave = 12 semitones × 100 cents/semitone = 1200 cents.
        //   So 2^(1200/1200) = 2^1 = 2.0 (octave up). Checks out. ✓
        //
        // Examples:
        //   +100 cents → 2^(100/1200) ≈ 1.0595  (one semitone up)
        //    +50 cents → 2^(50/1200)  ≈ 1.0293  (quarter-tone up)
        //      0 cents → 2^0           = 1.0000  (no change)
        //    -50 cents → 2^(-50/1200) ≈ 0.9716  (quarter-tone down)
        const double ratio = std::pow(2.0, static_cast<double>(cents) / 1200.0);
        shifter->setPitchScale(ratio);
    }

    // -------------------------------------------------------------------------
    // processBlock()  —  apply pitch shift in-place
    // -------------------------------------------------------------------------
    // Reads from samples[], writes the pitch-shifted result back to samples[].
    // numSamples can be any size — we bridge to RubberBand's fixed block size.
    void processBlock(float* samples, int numSamples)
    {
        if (!shifter) return;

        // ---- STEP 1: Push incoming host samples into the input FIFO ----
        //
        // insert(end(), ptr, ptr+n) appends n elements from a raw array.
        // Because we called reserve() in prepare(), this never allocates.
        inputFifo.insert(inputFifo.end(), samples, samples + numSamples);

        // ---- STEP 2: Drain the input FIFO in fixed RubberBand blocks ----
        //
        // RubberBand's shift() must receive EXACTLY rbBlockSize samples every time.
        // We loop as long as there are enough samples waiting in the FIFO.
        while (static_cast<int>(inputFifo.size()) >= rbBlockSize)
        {
            // shift() takes arrays of channel pointers — one pointer per channel.
            // We have 1 channel (mono), so each array has just one element.
            // inputFifo.data() always points to the FIRST (oldest) sample in the
            // vector, which is the correct "front of queue" position.
            const float* inPtrs[1]  = { inputFifo.data() };
            float*       outPtrs[1] = { rbOutputBlock.data() };

            // This is the core pitch-shifting call.
            // Reads exactly rbBlockSize samples from inPtrs[0].
            // Writes exactly rbBlockSize pitch-shifted samples to outPtrs[0].
            shifter->shift(inPtrs, outPtrs);

            // Remove the consumed samples from the front of the input FIFO.
            // erase(begin, begin+n) shifts all later elements leftward — O(n) copy,
            // but n is at most rbBlockSize (typically 256) so this is very fast.
            inputFifo.erase(inputFifo.begin(),
                            inputFifo.begin() + rbBlockSize);

            // Append the processed output block to the output FIFO.
            outputFifo.insert(outputFifo.end(),
                              rbOutputBlock.begin(),
                              rbOutputBlock.end());
        }

        // ---- STEP 3: Pull numSamples from the output FIFO into samples[] ----
        //
        // In steady state the output FIFO always has enough samples after step 2.
        // The only exception is the very first block after prepare() — before
        // RubberBand has produced any output yet ("pipeline warm-up" latency).
        if (static_cast<int>(outputFifo.size()) >= numSamples)
        {
            // Copy the front numSamples from the FIFO back to the caller's buffer.
            // std::copy works on any iterator range; for vector it's essentially memcpy.
            std::copy(outputFifo.begin(),
                      outputFifo.begin() + numSamples,
                      samples);

            // Remove those samples from the front of the output FIFO.
            outputFifo.erase(outputFifo.begin(),
                             outputFifo.begin() + numSamples);
        }
        else
        {
            // Pipeline not yet full — output silence this block.
            // This only happens for the first block or two after prepare().
            // On the wet path this brief silence is hidden by the delay buffer.
            std::fill(samples, samples + numSamples, 0.0f);
        }
    }

private:
    // The RubberBand LiveShifter object.
    // unique_ptr manages its lifetime — automatically deleted when we are.
    std::unique_ptr<RubberBand::RubberBandLiveShifter> shifter;

    // The fixed block size that shift() requires per call.
    // Queried after construction; does not change until prepare() is called again.
    int rbBlockSize = 512;

    // One block of output scratch space, reused every RubberBand pass.
    std::vector<float> rbOutputBlock;

    // Input FIFO: accumulates host audio until we have a full RubberBand block.
    // Output FIFO: holds processed audio until the host reads it.
    // Both are pre-allocated in prepare() — no audio-thread allocation occurs.
    std::vector<float> inputFifo;
    std::vector<float> outputFifo;

    // The most recently set shift amount in cents (kept for reference).
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

    // Per-path volume faders (-24 to +6 dB).
    // DRY_VOL scales the unprocessed signal before it reaches the mix output.
    // WET_VOL replaces the old hard-coded -6 dB wet offset — now user-adjustable.
    static constexpr const char* PARAM_DRY_VOL = "DRY_VOL";
    static constexpr const char* PARAM_WET_VOL = "WET_VOL";

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

    // ---- VU level (written by audio thread, read by GUI) ----
    // Single output level: peak of max(|left|, |right|) across the block.
    std::atomic<float> outputLevelL { 0.0f };

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

    // Dual-head crossfader for the amplitude-driven delay.
    //
    // When the target delay changes, we DON'T ramp the read position (that causes
    // pitch shift). Instead we snap instantly to the new position (head A) and
    // crossfade the audio from the old position (head B) over a short window.
    // Both heads read at a constant sample offset during the crossfade, so neither
    // produces any pitch artifact — only the blend changes.
    float delayHeadA         = 0.0f;  // current (target) read position in samples
    float delayHeadB         = 0.0f;  // previous read position, fading out
    int   xfadeSamplesLeft   = 0;     // countdown: samples remaining in crossfade
    int   xfadeLengthSamples = 441;   // total crossfade length (recomputed in prepare())

    float lfoPhase = 0.0f; // LFO phase accumulator 0..1

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
