#pragma once

// Small Win32 helpers kept out of the JUCE translation units.
namespace WinUtils
{
    // Top-level window that contains the given native window handle.
    void* getTopLevelWindow (void* hwnd);

    // Physical-pixel size of a window. Returns false if unknown.
    bool getWindowSize (void* hwnd, int& width, int& height);

    // Keep the window visible on the monitor but leave it out of any screen
    // capture (Windows 10 2004+). Returns false if the system refused.
    bool setExcludedFromCapture (void* hwnd, bool exclude);

    // Mouse clicks pass straight through the window to whatever is below it.
    void makeClickThrough (void* hwnd);

    // Clicking the window does not take focus away from Cubase.
    void makeNoActivate (void* hwnd);
}
