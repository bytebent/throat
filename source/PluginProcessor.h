#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "ThroatDSP.h"
#include <array>
#include <atomic>

// Live view data, written by the audio thread and read by the editor (relaxed atomics: a torn
// frame only means one slightly odd picture, never a crash or a lock).
struct ThroatView
{
    static constexpr int N = 256, RING = 2048;
    std::array<std::atomic<float>, N> g {}, n {};
    std::atomic<float> f0 { 0 }, rd { 1 }, oq { 0 };
    std::atomic<int> voices { 0 };
    std::array<std::atomic<float>, RING> ring {};
    std::atomic<int> ringPos { 0 };
};

class ThroatProcessor : public juce::AudioProcessor
{
public:
    ThroatProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Throat"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    ThroatView view;
    std::atomic<bool> editorOpen { false };
    std::atomic<double> currentSampleRate { 48000.0 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void pushParams();
    void handleMidi (const juce::MidiMessage& m);
    void publishView (const float* out, int n);

    bytebent::throat::Engine dsp;
    std::array<std::atomic<float>*, bytebent::throat::kNumParams> raw {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThroatProcessor)
};
