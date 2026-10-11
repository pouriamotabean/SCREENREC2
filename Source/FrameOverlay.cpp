#include "FrameOverlay.h"
#include "WinUtils.h"
#include <cmath>

namespace
{
    const juce::Colour amber  (0xffe0a040);
    const juce::Colour recRed (0xffe0342c);
    constexpr int topReserve = 40;   // logical px kept free above the default frame (title bar + handles)
}

FrameOverlay::FrameOverlay (Recorder& r) : recorder (r)
{
    frame.setOpaque (false);
    frame.setInterceptsMouseClicks (false, false);
    frame.setAlwaysOnTop (true);

    for (auto* h : { &moveHandle, &zoomHandle })
    {
        h->setOpaque (false);
        h->setAlwaysOnTop (true);
        h->setWantsKeyboardFocus (false);
        h->setMouseClickGrabsKeyboardFocus (false);
        h->setSize (handleSize, handleSize);
    }

    moveHandle.setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    zoomHandle.setMouseCursor (juce::MouseCursor::UpDownLeftRightResizeCursor);

    startTimerHz (30);
}

FrameOverlay::~FrameOverlay()
{
    stopTimer();
    hide();
}

//==============================================================================

juce::Rectangle<int> FrameOverlay::screenArea() const
{
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        return d->totalArea;

    return { 0, 0, 1920, 1080 };
}

double FrameOverlay::minHeightFraction() const
{
    double physicalH = 1080.0;
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        physicalH = d->totalArea.getHeight() * d->scale;

    // Smallest frame: a third of the screen height (but at least 540 px),
    // so the output never gets too soft.
    return juce::jlimit (0.1, 1.0, juce::jmax (physicalH / 3.0, 540.0) / physicalH);
}

void FrameOverlay::getFrame (double& cx, double& cy, double& h) const
{
    recorder.getFrame (mode(), cx, cy, h);
}

juce::Rectangle<int> FrameOverlay::frameRectLogical() const
{
    double cx, cy, h;
    getFrame (cx, cy, h);
    if (h <= 0.0)
        h = 1.0;

    const auto a = screenArea().toDouble();
    const double hh = h * a.getHeight();
    const double ww = hh * aspect();

    return juce::Rectangle<double> (a.getX() + cx * a.getWidth() - ww * 0.5,
                                    a.getY() + cy * a.getHeight() - hh * 0.5, ww, hh)
               .getSmallestIntegerContainer();
}

void FrameOverlay::setFrame (double cx, double cy, double h)
{
    const auto a = screenArea().toDouble();
    const double maxH = juce::jmin (1.0, a.getWidth() / aspect() / a.getHeight());

    h = juce::jlimit (juce::jmin (minHeightFraction(), maxH), maxH, h);
    const double wf = h * a.getHeight() * aspect() / a.getWidth();   // width as fraction of screen width

    cx = juce::jlimit (wf * 0.5, 1.0 - wf * 0.5, cx);
    cy = juce::jlimit (h * 0.5, 1.0 - h * 0.5, cy);

    recorder.setFrame (mode(), cx, cy, h);
    layout();
}

void FrameOverlay::resetToDefault()
{
    const auto a = screenArea().toDouble();
    auto band = a;

    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        band = d->userArea.toDouble();          // without the taskbar

    band = band.withTrimmedTop ((double) topReserve).withTrimmedBottom ((double) border + 2.0);

    double h = band.getHeight();
    if (h * aspect() > band.getWidth() - 2.0 * (handleSize + border + 2))
        h = (band.getWidth() - 2.0 * (handleSize + border + 2)) / aspect();

    const auto c = band.getCentre();
    setFrame ((c.x - a.getX()) / a.getWidth(), (c.y - a.getY()) / a.getHeight(), h / a.getHeight());
    recorder.saveSettings();
}

//==============================================================================

