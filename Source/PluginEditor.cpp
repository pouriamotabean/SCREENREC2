#include "PluginEditor.h"
#include "Timing.h"
#include "FrameOverlay.h"
#include "WinUtils.h"
#include "BinaryData.h"

namespace
{
    // Palette taken from the skin
    const juce::Colour amber      (0xfff2b24a);
    const juce::Colour amberDeep  (0xffd48a22);
    const juce::Colour amberText  (0xfff4c27a);
    const juce::Colour labelText  (0xffcfc6b8);
    const juce::Colour dimText    (0xff8a857c);
    const juce::Colour recRed     (0xffe8402c);

    // Skin geometry (pixels of the 1536x1024 image)
    const juce::Point<float> recCentre  { 237.0f, 368.0f };
    constexpr float recDomeR = 90.0f, recClickR = 160.0f;
    const juce::Rectangle<float> displayInner { 500.0f, 186.0f, 557.0f, 170.0f };
    const juce::Point<float> vuPivot    { 1306.0f, 357.0f };
    const juce::Rectangle<float> vuFace { 1157.0f, 198.0f, 304.0f, 159.0f };
    const juce::Point<float> knobCentre { 1305.0f, 522.0f };
    constexpr float knobR = 64.0f;
    const juce::Rectangle<float> ledArea    { 640.0f, 667.0f, 775.0f, 28.0f };
    const juce::Rectangle<float> statusArea { 90.0f, 846.0f, 970.0f, 52.0f };
    constexpr int ledCount = 40;
    constexpr float knobRange = 2.356f;   // +-135 degrees

