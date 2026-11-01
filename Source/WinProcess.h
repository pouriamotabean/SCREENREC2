#pragma once
#include <string>
#include <cstddef>
#include <atomic>

// Minimal child process with a writable stdin pipe and stderr/stdout sent to a
// log file. Runs hidden (no console window). Used to drive ffmpeg.exe.
class WinProcess
{
public:
    WinProcess() = default;
    ~WinProcess();

    bool start (const std::wstring& commandLine, bool withStdin, const std::wstring& logFilePath);

    // Blocking write to the child's stdin. Returns false if the pipe broke.
    bool write (const void* data, size_t numBytes);
    void closeStdin();

    // Returns true if the process has exited within timeoutMs.
    bool waitForExit (int timeoutMs);
    int  exitCode() const;
    bool isRunning() const;
    void kill();

private:
    void* process   = nullptr;
    void* stdinWrite = nullptr;
    void* logHandle = nullptr;
    mutable int code = -1;

    WinProcess (const WinProcess&) = delete;
    WinProcess& operator= (const WinProcess&) = delete;
};

// Runs a command to completion (hidden). Returns the exit code, or -1 on
// failure / timeout / cancel (the process is killed on timeout or cancel).
// If 'cancel' becomes true while waiting, the child is killed right away.
int runProcessAndWait (const std::wstring& commandLine, const std::wstring& logFilePath, int timeoutMs,
                       const std::atomic<bool>* cancel = nullptr);