void FrameOverlay::show()
{
    if (shown)
        return;

    // Make sure the stored frame fits this screen (or create the default one).
    double cx, cy, h;
    getFrame (cx, cy, h);
    if (h <= 0.0)
        resetToDefault();
    else
        setFrame (cx, cy, h);
    lastMode = (int) mode();

    frame.addToDesktop (juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresMouseClicks);
    frame.setVisible (true);

    moveHandle.addToDesktop (juce::ComponentPeer::windowIsTemporary);
    zoomHandle.addToDesktop (juce::ComponentPeer::windowIsTemporary);
    moveHandle.setVisible (true);
    zoomHandle.setVisible (true);

    excludedOk = true;

    if (auto* peer = frame.getPeer())
    {
        WinUtils::makeClickThrough (peer->getNativeHandle());
        excludedOk &= WinUtils::setExcludedFromCapture (peer->getNativeHandle(), true);
    }

    for (auto* hd : { &moveHandle, &zoomHandle })
        if (auto* peer = hd->getPeer())
        {
            WinUtils::makeNoActivate (peer->getNativeHandle());
            excludedOk &= WinUtils::setExcludedFromCapture (peer->getNativeHandle(), true);
        }

    shown = true;
    layout();
    moveHandle.toFront (false);
    zoomHandle.toFront (false);
}

void FrameOverlay::hide()
{
    if (! shown)
        return;

    for (juce::Component* c : { (juce::Component*) &moveHandle, (juce::Component*) &zoomHandle, (juce::Component*) &frame })
    {
        c->setVisible (false);
        if (c->isOnDesktop())
            c->removeFromDesktop();
    }

    shown = false;
}

void FrameOverlay::layout()
{
    const auto r = frameRectLogical();
    const auto area = screenArea();
    lastFrameRect = r;

    frame.setBounds (r.expanded (border));
    frame.repaint();

    const int hs = handleSize, gap = border + 2;

    juce::Rectangle<int> tl (r.getX() - hs - gap, r.getY() - hs - gap, hs, hs);
    if (! area.contains (tl))
        tl = { r.getX() + gap, r.getY() + gap, hs, hs };

    juce::Rectangle<int> tr (r.getRight() + gap, r.getY() - hs - gap, hs, hs);
    if (! area.contains (tr))
        tr = { r.getRight() - hs - gap, r.getY() + gap, hs, hs };

    moveHandle.setBounds (tl);
    zoomHandle.setBounds (tr);
}

void FrameOverlay::timerCallback()
{
    const bool syncTestRunning = recorder.isSyncTest() && recorder.isBusy();
    const bool want = ! syncTestRunning && (editorOpen || recorder.isBusy());

    if (want && ! shown)  show();
    if (! want && shown)  hide();

    if (! shown)
        return;

    const bool rec = recorder.getState() == Recorder::State::recording;
    if (rec != lastRecording)
    {
        lastRecording = rec;
        frame.recording = rec;
        frame.repaint();
    }

    if ((int) mode() != lastMode)              // YOUTUBE <-> REELS
    {
        lastMode = (int) mode();
        double cx, cy, h;
        getFrame (cx, cy, h);
        if (h <= 0.0)
            resetToDefault();
        else
            setFrame (cx, cy, h);
    }

    if (frameRectLogical() != lastFrameRect)   // screen layout changed
        layout();
}

//==============================================================================

