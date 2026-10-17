#include "PluginProcessor.h"

namespace th = bytebent::throat;

namespace
{
using APF = juce::AudioParameterFloat;
using Attr = juce::AudioParameterFloatAttributes;

// Ranges and curves match tools/bench/template.html: n^3 for times, linear otherwise.
juce::NormalisableRange<float> cubeRange (float min, float max)
{
    return { min, max,
             [] (float a, float b, float n) { return a + (b - a) * n * n * n; },
             [] (float a, float b, float v) { return std::cbrt (juce::jlimit (0.0f, 1.0f, (v - a) / (b - a))); },
             [] (float, float, float v) { return v; } };
}

juce::String pct (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }
juce::String secs (float v, int) { return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s"; }

const th::ParamInfo& info (th::Param p) { return th::paramInfo (p); }
juce::ParameterID pid (th::Param p) { return { info (p).id, 1 }; }
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout ThroatProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    auto unit = [&] (th::Param p, juce::String (*fmt) (float, int))
    {
        l.add (std::make_unique<APF> (pid (p), info (p).name, juce::NormalisableRange<float> (0.0f, 1.0f),
                                      (float) info (p).def, Attr().withStringFromValueFunction (fmt)));
    };
    auto time = [&] (th::Param p)
    {
        l.add (std::make_unique<APF> (pid (p), info (p).name, cubeRange ((float) info (p).min, (float) info (p).max),
                                      (float) info (p).def, Attr().withLabel ("s").withStringFromValueFunction (secs)));
    };

    for (auto p : { th::pVoicing, th::pBreathy, th::pTense, th::pCreaky, th::pThin, th::pGrowl, th::pJitter, th::pShimmer })
        unit (p, pct);

    l.add (std::make_unique<juce::AudioParameterInt> (pid (th::pTranspose), info (th::pTranspose).name, -24, 24, 0,
        juce::AudioParameterIntAttributes().withLabel ("st")));
    l.add (std::make_unique<APF> (pid (th::pDetune), info (th::pDetune).name, juce::NormalisableRange<float> (-100.0f, 100.0f), 0.0f,
        Attr().withLabel ("ct").withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + " ct"; })));
    l.add (std::make_unique<juce::AudioParameterChoice> (pid (th::pMode), info (th::pMode).name, juce::StringArray { "Mono", "Poly" }, 0));
    l.add (std::make_unique<APF> (pid (th::pGlide), info (th::pGlide).name, cubeRange (0.0f, 2.0f), (float) info (th::pGlide).def,
        Attr().withLabel ("s").withStringFromValueFunction ([] (float v, int i) { return v < 0.0015f ? juce::String ("Off") : secs (v, i); })));
    time (th::pAttack);
    time (th::pDecay);
    unit (th::pSustain, pct);
    time (th::pRelease);
    l.add (std::make_unique<APF> (pid (th::pVolume), info (th::pVolume).name, juce::NormalisableRange<float> (-48.0f, 6.0f), -6.0f,
        Attr().withLabel ("dB").withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " dB"; })));
    l.add (std::make_unique<juce::AudioParameterChoice> (pid (th::pSource), info (th::pSource).name, juce::StringArray { "Flow", "Derivative" }, 0));
    return l;
}

ThroatProcessor::ThroatProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (int i = 0; i < th::kNumParams; ++i)
        raw[(size_t) i] = apvts.getRawParameterValue (th::paramInfo (i).id);
}

bool ThroatProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void ThroatProcessor::prepareToPlay (double sampleRate, int)
{
    dsp.prepare (sampleRate);
    pushParams();
    setLatencySamples (th::Engine::latency());
}

void ThroatProcessor::pushParams()
{
    // choice and int parameters arrive as their index / value, which is what the engine expects
    for (int i = 0; i < th::kNumParams; ++i)
        dsp.setParam (i, raw[(size_t) i]->load (std::memory_order_relaxed));
}

void ThroatProcessor::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
        dsp.noteOn (m.getNoteNumber(), m.getVelocity());
    else if (m.isNoteOff())
        dsp.noteOff (m.getNoteNumber());
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        dsp.allNotesOff();
}

void ThroatProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    if (numCh <= 0 || n <= 0) return;

    pushParams();

    // render up to each MIDI event, so notes land where they were played
    float* out = buffer.getWritePointer (0);
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, n, meta.samplePosition);
        if (at > pos) { dsp.process (out + pos, at - pos); pos = at; }
        handleMidi (meta.getMessage());
    }
    if (pos < n) dsp.process (out + pos, n - pos);

    for (int c = 1; c < numCh; ++c)
        buffer.copyFrom (c, 0, buffer, 0, 0, n);
}

juce::AudioProcessorEditor* ThroatProcessor::createEditor()
{
    // plain editor for now; the web interface replaces it
    return new juce::GenericAudioProcessorEditor (*this);
}

void ThroatProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void ThroatProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ThroatProcessor();
}
