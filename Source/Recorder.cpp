#include "Recorder.h"
#include "Timing.h"
#include "WinProcess.h"
#include <algorithm>
#include <cmath>

namespace
{
    std::atomic<bool> anyInstanceBusy { false };   // only one recording at a time

    constexpr double clickFrequency = 1000.0;
    constexpr double clickLengthSeconds = 0.015;
    constexpr double clickLevel = 0.5;

    std::wstring toWide (const juce::String& s) { return s.toWideCharPointer(); }

    juce::String quoteArg (const juce::String& a)
    {
        if (a.isEmpty())
            return "\"\"";
        if (a.containsAnyOf (" \t\"&|<>^,;=()"))
            return "\"" + a.replace ("\"", "\\\"") + "\"";
        return a;
    }

    double median (std::vector<double> v)
    {
        if (v.empty())
            return 0.0;
        std::sort (v.begin(), v.end());
        const size_t m = v.size() / 2;
        return (v.size() % 2) ? v[m] : 0.5 * (v[m - 1] + v[m]);
    }
}

//==============================================================================
Recorder::Recorder()
{
    loadSyncOffset();
    loadSettings();
    {
        const auto f = getOutputFolder().getChildFile ("mic_offset_ms.txt");
        if (f.existsAsFile())
            micOffsetMs = f.loadFileAsString().trim().getDoubleValue();
    }
    startTimerHz (20);
}

Recorder::~Recorder()
{
    stopTimer();
    joinWorker();

    if (state.load() == State::recording)
    {
        // Host is closing the plugin mid-recording: finish the file synchronously.
        state = State::saving;
        runFinish();
    }

    joinWorker();

    if (probeThread.joinable())
        probeThread.join();

    if (drainThread.joinable())
    {
        drainRunning = false;
        drainThread.join();
    }
}

void Recorder::joinWorker()
{
    if (worker.joinable())
        worker.join();
}

juce::File Recorder::getOutputFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("ScreenRec");
}

//==============================================================================
// Audio thread

void Recorder::prepare (double sampleRate, int maxBlockSize)
{
    currentSampleRate = sampleRate > 0 ? sampleRate : 48000.0;
    currentBlockSize = juce::jmax (1, maxBlockSize);
}

