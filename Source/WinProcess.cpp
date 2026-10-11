#include "WinProcess.h"

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <vector>

WinProcess::~WinProcess()
{
    if (isRunning())
        kill();

    closeStdin();

    if (process != nullptr)   CloseHandle ((HANDLE) process);
    if (logHandle != nullptr) CloseHandle ((HANDLE) logHandle);
}

bool WinProcess::start (const std::wstring& commandLine, bool withStdin, const std::wstring& logFilePath)
{
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof (sa);
    sa.bInheritHandle = TRUE;

    HANDLE log = INVALID_HANDLE_VALUE;
    if (! logFilePath.empty())
        log = CreateFileW (logFilePath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (log == INVALID_HANDLE_VALUE)
        log = CreateFileW (L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    logHandle = log;

    HANDLE childStdinRead = nullptr, childStdinWrite = nullptr;

    if (withStdin)
    {
        // Large pipe buffer: one raw 1080p frame is ~8 MB.
        if (! CreatePipe (&childStdinRead, &childStdinWrite, &sa, 16 * 1024 * 1024))
            return false;

        SetHandleInformation (childStdinWrite, HANDLE_FLAG_INHERIT, 0);
    }
    else
    {
        childStdinRead = CreateFileW (L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
    }

    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = childStdinRead;
    si.hStdOutput = log;
    si.hStdError  = log;

    PROCESS_INFORMATION pi {};
    std::vector<wchar_t> cmd (commandLine.begin(), commandLine.end());
    cmd.push_back (0);

    const BOOL ok = CreateProcessW (nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW,
                                    nullptr, nullptr, &si, &pi);

    if (childStdinRead != nullptr && childStdinRead != INVALID_HANDLE_VALUE)
        CloseHandle (childStdinRead);

    if (! ok)
    {
        if (childStdinWrite != nullptr) CloseHandle (childStdinWrite);
        return false;
    }

    CloseHandle (pi.hThread);
    process = pi.hProcess;
    stdinWrite = childStdinWrite;
    return true;
}

bool WinProcess::write (const void* data, size_t numBytes)
{
    if (stdinWrite == nullptr)
        return false;

    auto* p = static_cast<const char*> (data);

    while (numBytes > 0)
    {
        DWORD chunk = (DWORD) (numBytes > (1u << 24) ? (1u << 24) : numBytes);
        DWORD written = 0;

        if (! WriteFile ((HANDLE) stdinWrite, p, chunk, &written, nullptr) || written == 0)
            return false;

        p += written;
        numBytes -= written;
    }

    return true;
}

void WinProcess::closeStdin()
{
    if (stdinWrite != nullptr)
    {
        CloseHandle ((HANDLE) stdinWrite);
        stdinWrite = nullptr;
    }
}

bool WinProcess::waitForExit (int timeoutMs)
{
    if (process == nullptr)
        return true;

    return WaitForSingleObject ((HANDLE) process, (DWORD) timeoutMs) == WAIT_OBJECT_0;
}

int WinProcess::exitCode() const
{
    if (process == nullptr)
        return code;

    DWORD c = 0;
    if (GetExitCodeProcess ((HANDLE) process, &c) && c != STILL_ACTIVE)
        code = (int) c;

    return code;
}

bool WinProcess::isRunning() const
{
    if (process == nullptr)
        return false;

    DWORD c = 0;
    return GetExitCodeProcess ((HANDLE) process, &c) && c == STILL_ACTIVE;
}

void WinProcess::kill()
{
    if (process != nullptr)
    {
        TerminateProcess ((HANDLE) process, 1);
        WaitForSingleObject ((HANDLE) process, 2000);
    }
}

int runProcessAndWait (const std::wstring& commandLine, const std::wstring& logFilePath, int timeoutMs)
{
    WinProcess p;

    if (! p.start (commandLine, false, logFilePath))
        return -1;

    if (! p.waitForExit (timeoutMs))
    {
        p.kill();
        return -1;
    }

    return p.exitCode();
}

#else  // non-Windows stubs (the plugin only targets Windows)

WinProcess::~WinProcess() {}
bool WinProcess::start (const std::wstring&, bool, const std::wstring&) { return false; }
bool WinProcess::write (const void*, size_t) { return false; }
void WinProcess::closeStdin() {}
bool WinProcess::waitForExit (int) { return true; }
int  WinProcess::exitCode() const { return code; }
bool WinProcess::isRunning() const { return false; }
void WinProcess::kill() {}
int runProcessAndWait (const std::wstring&, const std::wstring&, int) { return -1; }

#endif
