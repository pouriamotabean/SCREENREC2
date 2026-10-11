#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "FrameOverlay.h"

ScreenRecProcessor::ScreenRecProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Mic (Side-Chain)", juce::AudioChannelSet::stereo(), false))
{
}

ScreenRecProcessor::~ScreenRecProcessor()
{
    overlay.reset();
}

void ScreenRecProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    recorder.prepare (sampleRate, samplesPerBlock);
}

bool ScreenRecProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out.isDisabled() || layouts.getMainInputChannelSet() != out)
        return false;

    // Side-chain (mic): off, mono or stereo.
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        return sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo();
    }

    return true;
}

void ScreenRecProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    auto main = getBusBuffer (buffer, true, 0);

    // Mic side-chain: recorded into the video only, never sent to the output.
    juce::AudioBuffer<float> side;
    const juce::AudioBuffer<float>* sidePtr = nullptr;
    if (getBusCount (true) > 1 && getBus (true, 1) != nullptr && getBus (true, 1)->isEnabled())
    {
        side = getBusBuffer (buffer, true, 1);
        if (side.getNumChannels() > 0)
            sidePtr = &side;
    }

    // Audio passes through untouched; the recorder only taps it
    // (and adds the short beeps during a sync test).
    recorder.processAudio (main, sidePtr);
}

juce::AudioProcessorEditor* ScreenRecProcessor::createEditor()
{
    return new ScreenRecEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScreenRecProcessor();
}
