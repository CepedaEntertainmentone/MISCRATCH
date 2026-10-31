#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

class MiScratchProcessor : public juce::AudioProcessor
{
public:
    MiScratchProcessor();
    ~MiScratchProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MiScratch"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    float readInterp (int channel, float delaySamples) const;

    juce::AudioProcessorValueTreeState apvts;

    // Buffer circular donde se graba el audio que entra
    juce::AudioBuffer<float> ring;
    int bufSize = 0;
    int writeIdx = 0;

    double sr = 44100.0;
    float windowSamples = 3528.0f;   // largo del "grano" (80 ms)
    float baseDelay = 3528.0f;       // latencia fija del plugin

    float phaseA = 0.0f;             // fase de la ventana A (la B va desfasada medio ciclo)
    float delayA = 0.0f, delayB = 0.0f;
    float lfoPhase = 0.0f;
    float stopFactor = 1.0f;         // 1 = girando normal, 0 = frenado

    juce::SmoothedValue<float> speedSm, depthSm, driveSm, mixSm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiScratchProcessor)
};
