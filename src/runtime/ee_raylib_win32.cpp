#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Raylib and windows.h both declare CloseWindow and Rectangle. Keep the
// Win32-only declarations out of the raylib translation unit.
extern "C" void* ee_win32_window_instance(void* window_handle)
{
    const HWND hwnd = static_cast<HWND>(window_handle);
    return reinterpret_cast<void*>(
        GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
}
