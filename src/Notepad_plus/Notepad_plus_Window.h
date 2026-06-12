#pragma once
#include "Notepad_plus.h"
#include "../Parameters/Parameters.h"
#include "../ScintillaComponent/Buffer.h"
#include <windows.h>
#include <vector>

namespace npp {

// The top-level frame window. Hosts menu, status bar, and the Notepad_plus core.
class Notepad_plus_Window
{
public:
    bool Init(HINSTANCE hInst, int nCmdShow);
    int  MessageLoop();
    HACCEL Accelerators() const { return accel_; }
    HWND Hwnd() const { return hwnd_; }

    // Open a file path from the command line (e.g. shell association).
    // Resolves to an absolute path; returns false only if the file cannot be opened.
    bool OpenFromCommandLine(const std::wstring& path);

    // Set before Init(): command-line file args. When non-empty, the frame
    // opens these instead of creating the default "new 1" tab on startup.
    void SetInitialFiles(std::vector<std::wstring> files) { initialFiles_ = std::move(files); }

    // Single-instance IPC: WM_COPYDATA dwData tag and the window class a
    // second launcher looks for. The payload is a wchar_t buffer with paths
    // separated by '\n' (no trailing terminator required — cbData bounds it).
    static constexpr ULONG_PTR kOpenFilesMsgId = 0x4E505046; // 'NPPF'
    static const wchar_t* ClassName();

    // Debounce window for the post-edit whole-buffer restyle (function-name
    // scan / markdown fence pass). Armed by SCN_MODIFIED on user edits and
    // consumed in WM_TIMER; running the scan on every keystroke visibly
    // slows typing on ~80k+ files.
    static constexpr UINT_PTR kStyleDebounceTimerId = 0x4E505053; // 'NPPS'
    static constexpr UINT     kStyleDebounceMs      = 200;

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void OnCreate(HWND h);
    void OnSize(HWND h);
    void RebuildRecentMenu();
    void BuildLanguageMenu();
    void UpdateCheckedMenus();
    void CopyPathToClipboard(BufferID id);
    void OpenContainingFolder(BufferID id);
    void ShowGoToLineDialog();
    void WireTabContextForView(int v);
    void ShowTextComparePicker();
    void ShowHexComparePicker();
    // Run the debounced whole-doc restyle on the active view's dirty buffer.
    void FlushPendingStyle();

    void CreateToolbar();
    void RebuildToolbar();
    int  ToolbarHeight() const;
    void ApplyCurrentTheme();
    void SetTheme(ThemeId t);
    void CycleTheme();

    HINSTANCE      hInst_     = nullptr;
    HWND           hwnd_      = nullptr;
    HWND           statusBar_ = nullptr;
    HWND           toolbar_   = nullptr;
    void*          tbImgs_    = nullptr;   // HIMAGELIST
    HACCEL         accel_     = nullptr;
    HMENU          menu_      = nullptr;
    Notepad_plus   app_;
    BufferID       ctxMenuBuffer_ = kInvalidBufferID;
    std::vector<std::wstring> initialFiles_;
};

} // namespace npp
