#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Recorder.h"

// The on-screen capture frame (16:9 YouTube or 9:16 Reels): outline with a
// centre marker, a MOVE handle (top-left: drag, mouse wheel = zoom,
// double-click = default size), a ZOOM handle (top-right: drag to resize
// around the centre) and a CLOSE button (bottom-right: hides the frame). Everything here is
// visible on the monitor but excluded from the recording, and the frame itself
// lets clicks through to Cubase.
class FrameOverlay : private juce::Timer
{
public:
    explicit FrameOverlay (Recorder&);
    ~FrameOverlay() override;

    void setEditorOpen (bool open)  { editorOpen = open; timerCallback(); }

private:
    struct FrameWindow : juce::Component
    {
        bool recording = false;
        void paint (juce::Graphics&) override;
    };

    struct Handle : juce::Component
    {
        enum class Kind { move, zoom, close };
        Handle (FrameOverlay& o, Kind k) : owner (o), kind (k) {}

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override  { hover = true;  repaint(); }
        void mouseExit (const juce::MouseEvent&) override   { hover = false; repaint(); }
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        FrameOverlay& owner;
        Kind kind;
        bool hover = false;
        double startCx = 0, startCy = 0, startH = 0;
    };

    void timerCallback() override;
    void show();
    void hide();
    void layout();

    juce::Rectangle<int> screenArea() const;          // primary display, logical px
    double minHeightFraction() const;
    Recorder::Mode mode() const                       { return recorder.getMode(); }
    double aspect() const                             { return Recorder::aspectOf (mode()); }
    void getFrame (double& cx, double& cy, double& h) const;
    void setFrame (double cx, double cy, double h);   // clamps, publishes, re-lays out
    void resetToDefault();                            // fits between title bar and taskbar
    juce::Rectangle<int> frameRectLogical() const;

    Recorder& recorder;
    FrameWindow frame;
    Handle moveHandle { *this, Handle::Kind::move };
    Handle zoomHandle { *this, Handle::Kind::zoom };
    Handle closeHandle { *this, Handle::Kind::close };

    bool editorOpen = false;
    bool shown = false;
    bool excludedOk = true;
    juce::Rectangle<int> lastFrameRect;
    bool lastRecording = false;
    int lastMode = -1;

    static constexpr int handleSize = 30;
    static constexpr int border = 4;

    JUCE_DECLARE_NON_COPYABLE (FrameOverlay)
};
