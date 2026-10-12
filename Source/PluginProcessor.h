#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Recorder.h"

class FrameOverlay;

class ScreenRecProcessor : public juce::AudioProcessor
{
public:
    ScreenRecProcessor();
    ~ScreenRecProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    Recorder recorder;
    std::unique_ptr<FrameOverlay> overlay;   // created by the editor (message thread)

    // Eye button: show this plugin's window in the video (for tutorials about
    // ScreenRec itself). Never saved: every new instance starts hidden.
    bool showPluginInVideo = false;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScreenRecProcessor)
};