    juce::String formatTime (double seconds)
    {
        const int s = (int) seconds;
        return juce::String::formatted ("%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    }

    juce::FontOptions sans (float size, bool bold = true, float tracking = 0.18f)
    {
        return juce::FontOptions (size).withStyle (bold ? "Bold" : "Regular").withKerningFactor (tracking);
    }

    juce::FontOptions mono (float size, bool bold = false)
    {
        return juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, bold ? juce::Font::bold : juce::Font::plain);
    }

    // VU scale printed on the skin: dB -> needle angle (degrees from vertical)
    float vuDegrees (float vu)
    {
        static const float db[]  = { -20.0f, -10.0f, -7.0f, -5.0f, -3.0f, 0.0f, 3.0f };
        static const float deg[] = { -54.0f, -38.6f, -20.8f, 0.0f, 18.6f, 39.7f, 55.0f };

        if (vu <= db[0])  return juce::jmax (-60.0f, deg[0] - (db[0] - vu) * 0.5f);
        if (vu >= db[6])  return juce::jmin (60.0f, deg[6] + (vu - db[6]) * 1.5f);

        for (int i = 0; i < 6; ++i)
            if (vu <= db[i + 1])
                return juce::jmap (vu, db[i], db[i + 1], deg[i], deg[i + 1]);

        return deg[6];
    }

    float knobValueFromDb (float db)   { return db >= 0.0f ? db / 12.0f : db / 36.0f; }
    float knobDbFromValue (float v)    { return v >= 0.0f ? v * 12.0f : v * 36.0f; }
}

//==============================================================================

ScreenRecEditor::ScreenRecEditor (ScreenRecProcessor& p)
    : AudioProcessorEditor (&p), processor (p), recorder (p.recorder)
{
    plate = juce::ImageCache::getFromMemory (BinaryData::plate_jpg, BinaryData::plate_jpgSize);
    knob  = juce::ImageCache::getFromMemory (BinaryData::knob_png,  BinaryData::knob_pngSize);

    areas[aRec]      = { recCentre.x - recClickR, recCentre.y - recClickR, recClickR * 2, recClickR * 2 };
    areas[aYoutube]  = { 487, 402, 190, 52 };
    areas[aReels]    = { 677, 402, 190, 52 };
    areas[aFrame]    = { 903, 402, 200, 52 };
    for (int i = 0; i < 4; ++i)
        areas[aQ480 + i] = { 487.0f + (float) i * 153.0f, 528.0f, 141.0f, 60.0f };
    areas[aMicTest]  = { 1142, 842, 158, 54 };
    areas[aSyncTest] = { 1314, 842, 158, 54 };
    areas[aKnob]     = { knobCentre.x - knobR, knobCentre.y - knobR, knobR * 2, knobR * 2 };
    areas[aPause]    = { 147, 621, 180, 46 };
    areas[aDuck]     = { 50, 689, 170, 44 };
    areas[aClicks]   = { 256, 689, 170, 44 };
    areas[aStatus]   = { 66, 830, 1012, 82 };
    areas[aHelp]     = { 1418, 64, 44, 44 };
    areas[aEye]      = { 1362, 64, 44, 44 };

    addChildComponent (flash);
    flash.setInterceptsMouseClicks (false, false);

    setResizable (true, true);
    setResizeLimits (480, 320, 960, 640);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (1.5);

    const int w = juce::jlimit (480, 960, recorder.getUiWidth());
    setSize (w, juce::roundToInt (w / 1.5));

    recorder.warmUp();

    if (processor.overlay == nullptr)
        processor.overlay = std::make_unique<FrameOverlay> (recorder);
    processor.overlay->setEditorOpen (true);

    recorder.checkForRecovery();

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
            const double s = juce::Component::getApproximateScaleFactorForComponent (this);
            const int maxW = (int) ((getWidth() + 400) * s), maxH = (int) ((getHeight() + 400) * s);

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
    scale = (float) getWidth() / skinW;
    plateCache = {};   // rebuilt at the right size on the next paint

    flash.setBounds ((displayInner.reduced (6.0f) * scale).toNearestInt());

    if (recorder.getUiWidth() != getWidth())
    {
        recorder.setUiWidth (getWidth());
        saveSizeTicks = 20;   // save after the drag settles
    }
}

int ScreenRecEditor::areaAt (juce::Point<float> p) const
{
    if (p.getDistanceFrom (recCentre) < recClickR)
        return aRec;
    if (p.getDistanceFrom (knobCentre) < knobR)
        return aKnob;

    for (int i = 1; i < aCount; ++i)
        if (i != aKnob && areas[i].contains (p))
            return i;

    return -1;
}

juce::Rectangle<int> ScreenRecEditor::physicalScreenRectOf (const juce::Component& c) const
{
    const auto logical = c.getScreenBounds();
    auto physical = juce::Desktop::getInstance().getDisplays().logicalToPhysical (logical);
    // Sample only the middle of the panel so small coordinate errors don't matter.
    return physical.reduced (physical.getWidth() / 4, physical.getHeight() / 4);
}

float ScreenRecEditor::knobAngle() const
{
    return juce::jlimit (-1.0f, 1.0f, knobValueFromDb (recorder.getMicGainDb())) * knobRange;
}

void ScreenRecEditor::setKnobFromDb (float db)
{
    db = juce::jlimit (-36.0f, 12.0f, db);
    if (std::abs (db) < 0.25f)
        db = 0.0f;   // gentle detent at 0 dB

    recorder.setMicGainDb (db);
    showTemporaryStatus (db <= -35.9f ? juce::String ("Mic level: muted")
                                      : "Mic level: " + juce::String (db > 0 ? "+" : "") + juce::String (db, 1) + " dB");
    repaint();
}

void ScreenRecEditor::showTemporaryStatus (const juce::String& s)
{
    tempStatus = s;
    tempStatusTicks = 60;   // ~2 s
}

//==============================================================================
// Drawing helpers

void ScreenRecEditor::drawButton (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& label,
                                  bool selected, bool enabled, bool hovered, float fontSize) const
{
    const float rad = 5.0f;

    // recess
    g.setColour (juce::Colours::black.withAlpha (0.8f));
    g.fillRoundedRectangle (r.expanded (3.0f), rad + 2.0f);

    if (selected)
    {
        juce::ColourGradient grad (amber.brighter (0.1f), r.getX(), r.getY(), amberDeep, r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r, rad);

        g.setColour (amber.withAlpha (0.18f));
        g.drawRoundedRectangle (r.expanded (5.0f), rad + 4.0f, 4.0f);   // glow

        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawLine (r.getX() + rad, r.getY() + 1.5f, r.getRight() - rad, r.getY() + 1.5f, 1.0f);

        g.setColour (juce::Colour (0xff1c1206).withAlpha (enabled ? 1.0f : 0.6f));
    }
    else
    {
        juce::ColourGradient grad (juce::Colour (0xff222222), r.getX(), r.getY(), juce::Colour (0xff121212), r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r, rad);

        g.setColour ((hovered && enabled ? amber.withAlpha (0.6f) : juce::Colour (0xff3a3a3a)));
        g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.5f);

        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawLine (r.getX() + rad, r.getY() + 2.0f, r.getRight() - rad, r.getY() + 2.0f, 1.0f);

        g.setColour (enabled ? labelText.withAlpha (hovered ? 1.0f : 0.85f) : labelText.withAlpha (0.3f));
    }