void FrameOverlay::FrameWindow::paint (juce::Graphics& g)
{
    const auto inner = getLocalBounds().toFloat().reduced ((float) border);
    const auto colour = recording ? recRed : amber;

    // Outline sits just outside the captured area.
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.drawRect (inner.expanded (4.0f), 1.0f);
    g.setColour (colour.withAlpha (0.95f));
    g.drawRect (inner.expanded (3.0f), 2.0f);

    // Corner ticks (in the border zone, outside the captured area).
    const auto o = getLocalBounds().toFloat();
    const float t = juce::jmin (40.0f, inner.getWidth() * 0.15f), k = (float) border;
    g.setColour (colour);
    g.fillRect (o.getX(), o.getY(), t, k);                      g.fillRect (o.getX(), o.getY(), k, t);
    g.fillRect (o.getRight() - t, o.getY(), t, k);              g.fillRect (o.getRight() - k, o.getY(), k, t);
    g.fillRect (o.getX(), o.getBottom() - k, t, k);             g.fillRect (o.getX(), o.getBottom() - t, k, t);
    g.fillRect (o.getRight() - t, o.getBottom() - k, t, k);     g.fillRect (o.getRight() - k, o.getBottom() - t, k, t);

    // Centre marker: soft translucent white circle with a fine crosshair.
    const auto c = inner.getCentre();
    const float rad = 20.0f;
    g.setColour (juce::Colours::white.withAlpha (0.16f));
    g.fillEllipse (c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.drawEllipse (c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f, 1.5f);
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.drawLine (c.x - 7.0f, c.y, c.x + 7.0f, c.y, 1.0f);
    g.drawLine (c.x, c.y - 7.0f, c.x, c.y + 7.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.fillEllipse (c.x - 2.0f, c.y - 2.0f, 4.0f, 4.0f);
}

void FrameOverlay::Handle::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (juce::Colour (0xff1b1c1f).withAlpha (0.9f));
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (amber.withAlpha (hover ? 1.0f : 0.8f));
    g.drawRoundedRectangle (b.reduced (1.0f), 6.0f, 2.0f);

    const auto c = b.getCentre();
    const float s = b.getWidth() * 0.3f;
    juce::Path p;

    if (kind == Kind::move)
    {
        p.addArrow ({ c.x, c.y, c.x - s, c.y }, 1.6f, 5.0f, 4.0f);
        p.addArrow ({ c.x, c.y, c.x + s, c.y }, 1.6f, 5.0f, 4.0f);
        p.addArrow ({ c.x, c.y, c.x, c.y - s }, 1.6f, 5.0f, 4.0f);
        p.addArrow ({ c.x, c.y, c.x, c.y + s }, 1.6f, 5.0f, 4.0f);
    }
    else
    {
        p.addArrow ({ c.x - s * 0.1f, c.y + s * 0.1f, c.x + s, c.y - s }, 1.6f, 6.0f, 5.0f);
        p.addArrow ({ c.x + s * 0.1f, c.y - s * 0.1f, c.x - s, c.y + s }, 1.6f, 6.0f, 5.0f);
    }

    g.setColour (amber.withAlpha (hover ? 1.0f : 0.85f));
    g.fillPath (p);
}

void FrameOverlay::Handle::mouseDown (const juce::MouseEvent&)
{
    owner.getFrame (startCx, startCy, startH);
    if (startH <= 0.0)
        startH = 1.0;
}

void FrameOverlay::Handle::mouseDrag (const juce::MouseEvent& e)
{
    const auto a = owner.screenArea().toDouble();
    const auto d = (e.getScreenPosition() - e.getMouseDownScreenPosition()).toDouble();

    if (kind == Kind::move)
    {
        owner.setFrame (startCx + d.x / a.getWidth(), startCy + d.y / a.getHeight(), startH);
        return;
    }

    // Zoom around the centre: the top-right corner follows the mouse.
    const double cx = a.getX() + startCx * a.getWidth();
    const double cy = a.getY() + startCy * a.getHeight();
    const double h0 = startH * a.getHeight();
    const double cornerX = cx + h0 * owner.aspect() * 0.5 + d.x;
    const double cornerY = cy - h0 * 0.5 + d.y;
    const double newH = juce::jmax (2.0 * (cy - cornerY), 2.0 * (cornerX - cx) / owner.aspect());

    owner.setFrame (startCx, startCy, newH / a.getHeight());
}

void FrameOverlay::Handle::mouseUp (const juce::MouseEvent&)
{
    owner.recorder.saveSettings();
}

void FrameOverlay::Handle::mouseDoubleClick (const juce::MouseEvent&)
{
    if (kind != Kind::move)
        return;

    owner.resetToDefault();
}

void FrameOverlay::Handle::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    double cx, cy, h;
    owner.getFrame (cx, cy, h);
    if (h <= 0.0)
        h = 1.0;
    owner.setFrame (cx, cy, h * std::exp (-w.deltaY * 1.2));   // wheel up = zoom in
    owner.recorder.saveSettings();
}
