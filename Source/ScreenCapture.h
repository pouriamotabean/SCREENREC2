#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "WinProcess.h"

// Captures the primary screen (with mouse cursor) at a fixed 30 fps grid driven
// by the shared high-resolution clock and pipes raw BGRA frames into ffmpeg.
//  - Frame mode: a moving region with a fixed aspect (16:9 YouTube, 9:16 Reels),
//    live position/zoom smoothed like a camera, scaled to the output size.
//  - Full screen (sync test only): whole screen scaled to 1080p height.
//
// Timeline guarantee: frame n of the output file always represents the screen
// at time startTicks + n / 30 s. If a capture is late, the previous image is
// repeated so the file never drifts from the clock.
class ScreenCapture
{
public:
    static constexpr int fps = 30;

    ScreenCapture() = default;
    ~ScreenCapture();

    // Measures the screen and decides the output size. Call before start().
    bool open (double aspect, int outputWidth, int outputHeight);
    bool openFullScreen();

    // Frame mode: three atomics {centreX, centreY, height} as fractions of the
    // screen (x of width, y and height of height). Read every frame.
    void setRegionSource (const std::atomic<double>* cxCyH) { region = cxCyH; }
    int getOutputWidth() const   { return outW; }
    int getOutputHeight() const  { return outH; }
    int getNativeWidth() const   { return nativeW; }
    int getNativeHeight() const  { return nativeH; }

    // Starts ffmpeg with the given command line (reads raw BGRA from stdin)
    // and begins capturing.
    bool start (const std::wstring& ffmpegCommandLine, const std::wstring& logPath);

    // Stops capturing, lets ffmpeg finish the file. Returns true if ffmpeg
    // exited cleanly.
    bool stop();

    int64_t getStartTicks() const   { return startTicks.load(); }
    int64_t getFrameCount() const   { return framesQueued.load(); }
    int64_t getRepeatedFrames() const { return framesRepeated.load(); }
    bool hasFailed() const          { return failed.load(); }

    // Sync test: mean brightness (0..1) of a rectangle given in NATIVE screen
    // pixels, sampled for every frame. Set before start().
    void setProbeRect (int x, int y, int w, int h);
    std::vector<float> getProbeSamples() const;

private:
    void captureLoop();
    void writerLoop();

    struct Item { int buffer; bool end; };

    int nativeW = 0, nativeH = 0, outW = 0, outH = 0;
    bool framed = false;
    double aspect = 16.0 / 9.0;
    bool measure();
    const std::atomic<double>* region = nullptr;

    WinProcess ffmpeg;
    std::thread captureThread, writerThread;
    std::atomic<bool> running { false }, failed { false };
    std::atomic<int64_t> startTicks { 0 }, framesQueued { 0 }, framesRepeated { 0 };

    std::vector<std::vector<uint8_t>> buffers;
    std::vector<int> freeBuffers;
    std::deque<Item> queue;     // buffer index, or -1 = repeat previous frame
    std::mutex lock;
    std::condition_variable queueCv;

    bool probeEnabled = false;
    int probeX = 0, probeY = 0, probeW = 0, probeH = 0;   // in output pixels
    std::vector<float> probeSamples;
    mutable std::mutex probeLock;
};