    g.setFont (sans (fontSize));
    g.drawText (label, r, juce::Justification::centred);
}

void ScreenRecEditor::drawMetalButton (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& label,
                                       bool active, bool enabled, bool hovered) const
{
    // dark bezel
    g.setColour (juce::Colour (0xff0b0b0b));
    g.fillRoundedRectangle (r.expanded (6.0f), 9.0f);
    g.setColour (juce::Colour (0xff3c3c3c));
    g.drawRoundedRectangle (r.expanded (6.0f), 9.0f, 1.0f);

    // brushed metal cap
    juce::ColourGradient grad (juce::Colour (0xffc9c6bf), r.getX(), r.getY(), juce::Colour (0xff8b8882), r.getX(), r.getBottom(), false);
    grad.addColour (0.5, juce::Colour (0xffaaa7a0));
    g.setGradientFill (grad);
    g.fillRoundedRectangle (r, 5.0f);

    for (float y = r.getY() + 3.0f; y < r.getBottom() - 2.0f; y += 3.0f)   // brushing
    {
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawHorizontalLine ((int) y, r.getX() + 3.0f, r.getRight() - 3.0f);
    }

    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (r, 5.0f, 1.5f);

    if (hovered && enabled)
    {
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.fillRoundedRectangle (r, 5.0f);
    }

    // LED
    const juce::Point<float> led (r.getX() + 24.0f, r.getCentreY());
    if (active)
    {
        g.setColour (amber.withAlpha (0.35f));
        g.fillEllipse (led.x - 12.0f, led.y - 12.0f, 24.0f, 24.0f);
        g.setColour (amber.brighter (0.3f));
    }
    else
    {
        g.setColour (juce::Colour (0xff1a1a1a));
    }
    g.fillEllipse (led.x - 7.0f, led.y - 7.0f, 14.0f, 14.0f);

    g.setColour (juce::Colour (0xff1b1b1b).withAlpha (enabled ? 1.0f : 0.45f));
    g.setFont (sans (21.0f, true, 0.14f));
    g.drawText (label, r.withTrimmedLeft (42.0f).withTrimmedRight (8.0f), juce::Justification::centred);
}

//==============================================================================

