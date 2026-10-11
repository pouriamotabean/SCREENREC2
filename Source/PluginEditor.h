#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class ScreenRecEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit ScreenRecEditor (ScreenRecProcessor&);
    ~ScreenRecEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    juce::Rectangle<int> physicalScreenRectOf (const juce::Component&) const;
    void updateCaptureExclusion (bool exclude);

    struct FlashPanel : juce::Component
    {
        bool on = false;
        void paint (juce::Graphics& g) override
        {
            g.fillAll (on ? juce::Colours::white : juce::Colour (0xff101012));
        }
    };

    ScreenRecProcessor& processor;
    Recorder& recorder;
    FlashPanel flash;

    juce::Rectangle<int> recButtonArea, syncArea, timeArea, fullArea, reelsArea;
    bool hoverRec = false, hoverSync = false, hoverFull = false, hoverReels = false;   // hoverFull = YOUTUBE
    void* excludedRoot = nullptr;   // top-level window currently hidden from capture
    juce::Rectangle<int> meterArea;
    float micMeter = 0.0f;          // displayed level, 0..1
    int fastTimerTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScreenRecEditor)
};
