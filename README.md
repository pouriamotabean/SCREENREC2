# ScreenRec — BY POURIA MOTABEAN

VST3 for the end of Cubase's master insert rack. One button records the whole
screen (1080p, 30 fps, H.264 ~8 Mbps, GPU encoder when available) together with
exactly what you hear on the master bus (music + monitored mic), into one MP4
with AAC 320 kbps audio.

- Output: `Documents\ScreenRec\YouTube_YYYY-MM-DD_HH-MM-SS.mp4` (1920x1080) or `Reels_....mp4` (1080x1920)
- Logs: `Documents\ScreenRec\logs\`
- Sync offset (set by SYNC TEST): `Documents\ScreenRec\sync_offset_ms.txt`
- Frame positions and mode: `Documents\ScreenRec\settings.txt`

## YOUTUBE / REELS frame
Both modes show a movable frame on screen with a centre marker: 16:9 for
YOUTUBE, 9:16 for REELS. Top-left handle = move (mouse wheel = zoom,
double-click = default size), top-right handle = zoom around the centre. Works
live while recording; the video glides after the frame like a camera. Default
YOUTUBE frame = largest 16:9 area between the window title bar and the taskbar.
The frame, handles and the plugin window are visible on the monitor but not in
the recording (Windows 10 2004+). Clicks inside the frame go through to Cubase.

## Mic via side-chain (no headphone latency)
Keep ScreenRec on Stereo Out. Activate its side-chain, give the mic track a
send to "ScreenRec - Side-Chain" and set the mic track output to "No Bus". The
mic is recorded into the video (placed at the moment you spoke) but never sent
to the output. The MIC meter in the plugin shows that it arrives.

## Install
Download the `ScreenRec-VST3` artifact from GitHub Actions, unzip, copy
`ScreenRec.vst3` (the whole folder, it contains ffmpeg.exe) to
`C:\Program Files\Common Files\VST3\`, rescan in Cubase.

## How sync works
- Screen frames are captured on a fixed 30 fps grid of the high-resolution
  system clock; late frames are repeated, so the video never drifts.
- Every audio block is time-stamped on the same clock. After STOP the audio is
  placed at the moment it was heard (ASIO buffer latency compensated), sound-card
  clock drift is corrected by resampling, and dropouts become silence.
- SYNC TEST (transport stopped, plugin window visible): 10 flashes + beeps,
  measured from the recorded file; the offset is stored and used from then on.

## Build
GitHub Actions (windows-latest), JUCE 8.0.4 via CMake FetchContent.