void ScreenRecEditor::paint (juce::Graphics& g)
{
    // ---- static skin, cached at the screen's real pixel size
    const float pixelScale = scale * (float) juce::Component::getApproximateScaleFactorForComponent (this);
    const int cw = juce::roundToInt (skinW * pixelScale), ch = juce::roundToInt (skinH * pixelScale);

    if (plate.isValid() && (plateCache.getWidth() != cw || plateCache.getHeight() != ch))
        plateCache = plate.rescaled (juce::jmax (1, cw), juce::jmax (1, ch), juce::Graphics::highResamplingQuality);

    if (plateCache.isValid())
        g.drawImage (plateCache, getLocalBounds().toFloat());
    else
        g.fillAll (juce::Colour (0xff141414));

    g.addTransform (juce::AffineTransform::scale (scale));

    const auto state = recorder.getState();
    const bool recording = state == Recorder::State::recording;
    const bool busy = state == Recorder::State::starting || state == Recorder::State::saving;
    const bool idle = ! recording && ! busy;
    const bool reels = recorder.getMode() == Recorder::Mode::reels;

    // ---- REC dome: dim when idle, glowing when recording
    {
        const auto dome = juce::Rectangle<float> (recDomeR * 2, recDomeR * 2).withCentre (recCentre);
        if (recording)
        {
            const float pulse = 0.5f + 0.5f * std::sin ((float) recorder.getElapsedSeconds() * 4.0f);
            juce::ColourGradient glow (juce::Colour (0xffff7a50).withAlpha (0.55f + 0.2f * pulse), recCentre.x - 20, recCentre.y - 25,
                                       juce::Colour (0x00ff2000), recCentre.x + recDomeR, recCentre.y + recDomeR, true);
            g.setGradientFill (glow);
            g.fillEllipse (dome);
            g.setColour (recRed.withAlpha (0.15f + 0.1f * pulse));
            g.drawEllipse (dome.expanded (10.0f), 10.0f);
        }
        else
        {
            g.setColour (juce::Colours::black.withAlpha (busy ? 0.55f : (hover == aRec ? 0.1f : 0.3f)));
            g.fillEllipse (dome.reduced (2.0f));
        }
    }

    // ---- display: lamp, state, timecode
    {
        const bool lampOn = recording && std::fmod (recorder.getElapsedSeconds(), 1.0) < 0.6;
        const juce::Point<float> lamp (displayInner.getX() + 32.0f, displayInner.getY() + 34.0f);
        if (lampOn)
        {
            g.setColour (recRed.withAlpha (0.35f));
            g.fillEllipse (lamp.x - 16, lamp.y - 16, 32, 32);
        }
        g.setColour (lampOn ? recRed.brighter (0.2f) : juce::Colour (0xff3a1612));
        g.fillEllipse (lamp.x - 9, lamp.y - 9, 18, 18);

        const int countdown = recorder.getCountdown();
        const bool isPaused = recording && recorder.isPaused();
        const juce::String label = recording ? (recorder.isMicTest() ? "MIC TEST" : recorder.isSyncTest() ? "SYNC TEST" : isPaused ? "PAUSED" : "RECORDING")
                                             : countdown > 0 ? "GET READY"
                                             : (busy ? (state == Recorder::State::saving ? "SAVING" : "STARTING") : "READY");
        g.setColour (amberText.withAlpha (recording || busy ? 0.95f : 0.45f));
        g.setFont (sans (22.0f, true, 0.3f));
        g.drawText (label, (int) lamp.x + 30, (int) lamp.y - 14, 300, 28, juce::Justification::centredLeft);

        g.setColour (amberText.withAlpha (0.25f));
        g.drawHorizontalLine ((int) lamp.y, lamp.x + 260.0f, displayInner.getRight() - 30.0f);

        if (! flash.isVisible())
        {
            const auto tc = juce::Rectangle<float> (displayInner.getX() + 24, displayInner.getY() + 62, displayInner.getWidth() - 40, 100);
            const auto text = countdown > 0 ? juce::String ("- ") + juce::String (countdown) + " -"
                                            : formatTime (recorder.getElapsedSeconds());
            const bool blinkOff = isPaused && std::fmod (juce::Time::getMillisecondCounterHiRes() / 1000.0, 1.0) > 0.55;
            const float a = countdown > 0 ? 1.0f : recording ? (blinkOff ? 0.25f : 1.0f) : 0.35f;
            g.setFont (mono (96.0f, true));

            g.setColour (amberText.withAlpha (0.12f * a));          // soft glow
            for (auto off : { juce::Point<float> (-2, 0), juce::Point<float> (2, 0), juce::Point<float> (0, -2), juce::Point<float> (0, 2) })
                g.drawText (text, tc.translated (off.x, off.y), juce::Justification::centredLeft);

            g.setColour (amberText.withAlpha (a));
            g.drawText (text, tc, juce::Justification::centredLeft);
        }
    }

    // ---- mode + frame + quality
    drawButton (g, areas[aYoutube], "YOUTUBE", ! reels, idle, hover == aYoutube, 24.0f);
    drawButton (g, areas[aReels], "REELS", reels, idle, hover == aReels, 24.0f);

    {
        const bool on = recorder.isFrameVisible();
        const juce::Rectangle<float> box (areas[aFrame].getX() + 4, areas[aFrame].getCentreY() - 19, 38, 38);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRoundedRectangle (box, 5.0f);
        g.setColour (hover == aFrame ? amber : amber.withAlpha (0.8f));
        g.drawRoundedRectangle (box, 5.0f, 2.0f);
        if (on)
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + 9, box.getCentreY() + 1);
            tick.lineTo (box.getX() + 16, box.getBottom() - 10);
            tick.lineTo (box.getRight() - 8, box.getY() + 10);
            g.setColour (amber);
            g.strokePath (tick, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (labelText.withAlpha (on ? 0.95f : 0.6f));
        g.setFont (sans (22.0f, false, 0.22f));
        g.drawText ("SHOW FRAME", areas[aFrame].withTrimmedLeft (58), juce::Justification::centredLeft);
    }

    const int q = recorder.effectiveQuality (recorder.getMode());
    const char* qNames[] = { "480", "720", "1080", "4K" };
    for (int i = 0; i < 4; ++i)
        drawButton (g, areas[aQ480 + i], qNames[i], q == i, idle && ! (i == 3 && reels), hover == aQ480 + i, 26.0f);

    // ---- VU needle (mic)
    {
        const float ang = vuAngle;
        const float len = 132.0f;
        const juce::Point<float> tip (vuPivot.x + std::sin (ang) * len, vuPivot.y - std::cos (ang) * len);

        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (vuFace.toNearestInt());
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.drawLine ({ vuPivot.translated (3, 3), tip.translated (3, 3) }, 2.5f);
        g.setColour (juce::Colour (0xff141414));
        g.drawLine ({ vuPivot, tip }, 2.0f);
        g.setColour (juce::Colour (0xff1e1e1e));
        g.fillEllipse (vuPivot.x - 9, vuPivot.y - 9, 18, 18);

        if (! recorder.isMicConnected())
        {
            g.setColour (juce::Colour (0xff6a5a40).withAlpha (0.8f));
            g.setFont (sans (13.0f, true, 0.2f));
            g.drawText ("SIDE-CHAIN OFF", vuFace.withTrimmedTop (118).withTrimmedBottom (18), juce::Justification::centred);
        }
    }

    // ---- knob (mic level in the video)
    if (knob.isValid())
    {
        const float kw = (float) knob.getWidth(), kh = (float) knob.getHeight();
        g.drawImageTransformed (knob, juce::AffineTransform::translation (-kw * 0.5f, -kh * 0.5f)
                                          .rotated (knobAngle())
                                          .translated (knobCentre.x, knobCentre.y), false);
    }

    // ---- master LED meter
    {
        g.setColour (labelText.withAlpha (0.85f));
        g.setFont (sans (21.0f, false, 0.22f));
        g.drawText ("MASTER", juce::Rectangle<float> (462, 664, 120, 34), juce::Justification::centredRight);

        const float gap = 5.0f;
        const float segW = (ledArea.getWidth() - gap * (ledCount - 1)) / ledCount;
        const int lit = juce::roundToInt (masterMeter * ledCount);
        const int peak = juce::roundToInt (masterPeakHold * ledCount);

        for (int i = 0; i < ledCount; ++i)
        {
            const float f = (float) i / ledCount;
            const juce::Colour c = f < 0.6f ? juce::Colour (0xff3fcf55) : f < 0.78f ? juce::Colour (0xffc8d83a)
                                 : f < 0.9f ? juce::Colour (0xfff0a030) : juce::Colour (0xffe8402c);
            const juce::Rectangle<float> seg (ledArea.getX() + i * (segW + gap), ledArea.getY(), segW, ledArea.getHeight());
            const bool on = i < lit || (i == peak - 1 && peak > 0);

            if (on)
            {
                g.setColour (c.withAlpha (0.25f));
                g.fillRoundedRectangle (seg.expanded (2.0f, 3.0f), 3.0f);
                g.setColour (c);
            }
            else
            {
                g.setColour (c.withMultipliedBrightness (0.18f).withMultipliedSaturation (0.6f));
            }
            g.fillRoundedRectangle (seg, 2.0f);
        }
    }

    // ---- offsets
    g.setColour (dimText);
    g.setFont (mono (22.0f));
    g.drawText ("sync " + juce::String (recorder.getSyncOffsetMs(), 0) + " ms  |  mic " + juce::String (recorder.getMicOffsetMs(), 0) + " ms",
                juce::Rectangle<float> (1060, 792, 402, 30), juce::Justification::centredRight);

    // ---- status
    {
        const bool err = state == Recorder::State::error;
        const juce::String s = tempStatusTicks > 0 ? tempStatus : recorder.getStatusText();
        g.setColour (err && tempStatusTicks <= 0 ? recRed.brighter (0.3f) : juce::Colour (0xffb3ada3));
        g.setFont (mono (24.0f));
        g.drawFittedText (s, statusArea.toNearestInt(), juce::Justification::centredLeft, 2, 0.8f);
    }

    // ---- pause / duck / clicks (left column)
    {
        const bool canPause = recording && ! recorder.isMicTest() && ! recorder.isSyncTest();
        const bool isPaused = recording && recorder.isPaused();
        const bool blink = isPaused && std::fmod (juce::Time::getMillisecondCounterHiRes() / 1000.0, 1.0) < 0.55;
        drawMetalButton (g, areas[aPause], isPaused ? "RESUME" : "PAUSE", blink, canPause, hover == aPause);
        drawMetalButton (g, areas[aDuck], "DUCK", recorder.isDuckEnabled(), true, hover == aDuck);
        drawMetalButton (g, areas[aClicks], "CLICKS", recorder.isClickEnabled(), true, hover == aClicks);
    }

    // ---- tests
    drawMetalButton (g, areas[aMicTest], "MIC TEST", recorder.isMicTest() && recording, idle, hover == aMicTest);
    drawMetalButton (g, areas[aSyncTest], "SYNC TEST", recorder.isSyncTest() && recording, idle, hover == aSyncTest);

    // ---- help button: engraved "?" on the steel header
    {
        const auto r = areas[aHelp];
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillEllipse (r.translated (0, 1.5f));
        juce::ColourGradient grad (juce::Colour (0xff3a3936), r.getX(), r.getY(), juce::Colour (0xff161616), r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillEllipse (r.reduced (1.0f));
        g.setColour ((hover == aHelp || helpOpen) ? amber : juce::Colour (0xffcfc6b8).withAlpha (0.85f));
        g.drawEllipse (r.reduced (1.5f), 1.5f);
        g.setFont (sans (26.0f, true, 0.0f));
        g.drawText ("?", r.translated (0, -1.0f), juce::Justification::centred);
    }

    // ---- eye: show this window in the video
    {
        const auto r = areas[aEye];
        const bool on = processor.showPluginInVideo;
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillEllipse (r.translated (0, 1.5f));
        juce::ColourGradient grad (juce::Colour (0xff3a3936), r.getX(), r.getY(), juce::Colour (0xff161616), r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillEllipse (r.reduced (1.0f));

        if (on)
        {
            g.setColour (amber.withAlpha (0.22f));
            g.fillEllipse (r.expanded (5.0f));
        }

        const auto c = r.getCentre();
        const juce::Colour ink = on ? amber : (hover == aEye ? amber.withAlpha (0.8f) : juce::Colour (0xffcfc6b8).withAlpha (0.85f));
        g.setColour (ink);
        g.drawEllipse (r.reduced (1.5f), 1.5f);

        juce::Path eye;
        eye.startNewSubPath (c.x - 13, c.y);
        eye.quadraticTo (c.x, c.y - 11, c.x + 13, c.y);
        eye.quadraticTo (c.x, c.y + 11, c.x - 13, c.y);
        eye.closeSubPath();
        g.strokePath (eye, juce::PathStrokeType (2.0f));
        g.fillEllipse (c.x - 4.0f, c.y - 4.0f, 8.0f, 8.0f);

        if (! on)
            g.drawLine (c.x - 12, c.y + 10, c.x + 12, c.y - 10, 2.0f);   // crossed out
    }

    if (helpOpen)
        drawHelp (g);
}

void ScreenRecEditor::drawHelp (juce::Graphics& g) const
{
    struct Line { const char* text; int kind; };   // 0 = title, 1 = heading, 2 = body, 3 = gap
    static const Line lines[] = {
        { "SCREENREC  -  QUICK GUIDE", 0 }, { "", 3 },
        { "RECORD", 1 },
        { "Put ScreenRec last on Stereo Out. REC starts after 3-2-1, REC again stops.", 2 },
        { "Files go to Documents\\ScreenRec. Click the status bar to open the last one.", 2 },
        { "FRAME", 1 },
        { "YOUTUBE 16:9 / REELS 9:16. Top-left handle: move (wheel = zoom,", 2 },
        { "double-click = default). Top-right handle: zoom. Works while recording.", 2 },
        { "The frame and this window never appear in the video.", 2 },
        { "MIC  (no headphone delay)", 1 },
        { "Turn on ScreenRec's side-chain. On the mic track: Send -> ScreenRec", 2 },
        { "Side-Chain, output -> No Bus. Knob = mic level in the video.", 2 },
        { "VU = mic, LEDs = master.", 2 },
        { "SYNC  (once, transport stopped)", 1 },
        { "SYNC TEST: keep this window visible, wait ~15 s.", 2 },
        { "MIC TEST: hold headphones against the mic, stay quiet ~12 s.", 2 },
        { "Repeat after changing master plugins or the ASIO buffer.", 2 },
        { "EXTRAS", 1 },
        { "PAUSE: left out of the file.   DUCK: music -10 dB under your voice.", 2 },
        { "CLICKS: ripples on clicks.   Crashed recordings are recovered on reopen.", 2 },
        { "EYE (top): show this window in the video, for tutorials about ScreenRec.", 2 },
    };

    // dim everything, then a dark glass card
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRect (juce::Rectangle<float> (0, 0, skinW, skinH));

    const juce::Rectangle<float> card (150, 100, 1236, 862);
    g.setColour (juce::Colour (0xf20d0d0d));
    g.fillRoundedRectangle (card, 14.0f);
    g.setColour (amber.withAlpha (0.7f));
    g.drawRoundedRectangle (card.reduced (1.0f), 14.0f, 2.0f);

    // close mark
    g.setColour (labelText.withAlpha (0.7f));
    const auto x = juce::Rectangle<float> (card.getRight() - 58, card.getY() + 22, 30, 30);
    g.drawLine (x.getX(), x.getY(), x.getRight(), x.getBottom(), 2.5f);
    g.drawLine (x.getX(), x.getBottom(), x.getRight(), x.getY(), 2.5f);

    float y = card.getY() + 34.0f;
    const float left = card.getX() + 56.0f, width = card.getWidth() - 112.0f;

    for (const auto& l : lines)
    {
        switch (l.kind)
        {
            case 0:
                g.setColour (amber);
                g.setFont (sans (30.0f, true, 0.2f));
                g.drawText (l.text, juce::Rectangle<float> (left, y, width, 38), juce::Justification::centredLeft);
                y += 38.0f;
                break;
            case 1:
                y += 10.0f;
                g.setColour (amber.withAlpha (0.95f));
                g.setFont (sans (21.0f, true, 0.22f));
                g.drawText (l.text, juce::Rectangle<float> (left, y, width, 30), juce::Justification::centredLeft);
                y += 32.0f;
                break;
            case 2:
                g.setColour (labelText.withAlpha (0.92f));
                g.setFont (sans (22.0f, false, 0.02f));
                g.drawText (l.text, juce::Rectangle<float> (left + 18.0f, y, width - 18.0f, 30), juce::Justification::centredLeft);
                y += 30.0f;
                break;
            default:
                y += 4.0f;
                break;
        }
    }

    g.setColour (dimText);
    g.setFont (sans (15.0f, false, 0.2f));
    g.drawText ("CLICK ANYWHERE TO CLOSE", juce::Rectangle<float> (card.getX(), card.getBottom() - 44, card.getWidth(), 30), juce::Justification::centred);
}

//==============================================================================
// Mouse

void ScreenRecEditor::mouseMove (const juce::MouseEvent& e)
{
    const int h = helpOpen ? -1 : areaAt (toSkin (e.position));
    if (h != hover)
    {
        hover = h;
        setMouseCursor (h >= 0 ? (h == aKnob ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::PointingHandCursor)
                               : juce::MouseCursor::NormalCursor);

        repaint();
    }
}

void ScreenRecEditor::mouseExit (const juce::MouseEvent&)
{
    if (hover != -1)
    {
        hover = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void ScreenRecEditor::mouseDown (const juce::MouseEvent& e)
{
    draggingKnob = ! helpOpen && areaAt (toSkin (e.position)) == aKnob;
    dragStartDb = recorder.getMicGainDb();
}

void ScreenRecEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingKnob)
        return;

    const float dy = (float) e.getDistanceFromDragStartY() / scale;     // skin pixels
    const float speed = e.mods.isShiftDown() ? 0.0015f : 0.006f;
    setKnobFromDb (knobDbFromValue (juce::jlimit (-1.0f, 1.0f, knobValueFromDb (dragStartDb) - dy * speed)));
}

void ScreenRecEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! helpOpen && areaAt (toSkin (e.position)) == aKnob)
        setKnobFromDb (0.0f);
}

void ScreenRecEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (! helpOpen && areaAt (toSkin (e.position)) == aKnob)
    {
        const float v = knobValueFromDb (recorder.getMicGainDb()) + w.deltaY * 0.2f;
        setKnobFromDb (knobDbFromValue (juce::jlimit (-1.0f, 1.0f, v)));
    }
}

void ScreenRecEditor::mouseUp (const juce::MouseEvent& e)
{
    if (draggingKnob)
    {
        draggingKnob = false;
        recorder.saveSettings();
        return;
    }

    if (helpOpen)   // any click closes the guide
    {
        helpOpen = false;
        repaint();
        return;
    }

    const auto state = recorder.getState();
    const bool idle = state == Recorder::State::idle || state == Recorder::State::error;
    const bool reels = recorder.getMode() == Recorder::Mode::reels;

    switch (areaAt (toSkin (e.position)))
    {
        case aHelp:    helpOpen = true; break;
        case aEye:
            processor.showPluginInVideo = ! processor.showPluginInVideo;
            showTemporaryStatus (processor.showPluginInVideo ? "This window WILL appear in the video"
                                                             : "This window is hidden from the video");
            break;
        case aRec:
            if (state == Recorder::State::recording)       recorder.stop();
            else if (state == Recorder::State::starting)   recorder.stop();   // cancels the countdown
            else if (idle)                                 recorder.start (false);
            break;

        case aPause:   recorder.togglePause(); break;
        case aDuck:    recorder.setDuckEnabled (! recorder.isDuckEnabled()); break;
        case aClicks:  recorder.setClickEnabled (! recorder.isClickEnabled()); break;
        case aStatus:  if (state != Recorder::State::saving) recorder.revealLastOutput(); break;

        case aSyncTest:
            if (idle)
            {
                flash.on = false;
                flash.setVisible (true);         // must be on screen before the probe rect is taken
                updateCaptureExclusion (false);  // the test has to see this window
                recorder.start (true, physicalScreenRectOf (flash));
            }
            break;

        case aMicTest:
            if (idle)
                recorder.start (false, {}, true);
            break;

        case aYoutube: if (idle) recorder.setMode (Recorder::Mode::youtube); break;
        case aReels:   if (idle) recorder.setMode (Recorder::Mode::reels);   break;
        case aFrame:   recorder.setFrameVisible (! recorder.isFrameVisible()); break;
        case aQ480:    if (idle) recorder.setQuality (0); break;
        case aQ720:    if (idle) recorder.setQuality (1); break;
        case aQ1080:   if (idle) recorder.setQuality (2); break;
        case aQ4k:     if (idle && ! reels) recorder.setQuality (3); break;
        default: break;
    }

    repaint();
}