void Recorder::processAudio (juce::AudioBuffer<float>& buffer, const juce::AudioBuffer<float>* sidechain)
{
    // Mic gain (knob), ramped per block.
    const float gDb = micGainDb.load (std::memory_order_relaxed);
    const float targetMicGain = gDb <= -35.9f ? 0.0f : juce::Decibels::decibelsToGain (gDb);
    const float startMicGain = currentMicGain;
    currentMicGain = targetMicGain;

    // Meters work even when not recording.
    {
        float pk = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            pk = juce::jmax (pk, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
        if (pk > masterLevel.load (std::memory_order_relaxed))
            masterLevel.store (pk, std::memory_order_relaxed);
    }

    if (sidechain != nullptr && sidechain->getNumChannels() > 0 && sidechain->getNumSamples() > 0)
    {
        float pk = 0.0f;
        for (int ch = 0; ch < sidechain->getNumChannels(); ++ch)
            pk = juce::jmax (pk, sidechain->getMagnitude (ch, 0, sidechain->getNumSamples()));
        pk *= targetMicGain;
        if (pk > micLevel.load (std::memory_order_relaxed))
            micLevel.store (pk, std::memory_order_relaxed);
        micConnected.store (true, std::memory_order_relaxed);
    }
    else
    {
        micConnected.store (false, std::memory_order_relaxed);
    }

    const bool active = audioActive.load (std::memory_order_acquire);

    if (active && ! audioWasActive)
    {
        samplesPushed = 0;
        nextClickIndex = 0;
        clickSamplesLeft = 0;
        clickPhase = 0.0;
    }

    audioWasActive = active;

    if (! active)
        return;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    const int64_t now = Timing::now();
    const double sr = sessionSampleRate;

    // ---- sync test: put a short beep into the output at the scheduled times
    if (syncTestMode.load() || micTestMode.load())
    {
        const int64_t freq = Timing::frequency();
        const int64_t blockEnd = now + (int64_t) ((double) numSamples / sr * (double) freq);
        int pos = 0;

        while (pos < numSamples)
        {
            if (clickSamplesLeft <= 0)
            {
                if (nextClickIndex >= testBeepCount())
                    break;

                const int64_t tk = testFirstFlash.load()
                                 + Timing::fromSeconds (testBeepTime (nextClickIndex));
                if (tk >= blockEnd)
                    break;

                const int start = tk <= now ? 0
                                            : (int) ((double) (tk - now) / (double) freq * sr);
                pos = juce::jmax (pos, juce::jmin (start, numSamples - 1));
                clickSamplesLeft = (int) (clickLengthSeconds * sr);
                clickPhase = 0.0;
                ++nextClickIndex;
            }

            const int n = juce::jmin (clickSamplesLeft, numSamples - pos);
            const double inc = juce::MathConstants<double>::twoPi * clickFrequency / sr;

            for (int i = 0; i < n; ++i)
            {
                const float v = (float) ((micTestMode.load() ? 0.3 : clickLevel) * std::sin (clickPhase));
                clickPhase += inc;
                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.addSample (ch, pos + i, v);
            }

            pos += n;
            clickSamplesLeft -= n;
        }
    }

    // ---- paused: nothing goes into the file
    if (paused.load (std::memory_order_acquire))
        return;

    // ---- tap: exactly what goes to the speakers/headphones
    if (stampFifo != nullptr && stampFifo->getFreeSpace() > 0)
    {
        int s1, n1, s2, n2;
        stampFifo->prepareToWrite (1, s1, n1, s2, n2);
        if (n1 > 0)
            stampFifoData[(size_t) s1] = { samplesPushed, now - pausedTotalTicks.load (std::memory_order_acquire) };
        stampFifo->finishedWrite (n1 + n2);
    }

    if (audioFifo != nullptr)
    {
        const float* left  = buffer.getReadPointer (0);
        const float* right = buffer.getReadPointer (numChannels > 1 ? 1 : 0);

        const bool hasMic = sidechain != nullptr && sidechain->getNumChannels() > 0
                         && sidechain->getNumSamples() >= numSamples;
        const float* micL = hasMic ? sidechain->getReadPointer (0) : nullptr;
        const float* micR = hasMic ? sidechain->getReadPointer (sidechain->getNumChannels() > 1 ? 1 : 0) : nullptr;

        int s1, n1, s2, n2;
        audioFifo->prepareToWrite (numSamples, s1, n1, s2, n2);   // in frames

        auto copy = [&] (int dst, int count, int srcOffset)
        {
            float* d = audioFifoData.data() + (size_t) dst * 4;
            const float gStep = (targetMicGain - startMicGain) / (float) juce::jmax (1, numSamples);
            for (int i = 0; i < count; ++i)
            {
                const float g = micTestMode.load (std::memory_order_relaxed) ? 1.0f
                              : startMicGain + gStep * (float) (srcOffset + i);
                d[i * 4]     = left[srcOffset + i];
                d[i * 4 + 1] = right[srcOffset + i];
                d[i * 4 + 2] = hasMic ? micL[srcOffset + i] * g : 0.0f;
                d[i * 4 + 3] = hasMic ? micR[srcOffset + i] * g : 0.0f;
            }
        };

        if (n1 > 0) copy (s1, n1, 0);
        if (n2 > 0) copy (s2, n2, n1);
        audioFifo->finishedWrite (n1 + n2);
        samplesPushed += n1 + n2;   // a full FIFO shows up as a gap and is filled with silence later
    }
}

//==============================================================================
// Message thread

void Recorder::warmUp()
{
    if (encoderReady.load() || probeThread.joinable())
        return;

    probeThread = std::thread ([this] { probeEncoder(); });
}

void Recorder::start (bool syncTest, juce::Rectangle<int> probeRect, bool micTest)
{
    const auto s = state.load();
    if (s == State::starting || s == State::recording || s == State::saving)
        return;

    bool expected = false;
    if (! anyInstanceBusy.compare_exchange_strong (expected, true))
    {
        setStatus ("Another ScreenRec instance is recording");
        return;
    }

    joinWorker();
    state = State::starting;
    syncTestMode = syncTest && ! micTest;
    micTestMode = micTest;
    stopRequested = false;
    paused = false;
    pausedTotalTicks = 0;
    countdownEnd = (syncTest || micTest) ? 0 : Timing::now() + Timing::fromSeconds (3.0);
    setStatus (micTest ? "Mic test: starting..." : (syncTest ? "Sync test: starting..." : "Get ready..."));

    workerBusy = true;
    worker = std::thread ([this, syncTest, probeRect, micTest]
    {
        runStart (syncTest && ! micTest, probeRect, micTest);
        workerBusy = false;
    });
}

void Recorder::stop()
{
    if (state.load() != State::recording)
    {
        if (state.load() == State::starting)
            stopRequested = true;
        return;
    }

    joinWorker();
    state = State::saving;
    setStatus ("Saving...");

    workerBusy = true;
    worker = std::thread ([this]
    {
        runFinish();
        workerBusy = false;
    });
}

double Recorder::getElapsedSeconds() const
{
    if (state.load() != State::recording)
        return 0.0;

    const int64_t now = Timing::now();
    int64_t pausedNow = pausedTotalTicks.load();
    if (paused.load())
        pausedNow += now - pauseStartTicks.load();

    return juce::jmax (0.0, Timing::toSeconds (now - captureStartTicks.load() - pausedNow));
}

int Recorder::getCountdown() const
{
    if (state.load() != State::starting)
        return 0;

    const int64_t end = countdownEnd.load();
    if (end == 0)
        return 0;

    const double left = Timing::toSeconds (end - Timing::now());
    return left > 0.0 ? (int) std::ceil (left) : 0;
}

void Recorder::togglePause()
{
    if (state.load() != State::recording || syncTestMode.load() || micTestMode.load())
        return;

    if (! paused.load())
    {
        pauseStartTicks = Timing::now();
        paused.store (true, std::memory_order_release);
        setStatus ("Paused");
    }
    else
    {
        pausedTotalTicks += Timing::now() - pauseStartTicks.load();   // before clearing the flag
        paused.store (false, std::memory_order_release);
        setStatus ("Recording  -  " + getEncoderName());
    }
}

bool Recorder::hasLastOutput() const
{
    std::lock_guard<std::mutex> g (textLock);
    return lastOutputPath.isNotEmpty() && juce::File (lastOutputPath).existsAsFile();
}

void Recorder::revealLastOutput() const
{
    juce::String path;
    {
        std::lock_guard<std::mutex> g (textLock);
        path = lastOutputPath;
    }

    const juce::File f (path);
    if (path.isNotEmpty() && f.existsAsFile())
        f.revealToUser();
    else
    {
        getOutputFolder().createDirectory();
        getOutputFolder().startAsProcess();
    }
}

bool Recorder::isFlashOn() const
{
    if (! syncTestMode.load() || state.load() != State::recording)
        return false;

    const double t = Timing::toSeconds (Timing::now() - testFirstFlash.load());
    if (t < 0.0)
        return false;

    const int k = (int) std::floor (t / testFlashInterval);
    return k < testFlashCount && (t - k * testFlashInterval) < testFlashLength;
}

juce::String Recorder::getStatusText() const
{
    std::lock_guard<std::mutex> g (textLock);
    return statusText;
}

juce::String Recorder::getEncoderName() const
{
    std::lock_guard<std::mutex> g (textLock);
    return encoder;
}

void Recorder::setStatus (const juce::String& s)
{
    {
        std::lock_guard<std::mutex> g (textLock);
        statusText = s;
    }
    log (s);
}

void Recorder::fail (const juce::String& s)
{
    setStatus (s);
    state = State::error;
    anyInstanceBusy = false;
}

void Recorder::log (const juce::String& s)
{
    if (sessionLog != juce::File())
        sessionLog.appendText (juce::Time::getCurrentTime().formatted ("[%H:%M:%S] ") + s + "\r\n", false, false, "\r\n");
}

void Recorder::timerCallback()
{
    if (! workerBusy.load() && worker.joinable())
        worker.join();

    const auto s = state.load();

    if (s == State::recording)
    {
        if (stopRequested.load())
            stop();
        else if (capture != nullptr && capture->hasFailed())
        {
            setStatus ("Video encoder stopped - saving what was recorded");
            stop();
        }
        else if (syncTestMode.load() || micTestMode.load())
        {
            const int64_t end = testFirstFlash.load()
                              + Timing::fromSeconds (testBeepTime (testBeepCount() - 1) + (micTestMode.load() ? 2.5 : 1.0));
            if (Timing::now() > end)
                stop();
        }
    }
}

//==============================================================================
// Encoder selection

juce::StringArray Recorder::videoEncoderArgs (const juce::String& enc, int kbps) const
{
    juce::StringArray a;
    const bool software = (enc == "libx264");

    a.addArray ({ "-vf", software ? "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p"
                                  : "scale=out_color_matrix=bt709:out_range=tv,format=nv12" });

    if (enc == "h264_nvenc")
        a.addArray ({ "-c:v", "h264_nvenc", "-preset", "p4", "-tune", "hq", "-rc", "vbr" });
    else if (enc == "h264_amf")
        a.addArray ({ "-c:v", "h264_amf", "-usage", "transcoding", "-quality", "balanced", "-rc", "vbr_peak" });
    else if (enc == "h264_qsv")
        a.addArray ({ "-c:v", "h264_qsv", "-preset", "medium" });
    else
        a.addArray ({ "-c:v", "libx264", "-preset", "veryfast" });

    a.addArray ({ "-b:v", juce::String (kbps) + "k", "-maxrate", juce::String (kbps * 3 / 2) + "k",
                  "-bufsize", juce::String (kbps * 2) + "k", "-g", "60",
                  "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv" });
    return a;
}

std::wstring Recorder::buildCommand (const juce::StringArray& args) const
{
    juce::String cmd = quoteArg (findFfmpeg().getFullPathName());
    for (auto& a : args)
        cmd << " " << quoteArg (a);
    return toWide (cmd);
}

void Recorder::probeEncoder()
{
    const auto ff = findFfmpeg();
    juce::String chosen = "libx264";

    if (ff.existsAsFile())
    {
        const auto probeLog = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("screenrec_probe.log");
        probeLog.deleteFile();

        for (auto enc : { "h264_nvenc", "h264_amf", "h264_qsv" })
        {
            juce::StringArray args { "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                                     "-i", "color=c=black:s=1920x1080:r=30", "-frames:v", "15" };
            args.addArray (videoEncoderArgs (enc));
            args.addArray ({ "-f", "null", "-" });

            if (runProcessAndWait (buildCommand (args), toWide (probeLog.getFullPathName()), 20000) == 0)
            {
                chosen = enc;
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> g (textLock);
        encoder = chosen;
    }
    encoderReady = true;
}

juce::File Recorder::findFfmpeg() const
{
    const auto module = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    const juce::File candidates[] = {
        module.getParentDirectory().getParentDirectory().getChildFile ("Resources").getChildFile ("ffmpeg.exe"),
        module.getSiblingFile ("ffmpeg.exe"),
        getOutputFolder().getChildFile ("ffmpeg.exe")
    };

    for (auto& f : candidates)
        if (f.existsAsFile())
            return f;

    return candidates[0];
}

//==============================================================================
// Start

void Recorder::runStart (bool syncTest, juce::Rectangle<int> probeRect, bool micTest)
{
    if (micTest && ! isMicConnected())
    {
        fail ("Mic test: side-chain is off. Activate ScreenRec side-chain and send the mic to it.");
        return;
    }

    const auto ff = findFfmpeg();
    if (! ff.existsAsFile())
    {
        fail ("ffmpeg.exe not found (expected inside ScreenRec.vst3/Contents/Resources)");
        return;
    }

    const auto outDir = getOutputFolder();
    sessionName = juce::String (micTest ? "MicTest_" : syncTest ? "SyncTest_" : (getMode() == Mode::reels ? "Reels_" : "YouTube_"))
                + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    sessionTempDir = outDir.getChildFile (".temp").getChildFile (sessionName);
    sessionOutput = outDir.getChildFile (sessionName + ".mp4");
    sessionLog = outDir.getChildFile ("logs").getChildFile (sessionName + ".log");

    if (! sessionTempDir.createDirectory() || ! sessionLog.getParentDirectory().createDirectory())
    {
        fail ("Cannot create folder " + outDir.getFullPathName());
        return;
    }

    log ("ScreenRec " JucePlugin_VersionString " - session " + sessionName);
    log ("ffmpeg: " + ff.getFullPathName());

    if (! encoderReady.load())
    {
        setStatus ("Checking video encoder...");
        if (probeThread.joinable())
            probeThread.join();
        if (! encoderReady.load())
            probeEncoder();
    }

    const juce::String enc = getEncoderName();
    log ("video encoder: " + enc);

    const Mode m = getMode();
    const int q = effectiveQuality (m);
    int outW = 1920, outH = 1080;
    outputSize (m, q, outW, outH);

    capture = std::make_unique<ScreenCapture>();
    capture->setRegionSource (frames[(int) m]);
    capture->setPauseSource (&paused, &pausedTotalTicks);
    if (! syncTest)
        capture->setClickSource (&clickEnabled);
    if (! (syncTest ? capture->openFullScreen()
                    : capture->open (aspectOf (m), outW, outH)))
    {
        fail ("Cannot read screen size");
        return;
    }

    log ("screen " + juce::String (capture->getNativeWidth()) + "x" + juce::String (capture->getNativeHeight())
         + " -> " + juce::String (capture->getOutputWidth()) + "x" + juce::String (capture->getOutputHeight())
         + " @ " + juce::String (ScreenCapture::fps) + " fps" + (syncTest ? "  [SYNC TEST, full screen]" : (m == Mode::reels ? "  [REELS]" : "  [YOUTUBE]")));

    if (syncTest && ! probeRect.isEmpty())
    {
        capture->setProbeRect (probeRect.getX(), probeRect.getY(), probeRect.getWidth(), probeRect.getHeight());
        log ("probe rect (physical px): " + probeRect.toString());
    }

    // ---- 3-2-1 (normal recordings); STOP during the countdown cancels
    while (countdownEnd.load() != 0 && Timing::now() < countdownEnd.load())
    {
        if (stopRequested.load())
        {
            capture.reset();
            sessionTempDir.deleteRecursively();
            log ("cancelled during countdown");
            setStatus ("Cancelled");
            countdownEnd = 0;
            state = State::idle;
            anyInstanceBusy = false;
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    countdownEnd = 0;

    // ---- audio side
    sessionSampleRate = currentSampleRate.load();
    sessionBlockSize = currentBlockSize.load();
    log ("audio " + juce::String (sessionSampleRate) + " Hz, block " + juce::String (sessionBlockSize));

    const int fifoFrames = (int) (sessionSampleRate * 10.0);
    audioFifoData.assign ((size_t) fifoFrames * 4, 0.0f);
    sessionMicPeak = 0.0f;
    audioFifo = std::make_unique<juce::AbstractFifo> (fifoFrames);
    stampFifoData.assign (65536, {});
    stampFifo = std::make_unique<juce::AbstractFifo> (65536);
    stamps.clear();
    stamps.reserve (1 << 16);

    juce::WavAudioFormat wav;
    auto rawFile = sessionTempDir.getChildFile ("audio_raw.wav");
    auto stream = std::make_unique<juce::FileOutputStream> (rawFile);
    if (! stream->openedOk())
    {
        fail ("Cannot write " + rawFile.getFullPathName());
        return;
    }

    rawWriter.reset (wav.createWriterFor (stream.get(), sessionSampleRate, 4, 32, {}, 0));   // L R micL micR
    if (rawWriter == nullptr)
    {
        fail ("Cannot create WAV writer");
        return;
    }
    stream.release();   // owned by the writer now

    // Crash safety: block stamps go to disk as they arrive.
    stampsOut.reset();
    framesFileCounter = 0;
    if (! syncTest && ! micTest)
    {
        stampsOut = std::make_unique<juce::FileOutputStream> (sessionTempDir.getChildFile ("stamps.bin"));
        if (! stampsOut->openedOk())
            stampsOut.reset();
    }

    drainRunning = true;
    drainThread = std::thread ([this] { drainLoop(); });

    if (micTest)
    {
        // Audio only: beeps go to the headphones, the mic hears them, we measure the gap.
        capture.reset();
        captureStartTicks = Timing::now() + Timing::frequency() / 10;
        testFirstFlash = captureStartTicks.load() + Timing::fromSeconds (1.0);
        audioActive.store (true, std::memory_order_release);
        state = State::recording;
        setStatus ("Mic test: hold your headphones against the mic, stay quiet");
        return;
    }

    // ---- video side
    juce::StringArray args { "-hide_banner", "-loglevel", "warning", "-y",
                             "-f", "rawvideo", "-pix_fmt", "bgra",
                             "-video_size", juce::String (capture->getOutputWidth()) + "x" + juce::String (capture->getOutputHeight()),
                             "-framerate", juce::String (ScreenCapture::fps), "-i", "-" };
    args.addArray (videoEncoderArgs (enc, syncTest ? 8000 : bitrateKbps (q)));
    args.add (sessionTempDir.getChildFile ("video.mkv").getFullPathName());

    audioActive.store (true, std::memory_order_release);

    if (! capture->start (buildCommand (args), toWide (sessionLog.getFullPathName())))
    {
        audioActive = false;
        drainRunning = false;
        if (drainThread.joinable()) drainThread.join();
        rawWriter.reset();
        fail ("Could not start ffmpeg");
        return;
    }

    captureStartTicks = capture->getStartTicks();
    testFirstFlash = captureStartTicks.load() + Timing::fromSeconds (1.5);

    if (! syncTest)
        writeSessionInfo();

    state = State::recording;
    setStatus (syncTest ? "Sync test: keep this window visible, transport stopped"
                        : "Recording  -  " + enc);
}

//==============================================================================
// Audio drain

void Recorder::drainLoop()
{
    while (drainRunning.load())
    {
        drainOnce();

        if (stampsOut != nullptr && ++framesFileCounter >= 50)   // ~1 s
        {
            framesFileCounter = 0;
            if (capture != nullptr)
                sessionTempDir.getChildFile ("frames.txt").replaceWithText (juce::String (capture->getFrameCount()));
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
}

void Recorder::drainOnce()
{
    if (audioFifo == nullptr || rawWriter == nullptr)
        return;

    for (;;)
    {
        const int ready = audioFifo->getNumReady();
        if (ready <= 0)
            break;

        const int chunk = juce::jmin (ready, 8192);
        int s1, n1, s2, n2;
        audioFifo->prepareToRead (chunk, s1, n1, s2, n2);

        juce::AudioBuffer<float> tmp (4, n1 + n2);
        auto deinterleave = [&] (int src, int count, int dstOffset)
        {
            const float* p = audioFifoData.data() + (size_t) src * 4;
            for (int i = 0; i < count; ++i)
                for (int ch = 0; ch < 4; ++ch)
                    tmp.setSample (ch, dstOffset + i, p[i * 4 + ch]);
        };
        if (n1 > 0) deinterleave (s1, n1, 0);
        if (n2 > 0) deinterleave (s2, n2, n1);
        audioFifo->finishedRead (n1 + n2);

        sessionMicPeak = juce::jmax (sessionMicPeak, tmp.getMagnitude (2, 0, tmp.getNumSamples()),
                                     tmp.getMagnitude (3, 0, tmp.getNumSamples()));

        rawWriter->writeFromAudioSampleBuffer (tmp, 0, tmp.getNumSamples());
    }

    if (stampFifo != nullptr)
    {
        const int ready = stampFifo->getNumReady();
        int s1, n1, s2, n2;
        stampFifo->prepareToRead (ready, s1, n1, s2, n2);
        const size_t first = stamps.size();
        for (int i = 0; i < n1; ++i) stamps.push_back (stampFifoData[(size_t) (s1 + i)]);
        for (int i = 0; i < n2; ++i) stamps.push_back (stampFifoData[(size_t) (s2 + i)]);
        stampFifo->finishedRead (n1 + n2);

        if (stampsOut != nullptr && stamps.size() > first)
        {
            stampsOut->write (stamps.data() + first, (stamps.size() - first) * sizeof (BlockStamp));
            stampsOut->flush();
        }
    }
}

//==============================================================================
// Finish

void Recorder::runFinish()
{
    if (micTestMode.load())
    {
        runFinishMicTest();
        return;
    }

    const bool videoOk = capture != nullptr && capture->stop();
    const int64_t frames = capture != nullptr ? capture->getFrameCount() : 0;
    const int64_t t0 = captureStartTicks.load();

    if (capture != nullptr)
        log ("video frames: " + juce::String (frames) + ", repeated (late) frames: " + juce::String (capture->getRepeatedFrames())
             + (videoOk ? "" : "  [ffmpeg reported an error]"));

    audioActive.store (false, std::memory_order_release);
    std::this_thread::sleep_for (std::chrono::milliseconds (150));   // let the current audio block finish

    drainRunning = false;
    if (drainThread.joinable())
        drainThread.join();
    drainOnce();
    rawWriter.reset();
    stampsOut.reset();
    paused = false;

    std::vector<float> probe;
    if (capture != nullptr)
        probe = capture->getProbeSamples();

    produceOutput (t0, frames, probe, false);
}

void Recorder::produceOutput (int64_t t0, int64_t frames, const std::vector<float>& probe, bool recovered)
{
    const auto videoFile = sessionTempDir.getChildFile ("video.mkv");

    if (frames <= 0 || ! videoFile.existsAsFile() || videoFile.getSize() == 0)
    {
        fail ("Video was not recorded - see log in " + sessionLog.getParentDirectory().getFullPathName());
        return;
    }

    setStatus ("Saving: aligning audio...");
    const auto rawAudio = sessionTempDir.getChildFile ("audio_raw.wav");
    const auto alignedMaster = sessionTempDir.getChildFile ("audio_master.wav");
    auto aligned = alignedMaster;

    const double blockTime = (double) sessionBlockSize / sessionSampleRate;
    const double offset = syncOffsetMs.load() / 1000.0;

    // Master: heard one buffer after the block was processed (+ measured offset).
    if (! renderAlignedAudio (rawAudio, alignedMaster, t0, frames, 0, blockTime + offset))
    {
        fail ("Audio alignment failed - see log");
        return;
    }

    // Side-chain mic: captured during the buffer before the block was processed.
    log ("side-chain mic peak: " + juce::String (juce::Decibels::gainToDecibels (sessionMicPeak, -120.0f), 1) + " dB");

    if (! syncTestMode.load() && sessionMicPeak > 1.0e-5f)
    {
        setStatus ("Saving: aligning mic...");
        const auto alignedMic = sessionTempDir.getChildFile ("audio_mic.wav");
        aligned = sessionTempDir.getChildFile ("audio_aligned.wav");

        if (! renderAlignedAudio (rawAudio, alignedMic, t0, frames, 2, -blockTime + micOffsetMs.load() / 1000.0)
            || ! mixWavFiles (alignedMaster, alignedMic, aligned, duckEnabled.load()))
        {
            fail ("Mic alignment failed - see log");
            return;
        }
    }

    juce::String resultText;

    if (syncTestMode.load())
    {
        int pairs = 0;
        const double residual = analyseSyncTest (aligned, probe, pairs);

        if (pairs >= 5)
        {
            const double newOffset = juce::jlimit (-500.0, 500.0, syncOffsetMs.load() + residual * 1000.0);
            log ("sync residual " + juce::String (residual * 1000.0, 1) + " ms from " + juce::String (pairs)
                 + " flashes -> new offset " + juce::String (newOffset, 1) + " ms");
            saveSyncOffset (newOffset);
            resultText = "Sync fixed: was off by " + juce::String (residual * 1000.0, 0)
                       + " ms, offset now " + juce::String (newOffset, 0) + " ms";
        }
        else
        {
            resultText = "Sync test failed (" + juce::String (pairs) + "/10 flashes matched). "
                         "Stop playback, keep the plugin window fully visible, try again.";
        }
    }

    // ---- mux: copy video, AAC 320 kbps audio
    setStatus ("Saving: encoding audio...");
    const int sr = (int) std::lround (sessionSampleRate);

    juce::StringArray args { "-hide_banner", "-loglevel", "warning", "-y",
                             "-i", videoFile.getFullPathName(),
                             "-i", aligned.getFullPathName(),
                             "-map", "0:v:0", "-map", "1:a:0",
                             "-c:v", "copy", "-c:a", "aac", "-b:a", "320k" };
    if (sr != 44100 && sr != 48000)
        args.addArray ({ "-ar", "48000" });
    if (recovered)
        args.add ("-shortest");
    args.addArray ({ "-movflags", "+faststart", sessionOutput.getFullPathName() });

    const int code = runProcessAndWait (buildCommand (args), toWide (sessionLog.getFullPathName()), 60 * 60 * 1000);

    if (code != 0 || ! sessionOutput.existsAsFile())
    {
        sessionTempDir.getChildFile ("recovery_failed").create();   // never retried automatically
        fail ("Final MP4 failed (ffmpeg code " + juce::String (code) + ") - temp files kept, see log");
        return;
    }

    log ("saved " + sessionOutput.getFullPathName() + " (" + juce::File::descriptionOfSizeInBytes (sessionOutput.getSize()) + ")");
    sessionTempDir.deleteRecursively();

    capture.reset();
    audioFifo.reset();
    stampFifo.reset();

    if (resultText.isEmpty())
        resultText = (recovered ? "Recovered " : "Saved ") + sessionOutput.getFileName() + "  (click to open)";

    if (! syncTestMode.load())
    {
        std::lock_guard<std::mutex> g (textLock);
        lastOutputPath = sessionOutput.getFullPathName();
    }

    setStatus (resultText);
    state = State::idle;
    anyInstanceBusy = false;
}

//==============================================================================
// Alignment: place every audio sample at the moment it was heard, on the same
// clock that times the video frames. Corrects the output latency, the sync
// offset, sound-card clock drift, and gaps (dropouts) in the audio stream.

bool Recorder::renderAlignedAudio (const juce::File& rawWav, const juce::File& outWav,
                                   int64_t videoStartTicks, int64_t frameCount,
                                   int firstChannel, double timeShift)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (rawWav));

    if (reader == nullptr)
    {
        log ("cannot read raw audio");
        return false;
    }

    const double sr = sessionSampleRate;
    const int64_t totalSource = reader->lengthInSamples;
    const int64_t totalOut = (int64_t) std::llround ((double) frameCount * sr / ScreenCapture::fps);
    log ("render channels " + juce::String (firstChannel) + "-" + juce::String (firstChannel + 1)
         + ": raw samples " + juce::String (totalSource) + ", output samples " + juce::String (totalOut)
         + ", time shift " + juce::String (timeShift * 1000.0, 2) + " ms");

    // ---- time (s, relative to video frame 0) at which each block was heard
    struct Pt { double s, t; };
    std::vector<Pt> pts;
    pts.reserve (stamps.size());
    for (auto& st : stamps)
        if (st.sampleIndex < totalSource)
            pts.push_back ({ (double) st.sampleIndex,
                             Timing::toSeconds (st.ticks - videoStartTicks) + timeShift });

    // ---- split into continuous segments (a new one after any dropout)
    struct Segment { int64_t sStart, sEnd; double a, b; };    // t = a + b * s
    std::vector<Segment> segments;
    const double nominalB = 1.0 / sr;

    size_t begin = 0;
    for (size_t i = 1; i <= pts.size(); ++i)
    {
        bool split = (i == pts.size());
        if (! split)
        {
            const double predicted = pts[i - 1].t + (pts[i].s - pts[i - 1].s) * nominalB;
            split = std::abs (pts[i].t - predicted) > 0.08;
        }

        if (! split)
            continue;

        const size_t n = i - begin;
        double a, b = nominalB;
        const double span = (pts[i - 1].t - pts[begin].t);

        if (n >= 50 && span >= 5.0)
        {
            double ms = 0, mt = 0;
            for (size_t k = begin; k < i; ++k) { ms += pts[k].s; mt += pts[k].t; }
            ms /= (double) n; mt /= (double) n;
            double num = 0, den = 0;
            for (size_t k = begin; k < i; ++k)
            {
                const double ds = pts[k].s - ms;
                num += ds * (pts[k].t - mt);
                den += ds * ds;
            }
            if (den > 0)
                b = juce::jlimit (nominalB * 0.999, nominalB * 1.001, num / den);
            a = mt - b * ms;
        }
        else
        {
            double acc = 0;
            for (size_t k = begin; k < i; ++k) acc += pts[k].t - pts[k].s * nominalB;
            a = acc / (double) n;
        }

        segments.push_back ({ (int64_t) pts[begin].s, 0, a, b });
        begin = i;
    }

    for (size_t k = 0; k < segments.size(); ++k)
        segments[k].sEnd = (k + 1 < segments.size()) ? segments[k + 1].sStart : totalSource;

    for (auto& sg : segments)
        log ("segment: samples " + juce::String (sg.sStart) + ".." + juce::String (sg.sEnd)
             + ", starts at " + juce::String (sg.a + sg.b * (double) sg.sStart, 4) + " s, clock "
             + juce::String ((sg.b * sr - 1.0) * 1.0e6, 1) + " ppm");

    // ---- render
    outWav.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = std::make_unique<juce::FileOutputStream> (outWav);
    if (! stream->openedOk())
        return false;

    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sr, 2, 32, {}, 0));
    if (writer == nullptr)
        return false;
    stream.release();

    constexpr int chunk = 8192;
    const int numIn = (int) reader->numChannels;
    juce::AudioBuffer<float> in (juce::jmax (numIn, firstChannel + 2), chunk * 2 + 64), out (2, chunk);
    int64_t outPos = 0;
    const double interpLatency = (double) juce::WindowedSincInterpolator::getBaseLatency();

    auto writeSilence = [&] (int64_t count)
    {
        out.clear();
        while (count > 0)
        {
            const int n = (int) juce::jmin<int64_t> (count, chunk);
            writer->writeFromAudioSampleBuffer (out, 0, n);
            count -= n;
            outPos += n;
        }
    };

    for (auto& sg : segments)
    {
        if (outPos >= totalOut)
            break;

        const double ratio = 1.0 / (sg.b * sr);       // source samples per output sample
        const int64_t target = (int64_t) std::llround ((sg.a + sg.b * (double) sg.sStart) * sr - interpLatency);
        int64_t src = sg.sStart;

        if (target > outPos)
            writeSilence (juce::jmin (target, totalOut) - outPos);
        else
            src += (int64_t) std::llround ((double) (outPos - target) * ratio);

        juce::WindowedSincInterpolator interp[2];

        while (outPos < totalOut && src < sg.sEnd)
        {
            const int64_t remainingOut = (int64_t) ((double) (sg.sEnd - src) / ratio);
            const int n = (int) juce::jmin ((int64_t) chunk, totalOut - outPos, remainingOut);
            if (n <= 0)
                break;

            const int need = (int) std::ceil (n * ratio) + 8;
            in.clear();
            reader->read (in.getArrayOfWritePointers(), numIn, src, need);

            int used = 0;
            for (int ch = 0; ch < 2; ++ch)
                used = interp[ch].process (ratio, in.getReadPointer (firstChannel + ch), out.getWritePointer (ch), n);

            writer->writeFromAudioSampleBuffer (out, 0, n);
            outPos += n;
            src += used;
        }
    }

    if (outPos < totalOut)
        writeSilence (totalOut - outPos);

    writer.reset();
    return true;
}

//==============================================================================
// Sync test analysis: compare when each flash appears in the video with when
// its beep appears in the aligned audio.

double Recorder::analyseSyncTest (const juce::File& alignedWav, const std::vector<float>& probe, int& pairsFound)
{
    pairsFound = 0;

    // ---- video flash onsets
    std::vector<double> flashes;
    if (! probe.empty())
    {
        const auto [mn, mx] = std::minmax_element (probe.begin(), probe.end());
        log ("probe brightness range " + juce::String (*mn, 3) + " .. " + juce::String (*mx, 3));

        if (*mx - *mn > 0.25f)
        {
            const float thr = 0.5f * (*mn + *mx);
            for (size_t n = 1; n < probe.size(); ++n)
                if (probe[n] >= thr && probe[n - 1] < thr)
                    flashes.push_back (((double) n - 0.5) / ScreenCapture::fps);   // happened between frames n-1 and n
        }
    }

    // ---- audio beep onsets
    std::vector<double> beeps;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (alignedWav));

    if (reader != nullptr)
    {
        const double sr = reader->sampleRate;
        juce::AudioBuffer<float> buf (2, 16384);
        int64_t lastOnset = -(int64_t) sr;

        for (int64_t pos = 0; pos < reader->lengthInSamples; pos += buf.getNumSamples())
        {
            const int n = (int) juce::jmin<int64_t> (buf.getNumSamples(), reader->lengthInSamples - pos);
            reader->read (&buf, 0, n, pos, true, true);
            for (int i = 0; i < n; ++i)
            {
                const float v = juce::jmax (std::abs (buf.getSample (0, i)), std::abs (buf.getSample (1, i)));
                if (v > 0.2f && pos + i - lastOnset > (int64_t) (0.3 * sr))
                {
                    beeps.push_back ((double) (pos + i) / sr);
                    lastOnset = pos + i;
                }
            }
        }
    }

    log ("flashes found: " + juce::String ((int) flashes.size()) + ", beeps found: " + juce::String ((int) beeps.size()));

    std::vector<double> diffs;
    for (double f : flashes)
    {
        double best = 1.0e9;
        for (double b : beeps)
            if (std::abs (f - b) < std::abs (best))
                best = f - b;

        if (std::abs (best) < 0.4)
        {
            diffs.push_back (best);
            log ("  flash " + juce::String (f, 3) + " s  vs beep: " + juce::String (best * 1000.0, 1) + " ms");
        }
    }

    pairsFound = (int) diffs.size();
    return median (diffs);
}

//==============================================================================

void Recorder::loadSyncOffset()
{
    const auto f = getOutputFolder().getChildFile ("sync_offset_ms.txt");
    if (f.existsAsFile())
        syncOffsetMs = f.loadFileAsString().trim().getDoubleValue();
}

void Recorder::saveSyncOffset (double ms)
{
    syncOffsetMs = ms;
    const auto f = getOutputFolder().getChildFile ("sync_offset_ms.txt");
    f.replaceWithText (juce::String (ms, 1));
}

//==============================================================================
// Settings (mode + REELS frame), stored next to the recordings.

namespace
{
    juce::File settingsFile() { return Recorder::getOutputFolder().getChildFile ("settings.txt"); }
}

void Recorder::setMode (Mode m)
{
    if (isBusy())
        return;

    mode = (int) m;
    saveSettings();
}

void Recorder::setFrame (Mode m, double cx, double cy, double h)
{
    auto& f = frames[(int) m];
    f[0] = juce::jlimit (0.0, 1.0, cx);
    f[1] = juce::jlimit (0.0, 1.0, cy);
    f[2] = h < 0.0 ? -1.0 : juce::jlimit (0.05, 1.0, h);
}

void Recorder::getFrame (Mode m, double& cx, double& cy, double& h) const
{
    const auto& f = frames[(int) m];
    cx = f[0].load();
    cy = f[1].load();
    h  = f[2].load();
}

void Recorder::loadSettings()
{
    const auto f = settingsFile();
    if (! f.existsAsFile())
        return;

    juce::StringArray lines;
    f.readLines (lines);

    double v[2][3] = { { 0.5, 0.5, -1.0 }, { 0.5, 0.5, -1.0 } };
    for (auto& l : lines)
    {
        const auto key = l.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto val = l.fromFirstOccurrenceOf ("=", false, false).trim();

        if (key == "mode") mode = (val == "reels") ? 1 : 0;
        if (key == "quality") quality = juce::jlimit (0, 3, val.getIntValue());
        if (key == "show_frame") frameVisible = val.getIntValue() != 0;
        if (key == "mic_gain_db") setMicGainDb ((float) val.getDoubleValue());
        if (key == "duck") duckEnabled = val.getIntValue() != 0;
        if (key == "clicks") clickEnabled = val.getIntValue() != 0;
        if (key == "ui_width") uiWidth = juce::jlimit (480, 960, val.getIntValue());

        for (int m = 0; m < 2; ++m)
        {
            const juce::String pre = m == 0 ? "youtube_" : "reels_";
            if (key == pre + "cx") v[m][0] = val.getDoubleValue();
            if (key == pre + "cy") v[m][1] = val.getDoubleValue();
            if (key == pre + "h")  v[m][2] = val.getDoubleValue();
        }
    }

    for (int m = 0; m < 2; ++m)
        setFrame ((Mode) m, v[m][0], v[m][1], v[m][2]);
}

void Recorder::saveSettings()
{
    getOutputFolder().createDirectory();

    juce::String t;
    t << "mode=" << (getMode() == Mode::reels ? "reels" : "youtube") << "\r\n"
      << "quality=" << quality.load() << "\r\n"
      << "show_frame=" << (frameVisible.load() ? 1 : 0) << "\r\n"
      << "mic_gain_db=" << juce::String (micGainDb.load(), 1) << "\r\n"
      << "duck=" << (duckEnabled.load() ? 1 : 0) << "\r\n"
      << "clicks=" << (clickEnabled.load() ? 1 : 0) << "\r\n"
      << "ui_width=" << uiWidth.load() << "\r\n";

    for (int m = 0; m < 2; ++m)
    {
        const juce::String pre = m == 0 ? "youtube_" : "reels_";
        t << pre << "cx=" << juce::String (frames[m][0].load(), 5) << "\r\n"
          << pre << "cy=" << juce::String (frames[m][1].load(), 5) << "\r\n"
          << pre << "h="  << juce::String (frames[m][2].load(), 5) << "\r\n";
    }

    settingsFile().replaceWithText (t);
}

//==============================================================================

bool Recorder::mixWavFiles (const juce::File& a, const juce::File& b, const juce::File& out, bool duck)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> ra (fm.createReaderFor (a)), rb (fm.createReaderFor (b));
    if (ra == nullptr || rb == nullptr)
        return false;

    out.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = std::make_unique<juce::FileOutputStream> (out);
    if (! stream->openedOk())
        return false;

    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (stream.get(), ra->sampleRate, 2, 32, {}, 0));
    if (w == nullptr)
        return false;
    stream.release();

    const int64_t len = juce::jmax (ra->lengthInSamples, rb->lengthInSamples);
    juce::AudioBuffer<float> ba (2, 16384), bb (2, 16384);

    // Ducking: while the voice is present the music drops by 10 dB
    // (fast down, holds 400 ms between words, comes back over ~0.8 s). Video only.
    const double sr = ra->sampleRate > 0 ? ra->sampleRate : 48000.0;
    const float duckGain = juce::Decibels::decibelsToGain (-10.0f);
    const float threshold = juce::Decibels::decibelsToGain (-40.0f);
    const float envAttack  = (float) (1.0 - std::exp (-1.0 / (0.005 * sr)));
    const float envRelease = (float) (1.0 - std::exp (-1.0 / (0.150 * sr)));
    const float gDown = (float) (1.0 - std::exp (-1.0 / (0.060 * sr)));
    const float gUp   = (float) (1.0 - std::exp (-1.0 / (0.800 * sr)));
    const int holdSamples = (int) (0.40 * sr);
    int64_t duckedSamples = 0;
    float env = 0.0f, gain = 1.0f;
    int hold = 0;

    for (int64_t pos = 0; pos < len; pos += 16384)
    {
        const int n = (int) juce::jmin<int64_t> (16384, len - pos);
        ba.clear(); bb.clear();
        ra->read (&ba, 0, n, pos, true, true);
        rb->read (&bb, 0, n, pos, true, true);

        if (duck)
        {
            float* l = ba.getWritePointer (0);
            float* r = ba.getWritePointer (1);
            const float* ml = bb.getReadPointer (0);
            const float* mr = bb.getReadPointer (1);

            for (int i = 0; i < n; ++i)
            {
                const float x = juce::jmax (std::abs (ml[i]), std::abs (mr[i]));
                env += (x - env) * (x > env ? envAttack : envRelease);

                if (env > threshold) hold = holdSamples;
                else if (hold > 0)   --hold;

                const float target = hold > 0 ? duckGain : 1.0f;
                gain += (target - gain) * (target < gain ? gDown : gUp);

                l[i] *= gain;
                r[i] *= gain;
                if (gain < 0.9f)
                    ++duckedSamples;
            }
        }

        for (int ch = 0; ch < 2; ++ch)
            ba.addFrom (ch, 0, bb, ch, 0, n);
        w->writeFromAudioSampleBuffer (ba, 0, n);
    }

    if (duck)
        log ("duck: on (-10 dB), music lowered during " + juce::String (len > 0 ? 100.0 * (double) duckedSamples / (double) len : 0.0, 1)
             + "% of the video");
    else
        log ("duck: off");

    return true;
}

//==============================================================================
// Quality

void Recorder::setQuality (int q)
{
    if (isBusy())
        return;

    quality = juce::jlimit (0, 3, q);
    saveSettings();
}

void Recorder::outputSize (Mode m, int q, int& w, int& h)
{
    static const int heights[] = { 480, 720, 1080, 2160 };
    static const int widths[]  = { 854, 1280, 1920, 3840 };
    q = juce::jlimit (0, 3, q);
    if (m == Mode::reels && q > 2)
        q = 2;

    if (m == Mode::reels) { w = heights[q]; h = widths[q]; }
    else                  { w = widths[q];  h = heights[q]; }
}

int Recorder::bitrateKbps (int q)
{
    static const int kbps[] = { 2500, 5000, 8000, 30000 };
    return kbps[juce::jlimit (0, 3, q)];
}

//==============================================================================
// Test beeps. Sync test: steady. Mic test: irregular, so the measured gap can
// never be confused with a whole beep interval.

double Recorder::testBeepTime (int k) const
{
    static const double micTimes[] = { 0.0, 0.7, 1.8, 2.7, 4.0, 4.8, 6.0, 7.0, 8.4 };

    if (micTestMode.load())
        return micTimes[juce::jlimit (0, 8, k)];

    return k * testFlashInterval;
}

int Recorder::testBeepCount() const
{
    return micTestMode.load() ? 9 : testFlashCount;
}

//==============================================================================
// Mic test

void Recorder::runFinishMicTest()
{
    const int64_t t0 = captureStartTicks.load();
    const int64_t frames = juce::jmax<int64_t> (1, (int64_t) (Timing::toSeconds (Timing::now() - t0) * ScreenCapture::fps));

    audioActive.store (false, std::memory_order_release);
    std::this_thread::sleep_for (std::chrono::milliseconds (150));

    drainRunning = false;
    if (drainThread.joinable())
        drainThread.join();
    drainOnce();
    rawWriter.reset();

    setStatus ("Mic test: measuring...");

    const double blockTime = (double) sessionBlockSize / sessionSampleRate;
    const double offset = syncOffsetMs.load() / 1000.0;
    const double oldMic = micOffsetMs.load() / 1000.0;

    const auto raw = sessionTempDir.getChildFile ("audio_raw.wav");
    const auto master = sessionTempDir.getChildFile ("audio_master.wav");
    const auto mic = sessionTempDir.getChildFile ("audio_mic.wav");

    juce::String result;

    if (sessionMicPeak < 0.003f)
    {
        result = "Mic test failed: the mic heard nothing. Hold the headphones right on the mic and turn them up.";
    }
    else if (! renderAlignedAudio (raw, master, t0, frames, 0, blockTime + offset)
             || ! renderAlignedAudio (raw, mic, t0, frames, 2, -blockTime + oldMic))
    {
        result = "Mic test failed - see log";
    }
    else
    {
        int pairs = 0;
        const double gap = analyseMicTest (master, mic, pairs);

        if (pairs >= 5)
        {
            const double newMs = juce::jlimit (-3000.0, 3000.0, (oldMic + gap) * 1000.0);
            micOffsetMs = newMs;
            getOutputFolder().getChildFile ("mic_offset_ms.txt").replaceWithText (juce::String (newMs, 1));
            log ("mic test: gap " + juce::String (gap * 1000.0, 1) + " ms from " + juce::String (pairs)
                 + " beeps -> mic offset " + juce::String (newMs, 1) + " ms");
            result = "Mic fixed: was off by " + juce::String (gap * 1000.0, 0) + " ms, mic offset now "
                   + juce::String (newMs, 0) + " ms";
        }
        else
        {
            result = "Mic test failed (" + juce::String (pairs) + "/9 beeps heard). Hold the headphones on the mic, stay quiet, try again.";
        }
    }

    sessionTempDir.deleteRecursively();
    audioFifo.reset();
    stampFifo.reset();

    setStatus (result);
    state = State::idle;
    anyInstanceBusy = false;
}

double Recorder::analyseMicTest (const juce::File& masterWav, const juce::File& micWav, int& pairsFound)
{
    pairsFound = 0;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    auto findOnsets = [&] (const juce::File& f, bool relative) -> std::vector<double>
    {
        std::vector<double> onsets;
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr)
            return onsets;

        const double sr = r->sampleRate;
        float threshold = 0.2f;

        if (relative)
        {
            float peak = 0.0f;
            juce::AudioBuffer<float> b (2, 16384);
            for (int64_t pos = 0; pos < r->lengthInSamples; pos += 16384)
            {
                const int n = (int) juce::jmin<int64_t> (16384, r->lengthInSamples - pos);
                r->read (&b, 0, n, pos, true, true);
                peak = juce::jmax (peak, b.getMagnitude (0, n));
            }
            threshold = peak * 0.3f;
        }

        juce::AudioBuffer<float> b (2, 16384);
        int64_t last = -(int64_t) sr;
        for (int64_t pos = 0; pos < r->lengthInSamples; pos += 16384)
        {
            const int n = (int) juce::jmin<int64_t> (16384, r->lengthInSamples - pos);
            r->read (&b, 0, n, pos, true, true);
            for (int i = 0; i < n; ++i)
            {
                const float v = juce::jmax (std::abs (b.getSample (0, i)), std::abs (b.getSample (1, i)));
                if (v > threshold && pos + i - last > (int64_t) (0.35 * sr))
                {
                    onsets.push_back ((double) (pos + i) / sr);
                    last = pos + i;
                }
            }
        }
        return onsets;
    };

    const auto beeps = findOnsets (masterWav, false);
    const auto heard = findOnsets (micWav, true);
    log ("mic test: beeps " + juce::String ((int) beeps.size()) + ", mic onsets " + juce::String ((int) heard.size()));

    // Find the one delay that lines up the most beeps with mic onsets.
    std::vector<double> diffs;
    for (double b : beeps)
        for (double h : heard)
            if (h - b > -0.5 && h - b < 2.5)
                diffs.push_back (h - b);

    double bestLag = 0.0;
    int bestCount = 0;
    for (double d : diffs)
    {
        int c = 0;
        for (double e : diffs)
            if (std::abs (e - d) < 0.03)
                ++c;
        if (c > bestCount) { bestCount = c; bestLag = d; }
    }

    std::vector<double> matched;
    for (double d : diffs)
        if (std::abs (d - bestLag) < 0.03)
            matched.push_back (d);

    pairsFound = (int) matched.size();
    return -median (matched);   // mic is late by median -> move it earlier
}

