#pragma once
#include "FolderCompareView.h"
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>

namespace npp {

// Singleton top-level Folder Compare frame.
// Layout (top-down):
//   Row 1:   Left:  [edit.......][...]   Right: [edit.......][...]
//   Row 2:   [Recursive]  [Compare]          status summary
//   Body:    self-drawn FolderCompareView
//   Bottom:  status bar
class FolderCompareWindow
{
public:
    static void Open(HWND owner, HINSTANCE hInst);

    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);

private:
    FolderCompareWindow() = default;
    ~FolderCompareWindow() = default;

    void BuildChildren(HINSTANCE hInst);
    void Layout();
    void BrowseInto(HWND edit);
    void RunCompare();
    void DeliverResults(std::vector<FolderEntry>* entries);
    void UpdateStatus(size_t total, size_t same, size_t diff, size_t leftOnly, size_t rightOnly);
    void OnActivateEntry(int row);

    // Posted from the worker thread when the scan completes.
    static constexpr UINT kMsgScanDone = WM_APP + 1;

    HWND               hwnd_        = nullptr;
    HWND               statusBar_   = nullptr;
    HWND               leftEdit_    = nullptr;
    HWND               leftBrowse_  = nullptr;
    HWND               rightEdit_   = nullptr;
    HWND               rightBrowse_ = nullptr;
    HWND               recursive_   = nullptr;
    HWND               compareBtn_  = nullptr;
    HWND               leftLabel_   = nullptr;
    HWND               rightLabel_  = nullptr;
    FolderCompareView  list_;

    std::wstring       leftRoot_;
    std::wstring       rightRoot_;
    bool               scanning_ = false;
    // Shared with the detached scan thread: flipped to false on destroy so a
    // late worker frees its result instead of posting to a dead window.
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);

    static FolderCompareWindow* s_active;
};

} // namespace npp
