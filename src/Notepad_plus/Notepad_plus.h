#pragma once
#include "../ScintillaComponent/ScintillaEditView.h"
#include "../ScintillaComponent/Buffer.h"
#include "../ScintillaComponent/FindReplaceDlg.h"
#include "../ScintillaComponent/FindInFilesDlg.h"
#include "../WinControls/TabBar/DocTabView.h"
#include "../WinControls/Docking/DockingManager.h"
#include "../WinControls/Docking/FindResultsPanel.h"
#include "../WinControls/Docking/FunctionListPanel.h"
#include "../WinControls/Docking/DocMapPanel.h"
#include "../WinControls/Docking/FolderWorkspacePanel.h"
#include <windows.h>
#include <string>
#include <unordered_map>

namespace npp {

// Posted to the frame window by the Find-in-Files worker thread when a scan
// completes. lParam carries a heap-allocated payload; the WndProc routes it
// to Notepad_plus::DeliverFindInFiles, which takes ownership.
inline constexpr UINT kMsgFindInFilesDone = WM_APP + 0x46;

// M2 "application core": one Scintilla view reused across multiple Buffers,
// driven by a DocTabView.
class Notepad_plus
{
public:
    // One-time setup: create editor + tab bar as children of `parent`.
    bool Attach(HWND parent, HINSTANCE hInst);

    // Layout: tab bar on top, status bar on bottom (passed in by caller).
    void Layout(const RECT& client, int statusBarHeight);

    // Status bar + title updaters (caller passes frame + statusbar).
    void UpdateStatusBar(HWND statusBar);
    void UpdateTitle(HWND frame);

    // Activation / Buffer ops
    void ActivateBuffer(BufferID id);      // switches Scintilla doc pointer + tab
    BufferID ActiveBuffer() const          { return views_[activeView_].activeId; }

    // Commands (return true on success; don't change UI themselves —
    // Notepad_plus_Window refreshes UI after each command).
    BufferID DoNew();                                  // always creates a fresh tab
    BufferID DoOpen(const std::wstring& path);         // new tab or activate existing
    bool     DoOpenDialog(HWND parent);
    bool     DoSave(HWND parent, BufferID id);
    bool     DoSaveAs(HWND parent, BufferID id);
    bool     DoSaveAll(HWND parent);
    bool     DoClose(HWND parent, BufferID id);        // prompt if dirty
    bool     DoCloseAll(HWND parent);
    bool     DoCloseAllBut(HWND parent, BufferID keep);
    bool     DoCloseToLeft(HWND parent, BufferID pivot);
    bool     DoCloseToRight(HWND parent, BufferID pivot);
    bool     CanQuit(HWND parent);
    void     Shutdown();   // release all docs/editors before window destruction

    // Edit command forwarders.
    void OnEdit(unsigned int cmd);

    // Language / encoding / EOL / bookmark / search commands.
    void SetLanguage(Buffer::Encoding /*unused*/) {}
    void ChangeLanguage(LangType lang);
    void ChangeEncoding(Buffer::Encoding enc);   // reinterpret (no bytes change)
    void ConvertEncoding(Buffer::Encoding enc);  // currently same as Change + mark dirty
    void ChangeEol(Buffer::Eol eol);             // converts existing CRLF/LF/CR

    void ToggleBookmark();
    void NextBookmark();
    void PrevBookmark();
    void ClearAllBookmarks();

    FindReplaceDlg&   FindDlg() { return findDlg_; }
    void ShowFind(HWND owner, HINSTANCE hInst);
    void ShowReplace(HWND owner, HINSTANCE hInst);
    void FindNextRepeat();
    void FindPrevRepeat();

    // Apply stored (or auto-detected) language + bookmark margin on activation.
    void RefreshEditorForActiveBuffer();