//==============================================================================
// Crash recovery
//
// While recording, the temp folder holds everything needed to finish the file
// later: video.mkv (ffmpeg closes it cleanly when our process dies), the raw
// 4-channel WAV (its header is repaired), the block stamps and session.txt.

void Recorder::writeSessionInfo()
{
    juce::String t;
    t << "version=1\r\n"
      << "sample_rate=" << juce::String (sessionSampleRate, 3) << "\r\n"
      << "block_size=" << sessionBlockSize << "\r\n"
      << "t0=" << juce::String ((juce::int64) captureStartTicks.load()) << "\r\n"
      << "output=" << sessionOutput.getFullPathName() << "\r\n";
    sessionTempDir.getChildFile ("session.txt").replaceWithText (t);
}

bool Recorder::repairWavHeader (const juce::File& wav)
{
    const juce::int64 fileSize = wav.getSize();
    if (fileSize < 64)
        return false;

    juce::MemoryBlock head;
    {
        juce::FileInputStream in (wav);
        if (! in.openedOk())
            return false;
        in.readIntoMemoryBlock (head, juce::jmin<juce::int64> (fileSize, 65536));
    }

    auto* d = static_cast<const char*> (head.getData());
    if (head.getSize() < 12 || std::memcmp (d, "RIFF", 4) != 0 || std::memcmp (d + 8, "WAVE", 4) != 0)
        return false;

    size_t pos = 12;
    while (pos + 8 <= head.getSize())
    {
        const auto chunkSize = (juce::uint32) juce::ByteOrder::littleEndianInt (d + pos + 4);

        if (std::memcmp (d + pos, "data", 4) == 0)
        {
            const juce::int64 dataStart = (juce::int64) pos + 8;
            juce::int64 dataBytes = ((fileSize - dataStart) / 16) * 16;   // 4 ch x 32-bit float
            dataBytes = juce::jmin<juce::int64> (dataBytes, 0xFFFFFF00LL);

            juce::FileOutputStream out (wav);
            if (! out.openedOk())
                return false;

            out.setPosition (4);
            out.writeInt ((int) (juce::uint32) juce::jmin<juce::int64> (dataStart + dataBytes - 8, 0xFFFFFFF0LL));
            out.setPosition ((juce::int64) pos + 4);
            out.writeInt ((int) (juce::uint32) dataBytes);
            out.flush();
            return true;
        }

        pos += 8 + chunkSize + (chunkSize & 1);
    }

    return false;
}

