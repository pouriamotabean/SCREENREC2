#include "ScreenCapture.h"
#include "Timing.h"
#include <algorithm>
#include <cmath>

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>

namespace
{
    // Make this thread see real (physical) pixels regardless of the host's
    // DPI mode. Loaded dynamically so older Windows builds still load the DLL.
    struct PhysicalPixelScope
    {
        using SetCtxFn = HANDLE (WINAPI*) (HANDLE);
        SetCtxFn fn = nullptr;
        HANDLE previous = nullptr;

        PhysicalPixelScope()
        {
            if (auto* user32 = GetModuleHandleW (L"user32.dll"))
                fn = (SetCtxFn) (void*) GetProcAddress (user32, "SetThreadDpiAwarenessContext");

            if (fn != nullptr)
                previous = fn ((HANDLE) (intptr_t) -4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        }

        ~PhysicalPixelScope()
        {
            if (fn != nullptr && previous != nullptr)
                fn (previous);
        }
    };

    struct DibSurface
    {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ old = nullptr;
        uint8_t* bits = nullptr;
        int w = 0, h = 0;

        bool create (HDC reference, int width, int height)
        {
            w = width; h = height;
            BITMAPINFO bi {};
            bi.bmiHeader.biSize = sizeof (bi.bmiHeader);
            bi.bmiHeader.biWidth = width;
            bi.bmiHeader.biHeight = -height;    // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            dc = CreateCompatibleDC (reference);
            void* p = nullptr;
            bitmap = CreateDIBSection (reference, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
            if (dc == nullptr || bitmap == nullptr)
                return false;

            bits = (uint8_t*) p;
            old = SelectObject (dc, bitmap);
            return true;
        }

        ~DibSurface()
        {
            if (dc != nullptr && old != nullptr) SelectObject (dc, old);
            if (bitmap != nullptr) DeleteObject (bitmap);
            if (dc != nullptr) DeleteDC (dc);
        }
    };

    struct CursorCache
    {
        HCURSOR handle = nullptr;
        int hotX = 0, hotY = 0, w = 32, h = 32;