    // --- Dual-view helpers (M5) -----------------------------------------
    struct ViewSlot {
        ScintillaEditView editor;
        DocTabView        tabs;
        BufferID          activeId = kInvalidBufferID;
    };
    ViewSlot&       V()          { return views_[activeView_]; }
    const ViewSlot& V() const    { return views_[activeView_]; }
    ViewSlot&       V(int i)     { return views_[i]; }
    const ViewSlot& V(int i)const{ return views_[i]; }
    int             ActiveView() const { return activeView_; }
    void            SetActiveView(int v);

    // Public getters proxy to the active view so existing call sites stay valid.
    ScintillaEditView& Editor() { return V().editor; }
    DocTabView&        Tabs()   { return V().tabs; }
    DockingManager&    Dock()   { return dock_; }
    FindResultsPanel&  FindResults()      { return findResults_; }
    FunctionListPanel& FunctionListPane() { return funcList_; }
    DocMapPanel&       DocMapPane()       { return docMap_; }
    FolderWorkspacePanel& FolderPane()    { return folder_; }

    // Toggle a docking panel; refreshes its content if newly shown.
    void ToggleDock(DockSide side);

    // Route WM_NOTIFY from the frame window to the right sub-panel.
    LRESULT RouteNotify(LPARAM lParam);

    // Start a Find-in-Files search using `p` on a worker thread; results
    // arrive at the frame as kMsgFindInFilesDone and are applied by
    // DeliverFindInFiles (which discards stale generations).
    void RunFindInFiles(const FindInFilesParams& p);
    void DeliverFindInFiles(void* payload);

    // Refresh helpers (called on activation / after edits).
    void RefreshDocMapViewport();
    void RefreshFunctionList();

private:
    bool PromptDirtySave(HWND parent, BufferID id);
    void OpenBufferInTab(BufferID id, bool activate);
    void SyncTabLabel(BufferID id);
    void StashViewState(BufferID id);
    void RestoreViewState(BufferID id);

    // Initialize one view (editor + tabs + callbacks). Called for view 0
    // during Attach() and for view 1 when the user first toggles split.
    void InitViewSlot(int idx, HWND parent, HINSTANCE hInst);

    ViewSlot          views_[2];
    int               activeView_   = 0;
    static constexpr bool splitEnabled_ = false;  // dual-view feature removed; kept for branch gating

    FindReplaceDlg    findDlg_;
    DockingManager    dock_;
    FindResultsPanel  findResults_;
    FunctionListPanel funcList_;
    DocMapPanel       docMap_;
    FolderWorkspacePanel folder_;
    FindInFilesParams lastFif_{};
    unsigned          fifGen_ = 0;   // current Find-in-Files generation
    std::unordered_map<BufferID, std::string> binarySnapshot_;
    bool              binaryMutating_ = false;

public:
    // Binary (hex-dump) view toggle for the active buffer.
    // ON: snapshots current bytes, replaces editor with `xxd`-style hex
    // dump, sets the editor read-only.  OFF: restores original content.
    bool ToggleBinaryMode();
    bool IsInBinaryMode(BufferID id) const;
    // Re-render the text gutter on `line` from its current hex values.
    // Called from SCN_MODIFIED when the user edits within binary mode.
    void SyncBinaryGutter(ScintillaEditView& ed, sptr_t line);

    // Column editor: insert text or numeric series at the same column on each
    // line of the current selection (works with linear or rectangular sel).
    struct ColumnEditParams {
        bool        insertNumber = false;
        std::wstring text;          // insertNumber=false
        long long   initial   = 0;
        long long   increment = 1;
        int         padWidth  = 0;  // 0 = no padding
        int         base      = 10; // 10/16/8/2
        bool        leadingZeros = false;
    };
    void ColumnEdit(const ColumnEditParams& p);

    // Pretty-print the active buffer as JSON (4-space indent). Reports
    // a status message via UpdateStatusBar caller. Returns true on success.
    bool FormatActiveAsJson();

    // Lightweight check: does the active buffer's path end in ".json"?
    bool ActiveBufferIsJson() const;
};

} // namespace npp