void Recorder::checkForRecovery()
{
    if (isBusy())
        return;

    const auto tempRoot = getOutputFolder().getChildFile (".temp");
    if (! tempRoot.isDirectory())
        return;

    for (const auto& entry : juce::RangedDirectoryIterator (tempRoot, false, "*", juce::File::findDirectories))
    {
        const auto dir = entry.getFile();
        const auto video = dir.getChildFile ("video.mkv");

        if (! dir.getChildFile ("session.txt").existsAsFile()
            || dir.getChildFile ("recovery_failed").existsAsFile()
            || ! video.existsAsFile()
            || ! dir.getChildFile ("audio_raw.wav").existsAsFile())
            continue;

        // ffmpeg may still be finishing the file: wait until it has been quiet for a while.
        if (juce::Time::getCurrentTime() - video.getLastModificationTime() < juce::RelativeTime::seconds (20))
            continue;

        bool expected = false;
        if (! anyInstanceBusy.compare_exchange_strong (expected, true))
            return;

        joinWorker();
        state = State::saving;
        syncTestMode = false;
        micTestMode = false;
        setStatus ("Recovering an unfinished recording...");

        workerBusy = true;
        worker = std::thread ([this, dir]
        {
            runRecovery (dir);
            workerBusy = false;
        });
        return;
    }
}

