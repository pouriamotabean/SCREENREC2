#include "PluginEditor.h"
#include "Timing.h"
#include "FrameOverlay.h"
#include "WinUtils.h"

namespace
{
    const juce::Colour bg       (0xff1b1c1f);
    const juce::Colour panel    (0xff232428);
    const juce::Colour line     (0xff34363c);
    const juce::Colour text     (0xffd9d6cf);
    const juce::Colour dimText  (0xff8a8780);
    const juce::Colour amber    (0xffe0a040);
    const juce::Colour recRed   (0xffe0342c);

    juce::String formatTime (double seconds)
    {
        const int s = (int) seconds;
        return juce::String::formatted ("%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    }
}

ScreenRecEditor::ScreenRecEditor (ScreenRecProcessor& p)
    : AudioProcessorEditor (&p), processor (p), recorder (p.recorder)
{
    addChildComponent (flash);
    flash.setInterceptsMouseClicks (false, false);
    setSize (440, 230);
    recorder.warmUp();

    if (processor.overlay == nullptr)
        processor.overlay = std::make_unique<FrameOverlay> (recorder);
    processor.overlay->setEditorOpen (true);

    startTimerHz (30);
}

ScreenRecEditor::~ScreenRecEditor()
{
    stopTimer();
    updateCaptureExclusion (false);

    if (processor.overlay != nullptr)
        processor.overlay->setEditorOpen (false);
}

// Hide the whole plugin window from the recording (it stays visible on the
// monitor). Only the plugin's own floating window is touched: if the top-level
// window is much bigger than the editor (e.g. a docked layout), nothing is done.
void ScreenRecEditor::updateCaptureExclusion (bool exclude)
{
    void* root = nullptr;

    if (exclude)
        if (auto* peer = getPeer())
        {
            root = WinUtils::getTopLevelWindow (peer->getNativeHandle());

            int w = 0, h = 0;
            const double scale = juce::Component::getApproximateScaleFactorForComponent (this);
            const int maxW = (int) ((getWidth() + 400) * scale), maxH = (int) ((getHeight() + 400) * scale);

            if (! WinUtils::getWindowSize (root, w, h) || w > maxW || h > maxH)
                root = nullptr;
        }

    if (root == excludedRoot)
        return;

    if (excludedRoot != nullptr)
        WinUtils::setExcludedFromCapture (excludedRoot, false);

    excludedRoot = (root != nullptr && WinUtils::setExcludedFromCapture (root, true)) ? root : nullptr;
}

void ScreenRecEditor::resized()
{
    recButtonArea = { 28, 62, 104, 104 };
    timeArea = { 156, 78, 260, 64 };
    syncArea = { getWidth() - 112, getHeight() - 34, 96, 22 };
    fullArea = { 156, 148, 72, 22 };
    reelsArea = { 232, 148, 62, 22 };
    meterArea = { 312, 152, 104, 14 };
    flash.setBounds (timeArea);
}

juce::Rectangle<int> ScreenRecEditor::physicalScreenRectOf (const juce::Component& c) const
{
    const auto logical = c.getScreenBounds();
    auto physical = juce::Desktop::getInstance().getDisplays().logicalToPhysical (logical);
    // Sample only the middle of the panel so small coordinate errors don't matter.
    return physical.reduced (physical.getWidth() / 4, physical.getHeight() / 4);
}

void ScreenRecEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    const auto state = recorder.getState();
    const bool recording = state == Recorder::State::recording;
    const bool busy = state == Recorder::State::starting || state == Recorder::State::saving;

    // ---- header
    g.setColour (panel);
    g.fillRect (0, 0, getWidth(), 44);
    g.setColour (line);
    g.drawHorizontalLine (44, 0.0f, (float) getWidth());

    g.setColour (text);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("SCREENREC", 20, 0, 200, 44, juce::Justification::centredLeft);

    g.setColour (amber);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("BY POURIA MOTABEAN", getWidth() - 220, 0, 200, 44, juce::Justification::centredRight);

    // ---- REC / STOP button
    auto b = recButtonArea.toFloat();
    g.setColour (panel.brighter (hoverRec && ! busy ? 0.15f : 0.0f));
    g.fillEllipse (b);
    g.setColour (line.brighter (0.2f));
    g.drawEllipse (b.reduced (1.0f), 2.0f);

    const auto inner = b.reduced (34.0f);
    if (recording)
    {
        g.setColour (text);
        g.fillRoundedRectangle (inner, 4.0f);
    }
    else
    {
        g.setColour (busy ? recRed.withAlpha (0.35f) : recRed);
        g.fillEllipse (inner.expanded (4.0f));
    }

    g.setColour (dimText);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (recording ? "STOP" : (busy ? "WAIT" : "REC"),
                recButtonArea.withY (recButtonArea.getBottom() + 4).withHeight (16),
                juce::Justification::centred);

    // ---- lamp + time
    const bool lampOn = recording && std::fmod (recorder.getElapsedSeconds(), 1.0) < 0.6;
    const juce::Rectangle<float> lamp ((float) timeArea.getX(), (float) timeArea.getY() - 18.0f, 10.0f, 10.0f);
    g.setColour (lampOn ? recRed : juce::Colour (0xff3a2422));
    g.fillEllipse (lamp);
    if (lampOn)
    {
        g.setColour (recRed.withAlpha (0.25f));
        g.fillEllipse (lamp.expanded (4.0f));
    }

    g.setColour (recording ? text : dimText);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (recording ? (recorder.isSyncTest() ? "SYNC TEST" : "RECORDING") : "READY",
                (int) lamp.getRight() + 8, (int) lamp.getY() - 3, 160, 16, juce::Justification::centredLeft);

    if (! flash.isVisible())
    {
        g.setColour (recording ? text : dimText);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 46.0f, juce::Font::plain));
        g.drawText (formatTime (recorder.getElapsedSeconds()), timeArea, juce::Justification::centredLeft);
    }

    // ---- FULL / REELS
    const bool modeEnabled = ! recording && ! busy;
    const bool reels = recorder.getMode() == Recorder::Mode::reels;

    auto drawSeg = [&] (juce::Rectangle<int> r, const juce::String& label, bool selected, bool hover)
    {
        const auto rf = r.toFloat().reduced (0.5f);
        if (selected)
        {
            g.setColour (amber.withAlpha (modeEnabled ? 0.9f : 0.45f));
            g.fillRoundedRectangle (rf, 4.0f);
            g.setColour (bg);
        }
        else
        {
            g.setColour (modeEnabled && hover ? amber : line.brighter (0.3f));
            g.drawRoundedRectangle (rf, 4.0f, 1.0f);
            g.setColour (modeEnabled ? dimText : line.brighter (0.3f));
        }
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (label, r, juce::Justification::centred);
    };

    drawSeg (fullArea, "YOUTUBE", ! reels, hoverFull);
    drawSeg (reelsArea, "REELS", reels, hoverReels);

    // ---- mic (side-chain) meter
    {
        const bool connected = recorder.isMicConnected();
        g.setColour (connected ? dimText : line.brighter (0.3f));
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (connected ? "MIC" : "MIC  (side-chain off)", meterArea.withY (meterArea.getY() - 14).withHeight (12),
                    juce::Justification::centredLeft);

        const auto m = meterArea.toFloat();
        g.setColour (juce::Colour (0xff121315));
        g.fillRoundedRectangle (m, 3.0f);
        if (connected && micMeter > 0.0f)
        {
            const auto lvl = m.withWidth (m.getWidth() * juce::jlimit (0.0f, 1.0f, micMeter));
            g.setColour (micMeter > 0.95f ? recRed : juce::Colour (0xff6fbf73));
            g.fillRoundedRectangle (lvl, 3.0f);
        }
        g.setColour (line.brighter (0.2f));
        g.drawRoundedRectangle (m, 3.0f, 1.0f);
    }

    // ---- status + sync
    g.setColour (line);
    g.drawHorizontalLine (getHeight() - 44, 0.0f, (float) getWidth());

    g.setColour (state == Recorder::State::error ? recRed.brighter (0.3f) : dimText);
    g.setFont (juce::FontOptions (12.0f));
    g.drawFittedText (recorder.getStatusText(), 16, getHeight() - 40, getWidth() - 140, 36,
                      juce::Justification::centredLeft, 2);

    const bool syncEnabled = ! recording && ! busy;
    g.setColour (syncEnabled ? (hoverSync ? amber : dimText) : line);
    g.drawRoundedRectangle (syncArea.toFloat().reduced (0.5f), 4.0f, 1.0f);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("SYNC TEST", syncArea, juce::Justification::centred);

    g.setColour (line.brighter (0.4f));
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("offset " + juce::String (recorder.getSyncOffsetMs(), 0) + " ms",
                syncArea.translated (0, -18), juce::Justification::centred);
}

