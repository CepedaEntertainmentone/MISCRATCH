#include "PluginProcessor.h"
#include <cmath>

MiScratchProcessor::MiScratchProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout MiScratchProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    using F = juce::AudioParameterFloat;
    using R = juce::NormalisableRange<float>;

    // 1 = normal, 0.5 = mitad de velocidad (una octava mas grave), 0 = congelado, negativo = al reves
    layout.add (std::make_unique<F> (juce::ParameterID { "speed", 1 }, "Velocidad", R (-2.0f, 2.0f, 0.001f), 1.0f));

    layout.add (std::make_unique<F> (juce::ParameterID { "scratch", 1 }, "Scratch cantidad", R (0.0f, 1.0f, 0.001f), 0.0f));

    R rateRange (0.25f, 20.0f, 0.01f);
    rateRange.setSkewForCentre (3.0f);
    layout.add (std::make_unique<F> (juce::ParameterID { "rate", 1 }, "Scratch Hz", rateRange, 4.0f));

    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "shape", 1 }, "Scratch forma",
                                                              juce::StringArray { "Seno", "Triangulo", "Sierra" }, 0));

    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "brake", 1 }, "Freno (tape stop)", false));
    layout.add (std::make_unique<F> (juce::ParameterID { "braketime", 1 }, "Tiempo de freno (s)", R (0.05f, 4.0f, 0.01f), 1.0f));
    layout.add (std::make_unique<F> (juce::ParameterID { "starttime", 1 }, "Tiempo de arranque (s)", R (0.05f, 2.0f, 0.01f), 0.3f));

    layout.add (std::make_unique<F> (juce::ParameterID { "drive", 1 }, "Saturacion", R (0.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<F> (juce::ParameterID { "mix", 1 }, "Mezcla", R (0.0f, 1.0f, 0.001f), 1.0f));
    return layout;
}

void MiScratchProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    windowSamples = (float) std::floor (0.08 * sr);
    baseDelay = windowSamples;                         // entero, para que la senal seca quede alineada
    bufSize = (int) (windowSamples * 5.5f) + 8;

    ring.setSize (2, bufSize);
    ring.clear();
    writeIdx = 0;

    phaseA = 0.0f;
    delayA = delayB = baseDelay;
    lfoPhase = 0.0f;
    stopFactor = apvts.getRawParameterValue ("brake")->load() > 0.5f ? 0.0f : 1.0f;

    speedSm.reset (sr, 0.03);
    depthSm.reset (sr, 0.03);
    driveSm.reset (sr, 0.03);
    mixSm.reset (sr, 0.03);
    speedSm.setCurrentAndTargetValue (apvts.getRawParameterValue ("speed")->load());
    depthSm.setCurrentAndTargetValue (apvts.getRawParameterValue ("scratch")->load());
    driveSm.setCurrentAndTargetValue (apvts.getRawParameterValue ("drive")->load());
    mixSm.setCurrentAndTargetValue (apvts.getRawParameterValue ("mix")->load());

    setLatencySamples ((int) baseDelay);
}

bool MiScratchProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

float MiScratchProcessor::readInterp (int ch, float d) const
{
    float pos = (float) writeIdx - d;
    while (pos < 0.0f)
        pos += (float) bufSize;

    int i0 = (int) pos;
    const float frac = pos - (float) i0;
    if (i0 >= bufSize) i0 -= bufSize;
    int i1 = i0 + 1;
    if (i1 >= bufSize) i1 -= bufSize;

    const float* x = ring.getReadPointer (ch);
    return x[i0] + frac * (x[i1] - x[i0]);
}

static float lfoShape (float ph, int shape)
{
    switch (shape)
    {
        case 0:  return std::sin (juce::MathConstants<float>::twoPi * ph);
        case 1:  return 4.0f * std::abs (ph - 0.5f) - 1.0f;
        default: return 2.0f * ph - 1.0f;
    }
}

void MiScratchProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int n = buffer.getNumSamples();
    if (numCh == 0 || bufSize == 0)
        return;

    speedSm.setTargetValue (apvts.getRawParameterValue ("speed")->load());
    depthSm.setTargetValue (apvts.getRawParameterValue ("scratch")->load());
    driveSm.setTargetValue (apvts.getRawParameterValue ("drive")->load());
    mixSm.setTargetValue   (apvts.getRawParameterValue ("mix")->load());

    const float lfoRate   = apvts.getRawParameterValue ("rate")->load();
    const int   shape     = (int) apvts.getRawParameterValue ("shape")->load();
    const bool  brakeOn   = apvts.getRawParameterValue ("brake")->load() > 0.5f;
    const float brakeTime = apvts.getRawParameterValue ("braketime")->load();
    const float startTime = apvts.getRawParameterValue ("starttime")->load();

    const float brakeStep = 1.0f / (brakeTime * (float) sr);
    const float startStep = 1.0f / (startTime * (float) sr);
    const float phaseStep = 1.0f / windowSamples;
    const int   dryOffset = (int) baseDelay;

    for (int i = 0; i < n; ++i)
    {
        const float speed = speedSm.getNextValue();
        const float depth = depthSm.getNextValue();
        const float drive = driveSm.getNextValue();
        const float mix   = mixSm.getNextValue();

        // Freno y arranque
        if (brakeOn) stopFactor = juce::jmax (0.0f, stopFactor - brakeStep);
        else         stopFactor = juce::jmin (1.0f, stopFactor + startStep);

        // Scratch automatico (LFO que mueve la velocidad hacia adelante y atras)
        lfoPhase += lfoRate / (float) sr;
        if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
        const float s = lfoShape (lfoPhase, shape);

        float rate = (1.0f - depth) * speed + depth * 2.0f * s;
        rate = juce::jlimit (-2.0f, 2.0f, rate) * stopFactor;

        // Leer la entrada y grabarla en el buffer circular
        float in[2] = { 0.0f, 0.0f };
        for (int ch = 0; ch < numCh; ++ch)
        {
            in[ch] = buffer.getReadPointer (ch)[i];
            ring.setSample (ch, writeIdx, in[ch]);
        }

        // Dos "cabezales de lectura" con ventana, desfasados medio ciclo
        const float prevA = phaseA;
        phaseA += phaseStep;
        if (phaseA >= 1.0f) phaseA -= 1.0f;
        if (phaseA < prevA) delayA = baseDelay;
        float phaseB = phaseA + 0.5f;
        if (phaseB >= 1.0f) phaseB -= 1.0f;
        if (prevA < 0.5f && phaseA >= 0.5f) delayB = baseDelay;

        delayA += (1.0f - rate);
        delayB += (1.0f - rate);
        delayA = juce::jlimit (1.0f, (float) (bufSize - 2), delayA);
        delayB = juce::jlimit (1.0f, (float) (bufSize - 2), delayB);

        const float sA = std::sin (juce::MathConstants<float>::pi * phaseA);
        const float sB = std::sin (juce::MathConstants<float>::pi * phaseB);
        const float wA = sA * sA;
        const float wB = sB * sB;

        for (int ch = 0; ch < numCh; ++ch)
        {
            float wet = wA * readInterp (ch, delayA) + wB * readInterp (ch, delayB);
            wet *= stopFactor;

            if (drive > 0.001f)
            {
                const float g = 1.0f + drive * 12.0f;
                wet = std::tanh (wet * g) / std::tanh (g);
            }

            int dryIdx = writeIdx - dryOffset;
            if (dryIdx < 0) dryIdx += bufSize;
            const float dry = ring.getSample (ch, dryIdx);

            buffer.getWritePointer (ch)[i] = dry * (1.0f - mix) + wet * mix;
        }

        if (++writeIdx >= bufSize) writeIdx = 0;
    }
}

juce::AudioProcessorEditor* MiScratchProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void MiScratchProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void MiScratchProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MiScratchProcessor();
}