void Recorder::runRecovery (juce::File dir)
{
    juce::StringPairArray info;
    {
        juce::StringArray lines;
        dir.getChildFile ("session.txt").readLines (lines);
        for (auto& l : lines)
            if (l.contains ("="))
                info.set (l.upToFirstOccurrenceOf ("=", false, false).trim(), l.fromFirstOccurrenceOf ("=", false, false).trim());
    }

    sessionTempDir = dir;
    sessionName = dir.getFileName() + "_recovered";
    sessionOutput = getOutputFolder().getChildFile (sessionName + ".mp4");
    sessionLog = getOutputFolder().getChildFile ("logs").getChildFile (sessionName + ".log");
    sessionLog.getParentDirectory().createDirectory();
    sessionSampleRate = info["sample_rate"].getDoubleValue() > 0 ? info["sample_rate"].getDoubleValue() : 48000.0;
    sessionBlockSize = juce::jmax (1, info["block_size"].getIntValue());
    const int64_t t0 = (int64_t) info["t0"].getLargeIntValue();

    log ("recovering " + dir.getFullPathName());

    // block stamps
    stamps.clear();
    {
        juce::MemoryBlock mb;
        dir.getChildFile ("stamps.bin").loadFileAsData (mb);
        const size_t n = mb.getSize() / sizeof (BlockStamp);
        stamps.resize (n);
        if (n > 0)
            std::memcpy (stamps.data(), mb.getData(), n * sizeof (BlockStamp));
    }

    const bool wavOk = repairWavHeader (dir.getChildFile ("audio_raw.wav"));

    // video length: from frames.txt, else from the last audio stamp
    int64_t frames = (int64_t) dir.getChildFile ("frames.txt").loadFileAsString().trim().getLargeIntValue();
    if (frames <= 0 && ! stamps.empty())
        frames = (int64_t) (Timing::toSeconds (stamps.back().ticks - t0) * ScreenCapture::fps);

    log ("stamps " + juce::String ((int) stamps.size()) + ", frames ~" + juce::String (frames) + (wavOk ? "" : ", WAV header not repaired"));

    // mic peak: scan the mic channels so the mic is only mixed when present
    sessionMicPeak = 0.0f;
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (dir.getChildFile ("audio_raw.wav")));
        if (r != nullptr && r->numChannels >= 4)
        {
            juce::Range<float> lv[4];
            r->readMaxLevels (0, r->lengthInSamples, lv, 4);
            sessionMicPeak = juce::jmax (lv[2].getEnd(), -lv[2].getStart(), lv[3].getEnd(), -lv[3].getStart());
        }
    }

    if (stamps.empty() || frames <= 0)
    {
        dir.getChildFile ("recovery_failed").create();
        fail ("Could not recover the unfinished recording (files kept in .temp)");
        return;
    }

    produceOutput (t0, frames, {}, true);

    if (state.load() == State::error)
        dir.getChildFile ("recovery_failed").create();
}
