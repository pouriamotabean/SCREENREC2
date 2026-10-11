#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "ScreenCapture.h"

// Owns one recording session: screen capture (ScreenCapture + ffmpeg), the
// master-bus audio tap, and the final alignment + mux into a single MP4.
//
// Audio thread  -> pushAudio()  (lock-free FIFO, never blocks)
// Drain thread  -> FIFO -> 32-bit float temp WAV
// Worker thread -> start-up (encoder probe, ffmpeg launch) and finishing
//                  (alignment to the video clock, AAC 320, MP4 mux)
class Recorder : private juce::Timer
{
public:
    enum class State { idle, starting, recording, saving, error };

    Recorder();
    ~Recorder() override;

    //==== audio thread =========================================================
    void prepare (double sampleRate, int maxBlockSize);
    // sidechain: the mic (may be nullptr). Recorded into the video, never sent to the output.
    void processAudio (juce::AudioBuffer<float>& buffer, const juce::AudioBuffer<float>* sidechain);

    //==== message thread =======================================================
    // probeRect: physical screen pixels of the sync-test flash panel (test only).
    void start (bool syncTest, juce::Rectangle<int> probeRect = {});
    void stop();
    void warmUp();      // probe the GPU encoder in the background (call when the editor opens)

    State getState() const          { return state.load(); }
    bool isSyncTest() const         { return syncTestMode.load(); }
    double getElapsedSeconds() const;
    juce::String getStatusText() const;
    juce::String getEncoderName() const;
    double getSyncOffsetMs() const  { return syncOffsetMs.load(); }

    // Sync test: whether the flash panel should be lit right now.
    bool isFlashOn() const;

    static juce::File getOutputFolder();

    //==== capture mode (message thread) ========================================
    // YOUTUBE: movable 16:9 frame -> 1920x1080.  REELS: movable 9:16 frame -> 1080x1920.
    enum class Mode { youtube = 0, reels = 1 };
    Mode getMode() const            { return (Mode) mode.load(); }
    void setMode (Mode m);          // ignored while busy
    static double aspectOf (Mode m) { return m == Mode::reels ? 9.0 / 16.0 : 16.0 / 9.0; }

    // Frame of a mode: centre x (fraction of screen width), centre y and height
    // (fractions of screen height). h < 0 means "not set yet, use the default".
    // The active mode's frame is read live by the capture thread.
    void setFrame (Mode m, double cx, double cy, double h);
    void getFrame (Mode m, double& cx, double& cy, double& h) const;
    void saveSettings();

    // Side-chain mic meter (peak since last call) and whether a side-chain is connected.
    float takeMicLevel()            { return micLevel.exchange (0.0f); }
    bool isMicConnected() const     { return micConnected.load(); }

    bool isBusy() const
    {
        const auto s = state.load();
        return s == State::starting || s == State::recording || s == State::saving;
    }

private:
    void timerCallback() override;

    void runStart (bool syncTest, juce::Rectangle<int> probeRect);
    void runFinish();
    void drainLoop();
    void drainOnce();
    void probeEncoder();
    void setStatus (const juce::String& s);
    void fail (const juce::String& s);
    void log (const juce::String& s);

    juce::File findFfmpeg() const;
    juce::StringArray videoEncoderArgs (const juce::String& encoder) const;
    std::wstring buildCommand (const juce::StringArray& args) const;

    bool renderAlignedAudio (const juce::File& rawWav, const juce::File& outWav,
                             int64_t videoStartTicks, int64_t frameCount,
                             int firstChannel, double timeShift);
    bool mixWavFiles (const juce::File& a, const juce::File& b, const juce::File& out);
    double analyseSyncTest (const juce::File& alignedWav, const std::vector<float>& probe, int& pairsFound);

    void loadSettings();
    void loadSyncOffset();
    void saveSyncOffset (double ms);

    //==== shared state =========================================================
    std::atomic<State> state { State::idle };
    std::atomic<bool> syncTestMode { false };
    std::atomic<bool> audioActive { false };       // audio thread pushes while true
    std::atomic<double> syncOffsetMs { 0.0 };
    std::atomic<int> mode { 0 };
    std::atomic<double> frames[2][3] { { { 0.5 }, { 0.5 }, { -1.0 } }, { { 0.5 }, { 0.5 }, { -1.0 } } };
    std::atomic<float> micLevel { 0.0f };
    std::atomic<bool> micConnected { false };
    float sessionMicPeak = 0.0f;
    std::atomic<double> currentSampleRate { 48000.0 };
    std::atomic<int> currentBlockSize { 512 };
    std::atomic<int64_t> captureStartTicks { 0 };
    std::atomic<bool> stopRequested { false };

    // Lock-free audio FIFO (interleaved stereo float).
    std::unique_ptr<juce::AbstractFifo> audioFifo;
    std::vector<float> audioFifoData;

    struct BlockStamp { int64_t sampleIndex; int64_t ticks; };
    std::unique_ptr<juce::AbstractFifo> stampFifo;
    std::vector<BlockStamp> stampFifoData;

    // Audio-thread-only state.
    bool audioWasActive = false;
    int64_t samplesPushed = 0;
    int clickSamplesLeft = 0;
    double clickPhase = 0.0;
    int nextClickIndex = 0;

    // Sync test schedule (clock ticks).
    std::atomic<int64_t> testFirstFlash { 0 };
    static constexpr int testFlashCount = 10;
    static constexpr double testFlashInterval = 1.037;    // not a multiple of 1/30 s on purpose
    static constexpr double testFlashLength = 0.2;

    // Session.
    std::unique_ptr<ScreenCapture> capture;
    std::unique_ptr<juce::AudioFormatWriter> rawWriter;
    std::vector<BlockStamp> stamps;
    double sessionSampleRate = 48000.0;
    int sessionBlockSize = 512;
    juce::File sessionTempDir, sessionLog, sessionOutput;
    juce::String sessionName;

    std::thread worker, drainThread, probeThread;
    std::atomic<bool> drainRunning { false };
    std::atomic<bool> workerBusy { false };
    void joinWorker();

    juce::String encoder;               // chosen video encoder
    std::atomic<bool> encoderReady { false };
    mutable std::mutex textLock;
    juce::String statusText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Recorder)
};
