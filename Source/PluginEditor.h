#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Skinned editor. Everything is laid out in the coordinates of the 1536x1024
// skin image and scaled to the window, which keeps the 3:2 ratio.
class ScreenRecEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit ScreenRecEditor (ScreenRecProcessor&);
    ~ScreenRecEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr float skinW = 1536.0f, skinH = 1024.0f;

private:
    void timerCallback() override;
    juce::Rectangle<int> physicalScreenRectOf (const juce::Component&) const;
    void updateCaptureExclusion (bool exclude);

    struct FlashPanel : juce::Component
    {
        bool on = false;
        void paint (juce::Graphics& g) override
        {
            g.fillAll (on ? juce::Colours::white : juce::Colour (0xff050505));
        }
    };

    enum Area { aRec, aYoutube, aReels, aFrame, aQ480, aQ720, aQ1080, aQ4k, aMicTest, aSyncTest, aKnob,
                aPause, aDuck, aClicks, aStatus, aHelp, aCount };
    int areaAt (juce::Point<float> skinPos) const;
    juce::Point<float> toSkin (juce::Point<float> p) const  { return p / scale; }

    void drawButton (juce::Graphics&, juce::Rectangle<float>, const juce::String&, bool selected, bool enabled, bool hovered, float fontSize) const;
    void drawMetalButton (juce::Graphics&, juce::Rectangle<float>, const juce::String&, bool active, bool enabled, bool hovered) const;
    void setKnobFromDb (float db);
    float knobAngle() const;
    void showTemporaryStatus (const juce::String&);
    void drawHelp (juce::Graphics&) const;
    bool helpOpen = false;

    ScreenRecProcessor& processor;
    Recorder& recorder;
    FlashPanel flash;

    juce::Image plate, knob, plateCache;
    float scale = 1.0f;

    juce::Rectangle<float> areas[aCount];
    int hover = -1;
    void* excludedRoot = nullptr;

    // knob drag
    bool draggingKnob = false;
    float dragStartDb = 0.0f;

    // meters
    float vuDb = -40.0f;           // smoothed mic level shown on the VU (dBFS)
    float vuAngle = -0.95f;        // needle angle (radians), with ballistics
    float masterMeter = 0.0f;      // 0..1
    float masterPeakHold = 0.0f;
    int peakHoldTicks = 0;

    juce::String tempStatus;
    int tempStatusTicks = 0;
    int fastTimerTicks = 0;
    int saveSizeTicks = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScreenRecEditor)
};