void ScreenRecEditor::mouseMove (const juce::MouseEvent& e)
{
    const bool r = recButtonArea.toFloat().contains (e.position)
                && e.position.getDistanceFrom (recButtonArea.getCentre().toFloat()) < recButtonArea.getWidth() * 0.5f;
    const bool s = syncArea.contains (e.getPosition());
    const bool f = fullArea.contains (e.getPosition());
    const bool rl = reelsArea.contains (e.getPosition());

    if (r != hoverRec || s != hoverSync || f != hoverFull || rl != hoverReels)
    {
        hoverRec = r;
        hoverSync = s;
        hoverFull = f;
        hoverReels = rl;
        repaint();
    }
}

void ScreenRecEditor::mouseUp (const juce::MouseEvent& e)
{
    const auto state = recorder.getState();

    if (e.position.getDistanceFrom (recButtonArea.getCentre().toFloat()) < recButtonArea.getWidth() * 0.5f)
    {
        if (state == Recorder::State::recording)
            recorder.stop();
        else if (state == Recorder::State::idle || state == Recorder::State::error)
            recorder.start (false);
    }
    else if (syncArea.contains (e.getPosition())
             && (state == Recorder::State::idle || state == Recorder::State::error))
    {
        flash.on = false;
        flash.setVisible (true);         // must be on screen before the probe rect is taken
        updateCaptureExclusion (false);  // the test has to see this window
        recorder.start (true, physicalScreenRectOf (flash));
    }
    else if (fullArea.contains (e.getPosition()) && ! recorder.isBusy())
    {
        recorder.setMode (Recorder::Mode::youtube);
    }
    else if (reelsArea.contains (e.getPosition()) && ! recorder.isBusy())
    {
        recorder.setMode (Recorder::Mode::reels);
    }

    repaint();
}

void ScreenRecEditor::timerCallback()
{
    const auto state = recorder.getState();
    const bool testRunning = recorder.isSyncTest()
                          && (state == Recorder::State::starting || state == Recorder::State::recording);

    if (flash.isVisible() != testRunning)
        flash.setVisible (testRunning);

    updateCaptureExclusion (! (recorder.isSyncTest() && recorder.isBusy()));

    // Mic meter: -60..0 dB mapped to 0..1, fast attack, smooth release.
    {
        const float pk = recorder.takeMicLevel();
        const float target = pk > 0.0f ? juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (pk, -60.0f) + 60.0f) / 60.0f) : 0.0f;
        micMeter = target > micMeter ? target : micMeter * 0.85f;
    }

    if (testRunning)
    {
        const bool on = recorder.isFlashOn();
        if (on != flash.on)
        {
            flash.on = on;
            flash.repaint();
        }

        if (getTimerInterval() != 2)
            startTimer (2);   // check the flash schedule every 2 ms

        if (++fastTimerTicks % 16 != 0)
            return;           // repaint the rest of the UI ~30x per second
    }
    else if (getTimerInterval() != 33)
    {
        startTimer (33);
    }

    repaint (getLocalBounds().withTrimmedTop (45));
}