//==============================================================================

void ScreenRecEditor::timerCallback()
{
    const auto state = recorder.getState();
    const bool testRunning = recorder.isSyncTest()
                          && (state == Recorder::State::starting || state == Recorder::State::recording);

    if (flash.isVisible() != testRunning)
        flash.setVisible (testRunning);

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
            return;           // the rest runs ~30x per second
    }
    else if (getTimerInterval() != 33)
    {
        startTimer (33);
    }

    updateCaptureExclusion (! (recorder.isSyncTest() && recorder.isBusy()) && ! processor.showPluginInVideo);

    // ---- VU (mic, after the knob): peak dBFS + 12 -> VU scale, ~300 ms ballistics
    {
        const float pk = recorder.takeMicLevel();
        const float db = pk > 0.0f ? juce::Decibels::gainToDecibels (pk, -60.0f) : -60.0f;
        vuDb += (db - vuDb) * (db > vuDb ? 0.45f : 0.18f);
        const float target = juce::degreesToRadians (vuDegrees (vuDb + 12.0f));
        vuAngle += (target - vuAngle) * 0.35f;
    }

    // ---- master LEDs: -48..0 dBFS, fast attack, smooth release, peak hold
    {
        const float pk = recorder.takeMasterLevel();
        const float v = pk > 0.0f ? juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (pk, -48.0f) + 48.0f) / 48.0f) : 0.0f;
        masterMeter = v > masterMeter ? v : masterMeter * 0.88f;

        if (v >= masterPeakHold) { masterPeakHold = v; peakHoldTicks = 30; }
        else if (--peakHoldTicks <= 0) masterPeakHold = juce::jmax (0.0f, masterPeakHold - 0.02f);
    }

    if (tempStatusTicks > 0)
        --tempStatusTicks;

    if (saveSizeTicks > 0 && --saveSizeTicks == 0)
        recorder.saveSettings();

    repaint();
}