        void update (HCURSOR c)
        {
            if (c == handle)
                return;

            handle = c;
            hotX = hotY = 0;
            w = GetSystemMetrics (SM_CXCURSOR);
            h = GetSystemMetrics (SM_CYCURSOR);

            ICONINFO ii {};
            if (GetIconInfo (c, &ii))
            {
                hotX = (int) ii.xHotspot;
                hotY = (int) ii.yHotspot;

                BITMAP bm {};
                if (ii.hbmColor != nullptr && GetObject (ii.hbmColor, sizeof (bm), &bm))
                {
                    w = bm.bmWidth; h = bm.bmHeight;
                }
                else if (ii.hbmMask != nullptr && GetObject (ii.hbmMask, sizeof (bm), &bm))
                {
                    w = bm.bmWidth; h = bm.bmHeight / 2;   // monochrome: AND + XOR masks stacked
                }

                if (ii.hbmColor != nullptr) DeleteObject (ii.hbmColor);
                if (ii.hbmMask != nullptr)  DeleteObject (ii.hbmMask);
            }
        }
    };
}

namespace
{
    // Anti-aliased amber ring (+ soft fill) blended into a top-down BGRA image.
    void drawRipple (uint8_t* bits, int w, int h, double cx, double cy, double radius, double thickness,
                     double ringAlpha, double fillAlpha)
    {
        const double outer = radius + thickness;
        const int x0 = std::max (0, (int) std::floor (cx - outer - 1)), x1 = std::min (w - 1, (int) std::ceil (cx + outer + 1));
        const int y0 = std::max (0, (int) std::floor (cy - outer - 1)), y1 = std::min (h - 1, (int) std::ceil (cy + outer + 1));
        const double cr = 242, cg = 178, cb = 74;   // amber

        for (int y = y0; y <= y1; ++y)
        {
            uint8_t* row = bits + (size_t) y * w * 4;
            for (int x = x0; x <= x1; ++x)
            {
                const double d = std::hypot (x + 0.5 - cx, y + 0.5 - cy);
                double a = 0.0;

                const double ringDist = std::abs (d - radius) - thickness * 0.5;
                if (ringDist < 1.0)
                    a = ringAlpha * std::clamp (1.0 - ringDist, 0.0, 1.0);

                if (d < radius)
                    a = std::max (a, fillAlpha * std::clamp (radius - d, 0.0, 1.0));

                if (a <= 0.0)
                    continue;

                uint8_t* px = row + x * 4;
                px[0] = (uint8_t) (px[0] + (cb - px[0]) * a);
                px[1] = (uint8_t) (px[1] + (cg - px[1]) * a);
                px[2] = (uint8_t) (px[2] + (cr - px[2]) * a);
            }
        }
    }
}

ScreenCapture::~ScreenCapture()
{
    if (running.load())
        stop();
}

bool ScreenCapture::measure()
{
    PhysicalPixelScope physical;
    nativeW = GetSystemMetrics (SM_CXSCREEN);
    nativeH = GetSystemMetrics (SM_CYSCREEN);
    return nativeW > 0 && nativeH > 0;
}

bool ScreenCapture::open (double frameAspect, int outputWidth, int outputHeight)
{
    if (! measure())
        return false;

    framed = true;
    aspect = frameAspect;
    outW = outputWidth;
    outH = outputHeight;
    return true;
}

bool ScreenCapture::openFullScreen()
{
    if (! measure())
        return false;

    framed = false;
    outH = 1080;
    outW = (int) std::lround ((double) nativeW * 1080.0 / (double) nativeH / 2.0) * 2;
    return true;
}

void ScreenCapture::setProbeRect (int x, int y, int w, int h)
{
    if (nativeW <= 0 || nativeH <= 0)
        return;

    const double sx = (double) outW / nativeW, sy = (double) outH / nativeH;
    probeX = std::clamp ((int) std::lround (x * sx), 0, outW - 1);
    probeY = std::clamp ((int) std::lround (y * sy), 0, outH - 1);
    probeW = std::clamp ((int) std::lround (w * sx), 1, outW - probeX);
    probeH = std::clamp ((int) std::lround (h * sy), 1, outH - probeY);
    probeEnabled = true;
}

std::vector<float> ScreenCapture::getProbeSamples() const
{
    std::lock_guard<std::mutex> g (probeLock);
    return probeSamples;
}

bool ScreenCapture::start (const std::wstring& cmd, const std::wstring& logPath)
{
    if (outW <= 0 && ! openFullScreen())
        return false;

    const size_t frameBytes = (size_t) outW * (size_t) outH * 4;
    buffers.assign (frameBytes > 16u * 1024 * 1024 ? 5 : 8, std::vector<uint8_t> (frameBytes, 0));   // 4K frames are ~33 MB
    freeBuffers.clear();
    for (int i = 0; i < (int) buffers.size(); ++i)
        freeBuffers.push_back (i);
    queue.clear();
    probeSamples.clear();
    framesQueued = 0;
    framesRepeated = 0;
    failed = false;

    if (! ffmpeg.start (cmd, true, logPath))
        return false;

    // Frame 0 is captured 100 ms from now; everything is timed from here.
    startTicks = Timing::now() + Timing::frequency() / 10;

    running = true;
    writerThread = std::thread ([this] { writerLoop(); });
    captureThread = std::thread ([this] { captureLoop(); });
    return true;
}

bool ScreenCapture::stop()
{
    running = false;

    if (captureThread.joinable())
        captureThread.join();

    {
        std::lock_guard<std::mutex> g (lock);
        queue.push_back ({ -1, true });
    }
    queueCv.notify_all();

    if (writerThread.joinable())
        writerThread.join();

    ffmpeg.closeStdin();

    if (! ffmpeg.waitForExit (120000))
    {
        ffmpeg.kill();
        return false;
    }

    return ffmpeg.exitCode() == 0 && ! failed.load();
}

void ScreenCapture::captureLoop()
{
    PhysicalPixelScope physical;
    Timing::HighResTimerScope highRes;
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    HDC screen = GetDC (nullptr);
    const bool scaling = framed || (nativeW != outW || nativeH != outH);

    // Largest region with the frame's aspect that fits the screen.
    int maxRegionH = nativeH;
    int maxRegionW = (int) std::lround (nativeH * aspect);
    if (maxRegionW > nativeW)
    {
        maxRegionW = nativeW;
        maxRegionH = (int) std::lround (nativeW / aspect);
    }

    DibSurface native, out;
    bool ok = out.create (screen, outW, outH);
    if (ok && scaling)
        ok = framed ? native.create (screen, maxRegionW, maxRegionH)
                   : native.create (screen, nativeW, nativeH);

    if (! ok)
    {
        failed = true;
        ReleaseDC (nullptr, screen);
        return;
    }

    SetStretchBltMode (out.dc, HALFTONE);
    SetBrushOrgEx (out.dc, 0, 0, nullptr);

    // Current capture region in physical pixels (whole screen for FULL).
    int rx = 0, ry = 0, rw = nativeW, rh = nativeH;

    // REELS camera smoothing state (physical pixels).
    double sCx = 0.0, sCy = 0.0, sH = 0.0;
    bool smoothInit = false;

    auto updateRegion = [&]
    {
        if (! framed || region == nullptr)
            return;

        const double hFrac = region[2].load();
        const double tCx = std::clamp (region[0].load(), 0.0, 1.0) * nativeW;
        const double tCy = std::clamp (region[1].load(), 0.0, 1.0) * nativeH;
        const double tH  = (hFrac > 0.0 ? std::clamp (hFrac, 0.05, 1.0) : 1.0) * nativeH;

        if (! smoothInit)
        {
            sCx = tCx; sCy = tCy; sH = tH;
            smoothInit = true;
        }
        else
        {
            constexpr double k = 0.22;   // ~150 ms glide at 30 fps
            sCx += (tCx - sCx) * k;
            sCy += (tCy - sCy) * k;
            sH  += (tH  - sH)  * k;
        }

        rh = std::clamp ((int) std::lround (sH), 16, maxRegionH);
        rw = std::clamp ((int) std::lround (rh * aspect), 16, maxRegionW);
        rh = std::min (rh, (int) std::lround (rw / aspect));
        rx = std::clamp ((int) std::lround (sCx - rw * 0.5), 0, nativeW - rw);
        ry = std::clamp ((int) std::lround (sCy - rh * 0.5), 0, nativeH - rh);
    };

    const int64_t freq = Timing::frequency();
    CursorCache cursor;
    float lastProbe = 0.0f;

    auto frameTime = [&] (int64_t t0, int64_t n) { return t0 + (int64_t) ((double) n * (double) freq / fps); };
    auto pausedSoFar = [&]() -> int64_t { return pauseTotal != nullptr ? pauseTotal->load() : 0; };
    auto isPaused = [&]() { return pauseFlag != nullptr && pauseFlag->load(); };

    struct Click { double x, y; int64_t frame; };
    std::vector<Click> clicks;
    bool buttonWasDown = false;

    const int64_t t0 = startTicks.load();
    int64_t n = 0;

    auto pushItem = [&] (int bufferIndex)
    {
        {
            std::lock_guard<std::mutex> g (lock);
            queue.push_back ({ bufferIndex, false });
        }
        queueCv.notify_one();
        ++framesQueued;
        if (bufferIndex < 0)
            ++framesRepeated;

        if (probeEnabled)
        {
            std::lock_guard<std::mutex> g (probeLock);
            probeSamples.push_back (lastProbe);
        }
    };

    while (running.load())
    {
        if (isPaused())
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
            continue;
        }

        Timing::sleepUntil (frameTime (t0, n) + pausedSoFar());
        if (isPaused())
            continue;

        // If we fell behind, repeat the last image for every missed slot so
        // the file stays locked to the clock (paused time does not count).
        const int64_t current = (int64_t) std::floor ((double) (Timing::now() - pausedSoFar() - t0) * fps / (double) freq);
        while (n < current)
        {
            pushItem (-1);
            ++n;
        }
        n = std::max (n, current);

        int bufferIndex = -1;
        {
            std::lock_guard<std::mutex> g (lock);
            if (! freeBuffers.empty())
            {
                bufferIndex = freeBuffers.back();
                freeBuffers.pop_back();
            }
        }

        updateRegion();
        const double sx = (double) outW / rw, sy = (double) outH / rh;

        if (bufferIndex >= 0)
        {
            if (scaling)
            {
                BitBlt (native.dc, 0, 0, rw, rh, screen, rx, ry, SRCCOPY | CAPTUREBLT);
                StretchBlt (out.dc, 0, 0, outW, outH, native.dc, 0, 0, rw, rh, SRCCOPY);
            }
            else
            {
                BitBlt (out.dc, 0, 0, outW, outH, screen, 0, 0, SRCCOPY | CAPTUREBLT);
            }

            // Click highlight: amber ripple where the mouse was pressed.
            if (clickEnabled != nullptr && clickEnabled->load())
            {
                const bool down = (GetAsyncKeyState (VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState (VK_RBUTTON) & 0x8000) != 0;
                if (down && ! buttonWasDown)
                {
                    POINT p {};
                    if (GetCursorPos (&p))
                        clicks.push_back ({ (double) p.x, (double) p.y, n });
                }
                buttonWasDown = down;

                constexpr int life = 14;   // frames (~0.45 s)
                clicks.erase (std::remove_if (clicks.begin(), clicks.end(), [&] (const Click& c) { return n - c.frame >= life; }), clicks.end());

                if (! clicks.empty())
                {
                    GdiFlush();
                    const double k = (double) std::min (outW, outH) / 1080.0;
                    for (auto& c : clicks)
                    {
                        const double t = (double) (n - c.frame) / life;
                        const double ox = (c.x - rx) * sx, oy = (c.y - ry) * sy;
                        drawRipple (out.bits, outW, outH, ox, oy, (12.0 + 30.0 * t) * k, 3.0 * k,
                                    0.9 * std::pow (1.0 - t, 1.4), 0.28 * (1.0 - t));
                    }
                }
            }
            else
            {
                clicks.clear();
                buttonWasDown = false;
            }

            CURSORINFO ci {};
            ci.cbSize = sizeof (ci);
            if (GetCursorInfo (&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor != nullptr)
            {
                cursor.update (ci.hCursor);
                const int cx = (int) std::lround ((ci.ptScreenPos.x - cursor.hotX - rx) * sx);
                const int cy = (int) std::lround ((ci.ptScreenPos.y - cursor.hotY - ry) * sy);
                const int cw = std::max (8, (int) std::lround (cursor.w * sx));
                const int ch = std::max (8, (int) std::lround (cursor.h * sy));
                DrawIconEx (out.dc, cx, cy, ci.hCursor, cw, ch, 0, nullptr, DI_NORMAL);
            }

            GdiFlush();

            if (probeEnabled)
            {
                double sum = 0.0;
                for (int y = probeY; y < probeY + probeH; ++y)
                {
                    const uint8_t* row = out.bits + ((size_t) y * outW + probeX) * 4;
                    for (int x = 0; x < probeW; ++x)
                        sum += 0.0722 * row[x * 4] + 0.7152 * row[x * 4 + 1] + 0.2126 * row[x * 4 + 2];
                }
                lastProbe = (float) (sum / ((double) probeW * probeH * 255.0));
            }

            std::copy (out.bits, out.bits + buffers[(size_t) bufferIndex].size(), buffers[(size_t) bufferIndex].begin());
        }

        pushItem (bufferIndex);   // -1 here means: encoder is behind, repeat
        ++n;
    }

    ReleaseDC (nullptr, screen);
}

void ScreenCapture::writerLoop()
{
    int last = -1;

    for (;;)
    {
        Item item;
        {
            std::unique_lock<std::mutex> g (lock);
            queueCv.wait (g, [this] { return ! queue.empty(); });
            item = queue.front();
            queue.pop_front();
        }

        if (item.end)
            break;

        int toWrite = item.buffer >= 0 ? item.buffer : last;

        if (toWrite < 0)
        {
            // Nothing captured yet: write a black frame to keep frame count exact.
            static thread_local std::vector<uint8_t> black;
            black.assign (buffers[0].size(), 0);
            if (! failed.load() && ! ffmpeg.write (black.data(), black.size()))
                failed = true;
            continue;
        }

        if (! failed.load() && ! ffmpeg.write (buffers[(size_t) toWrite].data(), buffers[(size_t) toWrite].size()))
            failed = true;

        if (item.buffer >= 0)
        {
            std::lock_guard<std::mutex> g (lock);
            if (last >= 0)
                freeBuffers.push_back (last);
            last = item.buffer;
        }
    }
}

#else  // non-Windows stub

ScreenCapture::~ScreenCapture() {}
bool ScreenCapture::open (double, int, int) { return false; }
bool ScreenCapture::openFullScreen() { return false; }
bool ScreenCapture::measure() { return false; }
void ScreenCapture::setProbeRect (int, int, int, int) {}
std::vector<float> ScreenCapture::getProbeSamples() const { return {}; }
bool ScreenCapture::start (const std::wstring&, const std::wstring&) { return false; }
bool ScreenCapture::stop() { return false; }
void ScreenCapture::captureLoop() {}
void ScreenCapture::writerLoop() {}

#endif
