#include "WinUtils.h"

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>

 #ifndef WDA_EXCLUDEFROMCAPTURE
  #define WDA_EXCLUDEFROMCAPTURE 0x00000011
 #endif

namespace WinUtils
{
    void* getTopLevelWindow (void* hwnd)
    {
        return hwnd != nullptr ? (void*) GetAncestor ((HWND) hwnd, GA_ROOT) : nullptr;
    }

    bool getWindowSize (void* hwnd, int& width, int& height)
    {
        RECT r {};
        if (hwnd == nullptr || ! GetWindowRect ((HWND) hwnd, &r))
            return false;

        width = (int) (r.right - r.left);
        height = (int) (r.bottom - r.top);
        return true;
    }

    bool setExcludedFromCapture (void* hwnd, bool exclude)
    {
        if (hwnd == nullptr)
            return false;

        return SetWindowDisplayAffinity ((HWND) hwnd, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) != FALSE;
    }

    void makeClickThrough (void* hwnd)
    {
        if (hwnd == nullptr)
            return;

        const auto ex = GetWindowLongPtrW ((HWND) hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW ((HWND) hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE);
    }

    void makeNoActivate (void* hwnd)
    {
        if (hwnd == nullptr)
            return;

        const auto ex = GetWindowLongPtrW ((HWND) hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW ((HWND) hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
    }
}

#else

namespace WinUtils
{
    void* getTopLevelWindow (void* hwnd) { return hwnd; }
    bool getWindowSize (void*, int&, int&) { return false; }
    bool setExcludedFromCapture (void*, bool) { return false; }
    void makeClickThrough (void*) {}
    void makeNoActivate (void*) {}
}

#endif
