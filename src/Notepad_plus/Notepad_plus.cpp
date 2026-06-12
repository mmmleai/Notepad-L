#include "Notepad_plus.h"
#include "../MISC/Common/FileIO.h"
#include "../MISC/Common/StringUtil.h"
#include "../Parameters/Parameters.h"
#include "../Parameters/Stylers.h"
#include "../ScintillaComponent/BufferManager.h"
#include "../resource.h"

#include <Scintilla.h>
#include <ScintillaMessages.h>
#include <algorithm>

#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <memory>
#include <string>
#include <vector>
#include <cctype>

namespace npp {

namespace {
    constexpr int kTabBarId = 5001;
    constexpr int kTabBarHeight = 30;

    // Backing storage for owner-drawn status bar parts. SB_SETTEXTW with
    // SBT_OWNERDRAW passes lParam straight through to WM_DRAWITEM as itemData,
    // so we point it at these stable strings rather than at a stack temporary.
    std::wstring g_sbParts[5];

    void SetStatusPart(HWND sb, int idx, const wchar_t* text) {
        g_sbParts[idx] = text ? text : L"";
        ::SendMessageW(sb, SB_SETTEXTW,
            static_cast<WPARAM>(idx) | SBT_OWNERDRAW,
            reinterpret_cast<LPARAM>(g_sbParts[idx].c_str()));
    }
}

void Notepad_plus::InitViewSlot(int idx, HWND parent, HINSTANCE hInst)
{
    int tabId = kTabBarId + idx;   // distinct WM_DRAWITEM routing per view
    views_[idx].tabs.Create(parent, hInst, tabId);
    views_[idx].editor.Create(parent, hInst);

    views_[idx].tabs.SetOnActivate([this, idx](BufferID id) {
        activeView_ = idx;
        ActivateBuffer(id);
    });
    views_[idx].tabs.SetOnClose([this, idx](BufferID id) {
        activeView_ = idx;
        HWND owner = ::GetParent(views_[idx].editor.Hwnd());
        if (DoClose(owner, id)) {
            // ✕ / middle-click close arrives via the tab bar's subclass proc,
            // not the frame's WM_NOTIFY — refresh the title bar explicitly.
            ::SendMessageW(owner, kMsgUiRefresh, 0, 0);
        }
    });
    views_[idx].tabs.SetOnReorder([](BufferID, int) {});
    views_[idx].tabs.SetOnDropOut([this, idx](BufferID id, POINT screen) {
        // Drop on the OTHER view's tab bar → move tab there.
        // Drop anywhere else when split is on → also move.
        int other = 1 - idx;
        if (!splitEnabled_ || !views_[other].tabs.Hwnd()) return;
        // If the drop point falls inside the other view's tab bar OR its editor,
        // treat it as a move to that view.
        RECT otherTabRc{}, otherEdRc{};
        ::GetWindowRect(views_[other].tabs.Hwnd(),   &otherTabRc);
        ::GetWindowRect(views_[other].editor.Hwnd(), &otherEdRc);
        bool overOther = ::PtInRect(&otherTabRc, screen) ||
                         ::PtInRect(&otherEdRc,  screen);
        if (!overOther) return;
        if (views_[idx].tabs.TabCount() <= 1) return;  // keep at least one
        Buffer* b = BufferManager::Instance().Get(id);
        if (!b) return;
        views_[idx].tabs.RemoveTab(id);
        views_[idx].activeId = views_[idx].tabs.ActiveBuffer();
        if (views_[idx].activeId != kInvalidBufferID) {
            if (Buffer* nb = BufferManager::Instance().Get(views_[idx].activeId))
                views_[idx].editor.AttachDocument(nb->DocHandle());
        }
        if (views_[other].tabs.IndexOf(id) < 0) {
            views_[other].tabs.AddTab(id, b->DisplayName(), b->IsDirty(), true);
        } else {
            views_[other].tabs.Activate(id);
        }
        activeView_ = other;
        ActivateBuffer(id);
        ::SendMessageW(::GetParent(views_[other].editor.Hwnd()),
            kMsgUiRefresh, 0, 0);
    });
    views_[idx].tabs.SetOnContext([this, idx](BufferID id, POINT screenPt) {
        activeView_ = idx;
        // Host (Notepad_plus_Window) re-installs a rich context menu callback
        // after Attach; this default is a no-op so activation still works.
        (void)id; (void)screenPt;
    });
}

bool Notepad_plus::Attach(HWND parent, HINSTANCE hInst)
{
    InitViewSlot(0, parent, hInst);
    BufferManager::Instance().SetFactoryView(&views_[0].editor);

    // Docking manager + panels.
    dock_.Create(parent, hInst);
    folder_.Create(parent, hInst);
    funcList_.Create(parent, hInst);
    docMap_.Create(parent, hInst);
    dock_.Register(DockSide::Left,   &folder_);
    dock_.Register(DockSide::Right,  &funcList_);
    dock_.SetOnClose([this](DockSide side) {
        dock_.Show(side, false);
        HWND frame = ::GetParent(V().editor.Hwnd());
        ::SendMessageW(frame, WM_SIZE, 0, 0);
    });

    auto centerOnLine = [this](int line0) {
        V().editor.Call(SCI_ENSUREVISIBLEENFORCEPOLICY, static_cast<uptr_t>(line0));
        V().editor.Call(SCI_GOTOLINE, static_cast<uptr_t>(line0));
        int lines = static_cast<int>(V().editor.Call(SCI_LINESONSCREEN));
        int first = line0 - lines / 2;
        if (first < 0) first = 0;
        V().editor.Call(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(first));
    };
    funcList_.SetOnGoto([this, centerOnLine](int line) {
        centerOnLine(line - 1);
        V().editor.SetFocus();
    });
    docMap_.SetOnScroll([this](int first) {
        V().editor.Call(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(first));
    });
    folder_.SetOnOpen([this](const std::wstring& path) {
        DoOpen(path);
    });
    return true;
}

void Notepad_plus::Layout(const RECT& client, int statusBarHeight)
{
    RECT avail = client;
    avail.bottom -= statusBarHeight;

    RECT center{};
    dock_.Layout(avail, center);

    RECT tabRc = center;
    tabRc.bottom = tabRc.top + kTabBarHeight;
    views_[0].tabs.Resize(tabRc);

    RECT editorRc = center;
    editorRc.top = tabRc.bottom;
    views_[0].editor.Resize(editorRc);
}

void Notepad_plus::UpdateTitle(HWND frame)
{
    Buffer* b = BufferManager::Instance().Get(V().activeId);
    std::wstring title = L"NotePad-L - ";
    if (!b) {
        title += L"(no document)";
    } else {
        title += b->IsUntitled() ? b->DisplayName()
                                 : b->Path();
        if (b->IsDirty()) title += L" *";
    }
    ::SetWindowTextW(frame, title.c_str());
}

void Notepad_plus::UpdateStatusBar(HWND sb)
{
    if (!sb) return;
    Buffer* b = BufferManager::Instance().Get(V().activeId);
    const bool dirty = b && b->IsDirty();

    const auto pos  = V().editor.Call(SCI_GETCURRENTPOS);
    const auto line = V().editor.Call(SCI_LINEFROMPOSITION, static_cast<uptr_t>(pos)) + 1;
    const auto col  = V().editor.Call(SCI_GETCOLUMN, static_cast<uptr_t>(pos)) + 1;
    const auto len  = V().editor.Call(SCI_GETLENGTH);
    const auto lines= V().editor.Call(SCI_GETLINECOUNT);

    wchar_t buf[128];
    ::swprintf_s(buf, L"Length: %lld    Lines: %lld",
        static_cast<long long>(len), static_cast<long long>(lines));
    SetStatusPart(sb, 0, buf);

    ::swprintf_s(buf, L"Ln: %lld    Col: %lld",
        static_cast<long long>(line), static_cast<long long>(col));
    SetStatusPart(sb, 1, buf);

    const wchar_t* encLabel = L"UTF-8";
    const wchar_t* eolLabel = L"CRLF";
    if (b) {
        switch (b->GetEncoding()) {
        case Buffer::Encoding::Utf8:        encLabel = L"UTF-8";        break;
        case Buffer::Encoding::Utf8Bom:     encLabel = L"UTF-8 BOM";    break;
        case Buffer::Encoding::Utf16LeBom:  encLabel = L"UTF-16 LE";    break;
        case Buffer::Encoding::Utf16BeBom:  encLabel = L"UTF-16 BE";    break;
        case Buffer::Encoding::Ansi:        encLabel = L"ANSI";         break;
        }
        switch (b->GetEol()) {
        case Buffer::Eol::Crlf: eolLabel = L"CRLF"; break;
        case Buffer::Eol::Lf:   eolLabel = L"LF";   break;
        case Buffer::Eol::Cr:   eolLabel = L"CR";   break;
        }
    }
    std::wstring encEol = std::wstring(encLabel) + L" - "
                        + LangTypeName(b ? b->GetLang() : LangType::Text);
    SetStatusPart(sb, 2, encEol.c_str());
    SetStatusPart(sb, 3, eolLabel);
    SetStatusPart(sb, 4, dirty ? L"Modified" : L"Saved");
}

void Notepad_plus::StashViewState(BufferID id)
{
    Buffer* b = BufferManager::Instance().Get(id);
    if (!b) return;
    auto& v = b->View();
    v.firstVisibleLine = V().editor.Call(SCI_GETFIRSTVISIBLELINE);
    v.caretPos         = V().editor.Call(SCI_GETCURRENTPOS);
    v.anchorPos        = V().editor.Call(SCI_GETANCHOR);
    v.xOffset          = V().editor.Call(SCI_GETXOFFSET);
}

void Notepad_plus::RestoreViewState(BufferID id)
{
    Buffer* b = BufferManager::Instance().Get(id);
    if (!b) return;
    const auto& v = b->View();
    // Selection first — SCI_SETSEL scrolls the caret into view, so setting it
    // after the scroll position would clobber our restored topLine/xOffset.
    V().editor.Call(SCI_SETSEL,              static_cast<uptr_t>(v.anchorPos), v.caretPos);
    V().editor.Call(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(v.firstVisibleLine));
    V().editor.Call(SCI_SETXOFFSET,          static_cast<uptr_t>(v.xOffset));
}

void Notepad_plus::ActivateBuffer(BufferID id)
{
    if (id == kInvalidBufferID || id == V().activeId) return;
    if (V().activeId != kInvalidBufferID) StashViewState(V().activeId);

    Buffer* b = BufferManager::Instance().Get(id);
    if (!b) return;

    V().editor.AttachDocument(b->DocHandle());
    V().activeId = id;
    RefreshEditorForActiveBuffer();
    // Buffer may have pending post-edit restyle work if the debounce timer
    // on the frame didn't fire before the user switched away. Same redraw
    // batching as FlushPendingStyle so the styling calls coalesce into one
    // repaint.
    if (b->StyleDirty()) {
        HWND edHwnd = V().editor.Hwnd();
        if (edHwnd) ::SendMessageW(edHwnd, WM_SETREDRAW, FALSE, 0);
        if (b->GetLang() == LangType::Markdown) StyleMarkdownFences(V().editor);
        else                                    HighlightFunctionNames(V().editor, b->GetLang());
        if (edHwnd) {
            ::SendMessageW(edHwnd, WM_SETREDRAW, TRUE, 0);
            ::InvalidateRect(edHwnd, nullptr, FALSE);
        }
        b->SetStyleDirty(false);
    }
    RestoreViewState(id);

    // Rebind docmap / function list to the newly-active document.
    if (dock_.IsShown(DockSide::Right) && dock_.Panel(DockSide::Right) == &docMap_) {
        docMap_.AttachDoc(b->DocHandle());
        RefreshDocMapViewport();
    }
    if (dock_.IsShown(DockSide::Right) && dock_.Panel(DockSide::Right) == &funcList_) {
        RefreshFunctionList();
    }

    V().editor.SetFocus();
}

void Notepad_plus::ToggleDock(DockSide side)
{
    bool wasShown = dock_.IsShown(side);
    dock_.Show(side, !wasShown);
    HWND frame = ::GetParent(V().editor.Hwnd());
    ::SendMessageW(frame, WM_SIZE, 0, 0);
    if (!wasShown) {
        if (side == DockSide::Right) {
            DockPanel* p = dock_.Panel(DockSide::Right);
            if (p == &funcList_) RefreshFunctionList();
            else if (p == &docMap_) {
                Buffer* b = BufferManager::Instance().Get(V().activeId);
                if (b) { docMap_.AttachDoc(b->DocHandle()); RefreshDocMapViewport(); }
            }
        }
    }
}

void Notepad_plus::RefreshFunctionList()
{
    Buffer* b = BufferManager::Instance().Get(V().activeId);
    if (!b) return;
    funcList_.Rebuild(V().editor, b->GetLang());
}

void Notepad_plus::RefreshDocMapViewport()
{
    int first = static_cast<int>(V().editor.Call(SCI_GETFIRSTVISIBLELINE));
    int lines = static_cast<int>(V().editor.Call(SCI_LINESONSCREEN));
    docMap_.UpdateViewport(first, lines);
}

LRESULT Notepad_plus::RouteNotify(LPARAM lParam)
{
    auto* nm = reinterpret_cast<LPNMHDR>(lParam);
    if (!nm) return 0;
    if (nm->hwndFrom == funcList_.Hwnd() ||
        ::GetParent(nm->hwndFrom) == funcList_.Hwnd())
        return funcList_.HandleNotify(lParam);
    if (nm->hwndFrom == folder_.Hwnd() ||
        ::GetParent(nm->hwndFrom) == folder_.Hwnd())
        return folder_.HandleNotify(lParam);
    return 0;
}

void Notepad_plus::RefreshEditorForActiveBuffer()
{
    Buffer* b = BufferManager::Instance().Get(V().activeId);
    if (!b) return;
    ApplyLanguage(V().editor, b->GetLang());
    switch (b->GetEol()) {
    case Buffer::Eol::Crlf: V().editor.Call(SCI_SETEOLMODE, SC_EOL_CRLF); break;
    case Buffer::Eol::Lf:   V().editor.Call(SCI_SETEOLMODE, SC_EOL_LF);   break;
    case Buffer::Eol::Cr:   V().editor.Call(SCI_SETEOLMODE, SC_EOL_CR);   break;
    }
    // Overtype is view state, not document state: re-assert it per buffer so
    // hex-column overwrite never leaks from a binary-mode tab into a text tab.
    V().editor.Call(SCI_SETOVERTYPE, IsInBinaryMode(V().activeId) ? 1 : 0);
}

void Notepad_plus::SyncTabLabel(BufferID id)
{
    Buffer* b = BufferManager::Instance().Get(id);
    if (!b) return;
    V().tabs.SetLabel(id, b->DisplayName(), b->IsDirty());
}

BufferID Notepad_plus::DoNew()
{
    BufferID id = BufferManager::Instance().NewUntitled();
    if (id == kInvalidBufferID) return id;
    Buffer* b = BufferManager::Instance().Get(id);
    V().tabs.AddTab(id, b->DisplayName(), false, /*activate*/true);
    return id;
}

void Notepad_plus::OpenBufferInTab(BufferID id, bool activate)
{
    Buffer* b = BufferManager::Instance().Get(id);
    if (!b) return;
    if (V().tabs.IndexOf(id) < 0) {
        V().tabs.AddTab(id, b->DisplayName(), b->IsDirty(), activate);
    } else if (activate) {
        V().tabs.Activate(id);
    }
}

BufferID Notepad_plus::DoOpen(const std::wstring& path)
{
    std::wstring err;
    BufferID id = BufferManager::Instance().OpenFile(path, &err);
    if (id == kInvalidBufferID) {
        ::MessageBoxW(::GetParent(V().editor.Hwnd()), err.c_str(), L"NotePad-L",
            MB_OK | MB_ICONERROR);
        return id;
    }
    if (Buffer* b = BufferManager::Instance().Get(id)) {
        if (b->GetLang() == LangType::Text) b->SetLang(LangFromPath(path));
    }
    OpenBufferInTab(id, /*activate*/true);
    Parameters::Instance().AddRecent(path);
    Parameters::Instance().Save();
    return id;
}

bool Notepad_plus::DoOpenDialog(HWND parent)
{
    wchar_t buf[32768] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = parent;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = ARRAYSIZE(buf);
    ofn.lpstrFilter =
        L"All files (*.*)\0*.*\0"
        L"Text files (*.txt)\0*.txt\0";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY |
                OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;

    if (!::GetOpenFileNameW(&ofn)) return false;

    // Parse multi-select: if first string is a directory, following strings are files.
    DWORD attr = ::GetFileAttributesW(buf);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring dir = buf;
        wchar_t* p = buf + dir.size() + 1;
        bool any = false;
        while (*p) {
            std::wstring full = dir + L"\\" + p;
            if (DoOpen(full) != kInvalidBufferID) any = true;
            p += wcslen(p) + 1;
        }
        return any;
    }
    return DoOpen(buf) != kInvalidBufferID;
}

namespace {
    std::string ParseHexDump(const std::string& dump);  // defined below.
}

// Binary-mode buffers are saved from their hex dump: parse the dump out of
// the buffer's own document (works for background tabs too — Save All used
// to serialize the dump text itself into the file) and write the decoded
// bytes. The document is never modified, so the user's undo history
// survives every save and a failed save leaves the dirty flag intact.
bool Notepad_plus::SaveBinaryDump(BufferID id, const std::wstring* newPath,
                                  std::wstring* err)
{
    auto& bm = BufferManager::Instance();
    std::string raw = ParseHexDump(bm.GetDocText(id));
    const bool ok = newPath ? bm.SaveBufferBytesAs(id, *newPath, raw, err)
                            : bm.SaveBufferBytes(id, raw, err);
    if (!ok) return false;
    auto it = binarySnapshot_.find(id);
    if (it != binarySnapshot_.end()) {
        it->second.bytes    = std::move(raw);
        it->second.wasDirty = false;
    }
    return true;
}

bool Notepad_plus::DoSave(HWND parent, BufferID id)
{
    Buffer* b = BufferManager::Instance().Get(id);
    if (!b) return false;
    if (b->IsUntitled()) return DoSaveAs(parent, id);
    std::wstring err;
    const bool ok = IsInBinaryMode(id)
        ? SaveBinaryDump(id, nullptr, &err)
        : BufferManager::Instance().SaveBuffer(id, &err);
    if (!ok) {
        ::MessageBoxW(parent, err.c_str(), L"NotePad-L", MB_OK | MB_ICONERROR);
        return false;
    }
    SyncTabLabel(id);
    Parameters::Instance().AddRecent(b->Path());
    Parameters::Instance().Save();
    return true;
}

bool Notepad_plus::DoSaveAs(HWND parent, BufferID id)
{
    Buffer* b = BufferManager::Instance().Get(id);
    if (!b) return false;

    wchar_t buf[MAX_PATH * 8] = L"";
    if (!b->IsUntitled()) ::wcscpy_s(buf, b->Path().c_str());

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = parent;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = ARRAYSIZE(buf);
    ofn.lpstrFilter =
        L"All files (*.*)\0*.*\0"
        L"Text files (*.txt)\0*.txt\0";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY |
                OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"txt";

    if (!::GetSaveFileNameW(&ofn)) return false;

    std::wstring err;
    const std::wstring newPath = buf;
    const bool ok = IsInBinaryMode(id)
        ? SaveBinaryDump(id, &newPath, &err)
        : BufferManager::Instance().SaveBufferAs(id, newPath, &err);
    if (!ok) {
        ::MessageBoxW(parent, err.c_str(), L"NotePad-L", MB_OK | MB_ICONERROR);
        return false;
    }
    SyncTabLabel(id);
    Parameters::Instance().AddRecent(buf);
    Parameters::Instance().Save();
    return true;
}

bool Notepad_plus::DoSaveAll(HWND parent)
{
    bool ok = true;
    for (BufferID id : BufferManager::Instance().AllIds()) {
        Buffer* b = BufferManager::Instance().Get(id);
        if (b && b->IsDirty()) {
            if (!DoSave(parent, id)) ok = false;
        }
    }
    return ok;
}

bool Notepad_plus::PromptDirtySave(HWND parent, BufferID id)
{
    Buffer* b = BufferManager::Instance().Get(id);
    if (!b || !b->IsDirty()) return true;

    // Bring the buffer on screen before the prompt — go through the tab bar
    // so the tab selection, editor and title stay in sync even if the user
    // cancels the close afterwards.
    if (V().activeId != id) {
        V().tabs.Activate(id);
        if (V().activeId != id) ActivateBuffer(id);  // safety net
    }

    std::wstring msg = L"Save changes to ";
    msg += b->DisplayName();
    msg += L"?";
    int r = ::MessageBoxW(parent, msg.c_str(), L"NotePad-L",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES)    return DoSave(parent, id);
    return true;
}

bool Notepad_plus::DoClose(HWND parent, BufferID id)
{
    if (!PromptDirtySave(parent, id)) return false;
    // Removing the selected tab activates its neighbour via the tab bar's
    // callback (full ActivateBuffer incl. styling + view-state restore);
    // removing a background tab leaves the current selection untouched.
    V().tabs.RemoveTab(id);
    if (V().activeId == id) V().activeId = kInvalidBufferID;
    binarySnapshot_.erase(id);
    BufferManager::Instance().CloseBuffer(id);

    // If that was the last tab in this view, make sure it isn't empty.
    if (V().tabs.TabCount() == 0) {
        DoNew();
    } else if (V().activeId != V().tabs.ActiveBuffer()) {
        // Safety net for paths where the tab callback didn't run.
        ActivateBuffer(V().tabs.ActiveBuffer());
    }
    return true;
}

bool Notepad_plus::DoCloseAll(HWND parent)
{
    // Iterate a snapshot since tab list mutates.
    while (V().tabs.TabCount() > 0) {
        BufferID id = V().tabs.BufferAt(0);
        if (!DoClose(parent, id)) return false;
        if (V().tabs.TabCount() == 1 &&
            BufferManager::Instance().Get(V().tabs.BufferAt(0)) &&
            BufferManager::Instance().Get(V().tabs.BufferAt(0))->IsUntitled() &&
            !BufferManager::Instance().Get(V().tabs.BufferAt(0))->IsDirty()) {
            break;
        }
    }
    return true;
}

bool Notepad_plus::DoCloseAllBut(HWND parent, BufferID keep)
{
    std::vector<BufferID> victims;
    for (int i = 0, n = V().tabs.TabCount(); i < n; ++i) {
        BufferID id = V().tabs.BufferAt(i);
        if (id != keep) victims.push_back(id);
    }
    for (BufferID id : victims) {
        if (!DoClose(parent, id)) return false;
    }
    return true;
}

bool Notepad_plus::DoCloseToLeft(HWND parent, BufferID pivot)
{
    const int pivotIdx = V().tabs.IndexOf(pivot);
    std::vector<BufferID> victims;
    for (int i = 0; i < pivotIdx; ++i) victims.push_back(V().tabs.BufferAt(i));
    for (BufferID id : victims) {
        if (!DoClose(parent, id)) return false;
    }
    return true;
}

bool Notepad_plus::DoCloseToRight(HWND parent, BufferID pivot)
{
    const int pivotIdx = V().tabs.IndexOf(pivot);
    std::vector<BufferID> victims;
    for (int i = pivotIdx + 1, n = V().tabs.TabCount(); i < n; ++i)
        victims.push_back(V().tabs.BufferAt(i));
    for (BufferID id : victims) {
        if (!DoClose(parent, id)) return false;
    }
    return true;
}

bool Notepad_plus::CanQuit(HWND parent)
{
    // Collect dirty buffers, prompt once per buffer. Cancel aborts quit.
    for (BufferID id : BufferManager::Instance().AllIds()) {
        Buffer* b = BufferManager::Instance().Get(id);
        if (b && b->IsDirty()) {
            if (!PromptDirtySave(parent, id)) return false;
        }
    }
    return true;
}

void Notepad_plus::Shutdown()
{
    // Detach Scintilla documents from all editor views BEFORE the windows
    // are destroyed, so Scintilla/Lexilla won't access freed lexer objects
    // during child-window teardown (crashes on Win7).
    for (int i = 0; i < 2; ++i) {
        if (views_[i].editor.Hwnd())
            views_[i].editor.Call(SCI_SETDOCPOINTER, 0, 0);
    }

    // Release all document handles held by the BufferManager.
    auto& bm = BufferManager::Instance();
    for (BufferID id : bm.AllIds())
        bm.CloseBuffer(id);
    bm.SetFactoryView(nullptr);
    binarySnapshot_.clear();

    // Explicitly destroy Scintilla editor windows so their destructors
    // won't try to DestroyWindow on already-dead handles later.
    for (int i = 0; i < 2; ++i)
        views_[i].editor.Destroy();
}

void Notepad_plus::OnEdit(unsigned int cmd)
{
    // The hex dump is a fixed-width table: structural edits (paste, line
    // moves, sort, case, …) arriving via menu or accelerator would shred it
    // and the shredded text would be decoded back into garbage bytes on
    // save. Only read-only commands and undo/redo are allowed in hex mode.
    if (IsInBinaryMode(ActiveBuffer())) {
        switch (cmd) {
        case IDM_EDIT_UNDO: case IDM_EDIT_REDO:
        case IDM_EDIT_COPY: case IDM_EDIT_SELECTALL:
            break;
        default:
            ::MessageBeep(MB_ICONASTERISK);
            return;
        }
    }

    switch (cmd) {
    case IDM_EDIT_UNDO:      V().editor.Call(SCI_UNDO);      break;
    case IDM_EDIT_REDO:      V().editor.Call(SCI_REDO);      break;
    case IDM_EDIT_CUT:       V().editor.Call(SCI_CUT);       break;
    case IDM_EDIT_COPY:      V().editor.Call(SCI_COPY);      break;
    case IDM_EDIT_PASTE:     V().editor.Call(SCI_PASTE);     break;
    case IDM_EDIT_DELETE:    V().editor.Call(SCI_CLEAR);     break;
    case IDM_EDIT_SELECTALL: V().editor.Call(SCI_SELECTALL); break;

    case IDM_EDIT_DUP_LINE:    V().editor.Call(SCI_LINEDUPLICATE); break;
    case IDM_EDIT_DEL_LINE:    V().editor.Call(SCI_LINEDELETE);    break;
    case IDM_EDIT_MOVE_UP:     V().editor.Call(SCI_MOVESELECTEDLINESUP);   break;
    case IDM_EDIT_MOVE_DOWN:   V().editor.Call(SCI_MOVESELECTEDLINESDOWN); break;
    case IDM_EDIT_SPLIT_LINES: V().editor.Call(SCI_LINESSPLIT, 0); break;
    case IDM_EDIT_JOIN_LINES:  V().editor.Call(SCI_LINESJOIN);     break;
    case IDM_EDIT_UPPERCASE:   V().editor.Call(SCI_UPPERCASE);     break;
    case IDM_EDIT_LOWERCASE:   V().editor.Call(SCI_LOWERCASE);     break;
    case IDM_EDIT_INDENT:      V().editor.Call(SCI_TAB);           break;
    case IDM_EDIT_OUTDENT:     V().editor.Call(SCI_BACKTAB);       break;

    case IDM_EDIT_SORT_ASC:
    case IDM_EDIT_SORT_DESC: {
        sptr_t selS = V().editor.Call(SCI_GETSELECTIONSTART);
        sptr_t selE = V().editor.Call(SCI_GETSELECTIONEND);
        sptr_t lineS = (selS == selE)
            ? 0
            : V().editor.Call(SCI_LINEFROMPOSITION, static_cast<uptr_t>(selS));
        sptr_t lineE = (selS == selE)
            ? V().editor.Call(SCI_GETLINECOUNT) - 1
            : V().editor.Call(SCI_LINEFROMPOSITION, static_cast<uptr_t>(selE));
        if (selS != selE && selE == V().editor.Call(SCI_POSITIONFROMLINE,
                                  static_cast<uptr_t>(lineE))) --lineE;

        std::vector<std::string> lines;
        for (sptr_t i = lineS; i <= lineE; ++i) {
            sptr_t a = V().editor.Call(SCI_POSITIONFROMLINE, static_cast<uptr_t>(i));
            sptr_t b = V().editor.Call(SCI_GETLINEENDPOSITION, static_cast<uptr_t>(i));
            std::string line(static_cast<size_t>(b - a), '\0');
            Sci_TextRangeFull tr{{a, b}, line.data()};
            V().editor.Call(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>(&tr));
            lines.push_back(std::move(line));
        }
        std::sort(lines.begin(), lines.end());
        if (cmd == IDM_EDIT_SORT_DESC)
            std::reverse(lines.begin(), lines.end());

        sptr_t rangeStart = V().editor.Call(SCI_POSITIONFROMLINE,
            static_cast<uptr_t>(lineS));
        sptr_t rangeEnd   = V().editor.Call(SCI_GETLINEENDPOSITION,
            static_cast<uptr_t>(lineE));

        // Join with the document's EOL mode — a bare '\n' would silently
        // convert the sorted range of a CRLF document to LF.
        const int eolMode = static_cast<int>(V().editor.Call(SCI_GETEOLMODE));
        const char* eol = (eolMode == SC_EOL_CR)   ? "\r"
                        : (eolMode == SC_EOL_LF)   ? "\n"
                                                   : "\r\n";
        std::string joined;
        for (size_t i = 0; i < lines.size(); ++i) {
            joined += lines[i];
            if (i + 1 < lines.size()) joined += eol;
        }
        V().editor.Call(SCI_BEGINUNDOACTION);
        V().editor.Call(SCI_SETTARGETRANGE,
            static_cast<uptr_t>(rangeStart), rangeEnd);
        V().editor.Call(SCI_REPLACETARGET,
            static_cast<uptr_t>(joined.size()),
            reinterpret_cast<sptr_t>(joined.data()));
        V().editor.Call(SCI_ENDUNDOACTION);
        break;
    }

    case IDM_EDIT_TRIM_TRAIL: {
        V().editor.Call(SCI_SETSEARCHFLAGS,
            SCFIND_REGEXP | SCFIND_CXX11REGEX);
        V().editor.Call(SCI_BEGINUNDOACTION);
        const char pat[] = "[ \\t]+$";
        sptr_t start = 0, end = V().editor.Call(SCI_GETLENGTH);
        while (true) {
            V().editor.Call(SCI_SETTARGETRANGE,
                static_cast<uptr_t>(start), end);
            sptr_t pos = V().editor.Call(SCI_SEARCHINTARGET,
                static_cast<uptr_t>(sizeof(pat) - 1),
                reinterpret_cast<sptr_t>(pat));
            if (pos < 0) break;
            V().editor.Call(SCI_REPLACETARGET, 0,
                reinterpret_cast<sptr_t>(""));
            end = V().editor.Call(SCI_GETLENGTH);
            start = pos + 1;
        }
        V().editor.Call(SCI_ENDUNDOACTION);
        break;
    }

    default: break;
    }
}

void Notepad_plus::ChangeLanguage(LangType lang)
{
    Buffer* b = BufferManager::Instance().Get(V().activeId);
    if (!b) return;
    b->SetLang(lang);
    ApplyLanguage(V().editor, lang);
}

void Notepad_plus::ConvertEncoding(Buffer::Encoding enc)
{
    if (Buffer* b = BufferManager::Instance().Get(V().activeId)) {
        if (b->GetEncoding() == enc) return;
        b->SetEncoding(enc);
        // Sticky dirty bit: undoing back to the savepoint must not launder
        // the pending conversion away. Cleared by the next successful save.
        // (The previous implementation relocated the Scintilla savepoint
        // here, which let one Ctrl+Z mark an edited buffer "Saved".)
        b->MarkEncodingDirty();
        SyncTabLabel(V().activeId);
    }
}

void Notepad_plus::ChangeEol(Buffer::Eol eol)
{
    Buffer* b = BufferManager::Instance().Get(V().activeId);
    if (!b) return;
    b->SetEol(eol);
    int mode = SC_EOL_CRLF;
    switch (eol) {
    case Buffer::Eol::Crlf: mode = SC_EOL_CRLF; break;
    case Buffer::Eol::Lf:   mode = SC_EOL_LF;   break;
    case Buffer::Eol::Cr:   mode = SC_EOL_CR;   break;
    }
    V().editor.Call(SCI_SETEOLMODE, static_cast<uptr_t>(mode));
    V().editor.Call(SCI_CONVERTEOLS, static_cast<uptr_t>(mode));
}

void Notepad_plus::ToggleBookmark()
{
    sptr_t line = V().editor.Call(SCI_LINEFROMPOSITION,
        static_cast<uptr_t>(V().editor.Call(SCI_GETCURRENTPOS)));
    sptr_t state = V().editor.Call(SCI_MARKERGET, static_cast<uptr_t>(line));
    if (state & (1 << 24))
        V().editor.Call(SCI_MARKERDELETE, static_cast<uptr_t>(line), 24);
    else
        V().editor.Call(SCI_MARKERADD,    static_cast<uptr_t>(line), 24);
}

void Notepad_plus::NextBookmark()
{
    sptr_t line = V().editor.Call(SCI_LINEFROMPOSITION,
        static_cast<uptr_t>(V().editor.Call(SCI_GETCURRENTPOS)));
    sptr_t next = V().editor.Call(SCI_MARKERNEXT,
        static_cast<uptr_t>(line + 1), 1 << 24);
    if (next < 0) next = V().editor.Call(SCI_MARKERNEXT, 0, 1 << 24);
    if (next >= 0) {
        V().editor.Call(SCI_GOTOLINE, static_cast<uptr_t>(next));
    }
}

void Notepad_plus::PrevBookmark()
{
    sptr_t line = V().editor.Call(SCI_LINEFROMPOSITION,
        static_cast<uptr_t>(V().editor.Call(SCI_GETCURRENTPOS)));
    sptr_t prev = V().editor.Call(SCI_MARKERPREVIOUS,
        static_cast<uptr_t>(line - 1), 1 << 24);
    if (prev < 0) {
        prev = V().editor.Call(SCI_MARKERPREVIOUS,
            static_cast<uptr_t>(V().editor.Call(SCI_GETLINECOUNT) - 1),
            1 << 24);
    }
    if (prev >= 0) {
        V().editor.Call(SCI_GOTOLINE, static_cast<uptr_t>(prev));
    }
}

void Notepad_plus::ClearAllBookmarks()
{
    V().editor.Call(SCI_MARKERDELETEALL, 24);
}

void Notepad_plus::ShowFind(HWND owner, HINSTANCE hInst)
{
    findDlg_.Show(owner, hInst, &V().editor, FindMode::Find);
}
void Notepad_plus::ShowReplace(HWND owner, HINSTANCE hInst)
{
    findDlg_.Show(owner, hInst, &V().editor, FindMode::Replace);
}
void Notepad_plus::FindNextRepeat() { findDlg_.FindNextAgain(&V().editor); }
void Notepad_plus::FindPrevRepeat() { findDlg_.FindPrevAgain(&V().editor); }

void Notepad_plus::SetActiveView(int v)
{
    if (v != 0) return;
    activeView_ = 0;
}

bool Notepad_plus::IsInBinaryMode(BufferID id) const
{
    return binarySnapshot_.find(id) != binarySnapshot_.end();
}

bool Notepad_plus::BinaryBaselineDirty(BufferID id) const
{
    auto it = binarySnapshot_.find(id);
    return it != binarySnapshot_.end() && it->second.wasDirty;
}

namespace {
    constexpr char kHexDigits[] = "0123456789ABCDEF";

    // Append a UTF-8-aware text gutter for one dump row (≤16 bytes):
    // printable ASCII as-is, valid multi-byte UTF-8 sequences decoded so
    // CJK characters are visible, everything else as '.'.
    void AppendGutter(std::string& out, const char* bytes, size_t count)
    {
        for (size_t i = 0; i < 16 && i < count; ) {
            unsigned char c = static_cast<unsigned char>(bytes[i]);
            size_t seqLen = 0;
            if (c < 0x80) { seqLen = (c >= 0x20 && c < 0x7F) ? 1 : 0; }
            else if ((c & 0xE0) == 0xC0) seqLen = 2;
            else if ((c & 0xF0) == 0xE0) seqLen = 3;
            else if ((c & 0xF8) == 0xF0) seqLen = 4;
            bool valid = seqLen > 0 && i + seqLen <= 16 && i + seqLen <= count;
            for (size_t k = 1; valid && k < seqLen; ++k) {
                if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80)
                    valid = false;
            }
            if (valid) { out.append(bytes + i, seqLen); i += seqLen; }
            else       { out.push_back('.');            ++i; }
        }
    }

    // Build an `xxd`-style hex dump of `bytes`. 16 bytes per row,
    // 8-byte gap, ASCII gutter. Hand-rolled nibble formatting — an
    // snprintf per byte made toggling hex mode on big files take seconds.
    std::string MakeHexDump(const std::string& bytes)
    {
        std::string out;
        const size_t total = bytes.size();
        out.reserve((total / 16 + 1) * 84);
        char row[64];
        for (size_t off = 0; off < total; off += 16) {
            int n = 0;
            for (int k = 7; k >= 0; --k)
                row[n++] = kHexDigits[(off >> (k * 4)) & 0xF];
            row[n++] = ' '; row[n++] = ' ';
            // Hex columns.
            for (size_t i = 0; i < 16; ++i) {
                if (off + i < total) {
                    unsigned char c = static_cast<unsigned char>(bytes[off + i]);
                    row[n++] = kHexDigits[c >> 4];
                    row[n++] = kHexDigits[c & 0xF];
                    row[n++] = ' ';
                } else {
                    row[n++] = ' '; row[n++] = ' '; row[n++] = ' ';
                }
                if (i == 7) row[n++] = ' ';
            }
            out.append(row, n);
            out.append(" |");
            AppendGutter(out, bytes.data() + off,
                std::min<size_t>(16, total - off));
            out.append("|\r\n");
        }
        return out;
    }

    // Inverse of MakeHexDump: recover raw bytes from the editor text.
    // Parses each line as "<8 hex offset>  <16 hex pairs>  |gutter|".
    // Tolerates partial/incomplete last rows; stops a line at the first
    // non-hex column or once 16 bytes have been read.
    int HexNibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    std::string ParseHexDump(const std::string& dump) {
        std::string bytes;
        bytes.reserve(dump.size() / 4);
        size_t p = 0;
        while (p < dump.size()) {
            size_t nl = dump.find('\n', p);
            size_t lineEnd = (nl == std::string::npos) ? dump.size() : nl;
            size_t le = lineEnd;
            if (le > p && dump[le - 1] == '\r') --le;
            // Skip 8-char offset + 2 spaces.
            size_t q = p + 10;
            int count = 0;
            while (q + 1 < le && count < 16) {
                if (dump[q] == '|') break;
                if (dump[q] == ' ') { ++q; continue; }
                int hi = HexNibble(dump[q]);
                int lo = HexNibble(dump[q + 1]);
                if (hi < 0 || lo < 0) break;
                bytes.push_back(static_cast<char>((hi << 4) | lo));
                q += 2;
                ++count;
            }
            p = (nl == std::string::npos) ? dump.size() : nl + 1;
        }
        return bytes;
    }
}

namespace {
    // Append a UTF-8-aware gutter for up to 16 bytes to `out`.
    void AppendGutter(std::string& out, const std::string& bytes) {
        size_t total = bytes.size();
        for (size_t i = 0; i < 16 && i < total; ) {
            unsigned char c = static_cast<unsigned char>(bytes[i]);
            size_t seqLen = 0;
            if (c < 0x80) { seqLen = (c >= 0x20 && c < 0x7F) ? 1 : 0; }
            else if ((c & 0xE0) == 0xC0) seqLen = 2;
            else if ((c & 0xF0) == 0xE0) seqLen = 3;
            else if ((c & 0xF8) == 0xF0) seqLen = 4;
            bool valid = seqLen > 0 && i + seqLen <= 16 && i + seqLen <= total;
            for (size_t k = 1; valid && k < seqLen; ++k) {
                if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80)
                    valid = false;
            }
            if (valid) { out.append(bytes, i, seqLen); i += seqLen; }
            else       { out.push_back('.');           ++i; }
        }
    }
}

void Notepad_plus::SyncBinaryGutter(ScintillaEditView& ed, sptr_t line)
{
    if (binaryMutating_) return;
    if (!IsInBinaryMode(ActiveBuffer())) return;

    sptr_t linePos = ed.Call(SCI_POSITIONFROMLINE, static_cast<uptr_t>(line));
    sptr_t lineLen = ed.Call(SCI_LINELENGTH,      static_cast<uptr_t>(line));
    if (lineLen <= 0) return;
    std::string txt(static_cast<size_t>(lineLen) + 1, '\0');
    ed.Call(SCI_GETLINE, static_cast<uptr_t>(line),
        reinterpret_cast<sptr_t>(txt.data()));
    txt.resize(static_cast<size_t>(lineLen));

    size_t pipe = txt.find(" |");
    if (pipe == std::string::npos) return;
    size_t endPipe = txt.find('|', pipe + 2);
    if (endPipe == std::string::npos) return;

    // Parse up to 16 hex pairs from the hex region (cols 10..pipe).
    std::string bytes;
    size_t q = (pipe > 10) ? 10 : pipe;
    int count = 0;
    while (q + 1 < pipe && count < 16) {
        if (txt[q] == ' ') { ++q; continue; }
        int hi = HexNibble(txt[q]);
        int lo = HexNibble(txt[q + 1]);
        if (hi < 0 || lo < 0) break;
        bytes.push_back(static_cast<char>((hi << 4) | lo));
        q += 2;
        ++count;
    }

    std::string newGutter;
    AppendGutter(newGutter, bytes);

    // Replace the existing gutter text between the two '|' markers. Skip
    // if unchanged to avoid needless edits that move the caret.
    std::string oldGutter = txt.substr(pipe + 2, endPipe - (pipe + 2));
    if (oldGutter == newGutter) return;

    sptr_t absStart = linePos + static_cast<sptr_t>(pipe + 2);
    sptr_t absEnd   = linePos + static_cast<sptr_t>(endPipe);

    binaryMutating_ = true;
    ed.Call(SCI_SETTARGETRANGE,
        static_cast<uptr_t>(absStart), static_cast<sptr_t>(absEnd));
    ed.Call(SCI_REPLACETARGET,
        static_cast<uptr_t>(newGutter.size()),
        reinterpret_cast<sptr_t>(newGutter.c_str()));
    binaryMutating_ = false;
}

bool Notepad_plus::ToggleBinaryMode()
{
    BufferID id = ActiveBuffer();
    if (id == kInvalidBufferID) return false;
    ScintillaEditView& ed = V().editor;

    auto it = binarySnapshot_.find(id);
    if (it != binarySnapshot_.end()) {
        // Turn off — parse current hex dump back to raw bytes so the
        // user's edits in the hex view are preserved.
        sptr_t curLen = ed.Call(SCI_GETLENGTH);
        std::string dumpText(static_cast<size_t>(curLen), '\0');
        if (curLen > 0) {
            ed.Call(SCI_GETTEXT, static_cast<uptr_t>(curLen + 1),
                reinterpret_cast<sptr_t>(dumpText.data()));
            dumpText.resize(static_cast<size_t>(curLen));
        }
        std::string restored = ParseHexDump(dumpText);
        const bool edited   = (restored != it->second.bytes);
        const bool wasDirty = it->second.wasDirty;
        ed.Call(SCI_SETTEXT, 0,
            reinterpret_cast<sptr_t>(restored.c_str()));
        ed.Call(SCI_EMPTYUNDOBUFFER);
        // Savepoint = "matches disk": only when the dump wasn't edited AND
        // the buffer was clean when hex mode was entered. Otherwise leave
        // the document modified so the dirty flag can't be laundered away.
        if (!edited && !wasDirty) ed.Call(SCI_SETSAVEPOINT);
        binarySnapshot_.erase(it);
        // Overwrite mode was only for hex-column editing — back to insert.
        ed.Call(SCI_SETOVERTYPE, 0);
        if (Buffer* b = BufferManager::Instance().Get(id)) {
            ApplyLanguage(ed, b->GetLang());
            b->SetDirty(edited || wasDirty);
            SyncTabLabel(id);
        }
        return false;
    }

    // Turn on — snapshot raw bytes from the editor (UTF-8 storage).
    sptr_t len = ed.Call(SCI_GETLENGTH);
    std::string bytes(static_cast<size_t>(len), '\0');
    if (len > 0) {
        ed.Call(SCI_GETTEXT, static_cast<uptr_t>(len + 1),
            reinterpret_cast<sptr_t>(bytes.data()));
        bytes.resize(static_cast<size_t>(len));
    }
    Buffer* b = BufferManager::Instance().Get(id);
    const bool wasDirty = b && b->IsDirty();
    std::string dump = MakeHexDump(bytes);
    binarySnapshot_.emplace(id, BinaryState{std::move(bytes), wasDirty});
    ed.Call(SCI_SETREADONLY, 0);
    ed.Call(SCI_SETTEXT, 0,
        reinterpret_cast<sptr_t>(dump.c_str()));
    ed.Call(SCI_EMPTYUNDOBUFFER);
    ed.Call(SCI_SETSAVEPOINT);
    // Drop syntax colouring; hex dump is plain monospace.
    ApplyLanguage(ed, LangType::Text);
    // Hex editing feels right in overwrite mode — typing a digit replaces
    // the one under caret instead of shifting the whole table.
    ed.Call(SCI_SETOVERTYPE, 1);
    // Make sure modification notifications reach our WM_NOTIFY handler so
    // the text gutter can sync on every hex-column edit.
    ed.Call(SCI_SETMODEVENTMASK, SC_MODEVENTMASKALL);
    if (b) {
        // Pre-existing unsaved edits survive the toggle; the dump itself
        // starting at a savepoint doesn't make the buffer "saved".
        b->SetDirty(wasDirty);
        SyncTabLabel(id);
    }
    return true;
}

namespace {

// Pretty-print arbitrary JSON text with 4-space indent. Best-effort:
// non-conforming input is passed through; this isn't a validator.
std::string PrettyPrintJson(const std::string& src)
{
    std::string out;
    out.reserve(src.size() + src.size() / 4);
    int  indent  = 0;
    bool inStr   = false;
    bool escape  = false;

    auto NewLine = [&]() {
        out.push_back('\n');
        for (int i = 0; i < indent; ++i) out.append("    ");
    };

    for (size_t i = 0; i < src.size(); ++i) {
        char c = src[i];

        if (inStr) {
            out.push_back(c);
            if (escape)        escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"')  inStr  = false;
            continue;
        }
        if (c == '"') { inStr = true; out.push_back(c); continue; }
        // Skip whitespace outside strings — we'll re-introduce our own.
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;

        switch (c) {
        case '{': case '[': {
            out.push_back(c);
            // Empty container: keep on the same line.
            size_t j = i + 1;
            while (j < src.size() &&
                   (src[j] == ' ' || src[j] == '\t' ||
                    src[j] == '\r' || src[j] == '\n')) ++j;
            if (j < src.size() &&
                ((c == '{' && src[j] == '}') || (c == '[' && src[j] == ']'))) {
                out.push_back(src[j]);
                i = j;
            } else {
                ++indent;
                NewLine();
            }
            break;
        }
        case '}': case ']':
            if (indent > 0) --indent;
            NewLine();
            out.push_back(c);
            break;
        case ',':
            out.push_back(c);
            NewLine();
            break;
        case ':':
            out.push_back(':');
            out.push_back(' ');
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

} // namespace

bool Notepad_plus::ActiveBufferIsJson() const
{
    Buffer* b = BufferManager::Instance().Get(V().activeId);
    if (!b) return false;
    std::wstring p = b->Path();
    if (p.size() < 5) return false;
    std::wstring ext = p.substr(p.size() - 5);
    for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));
    return ext == L".json";
}

bool Notepad_plus::FormatActiveAsJson()
{
    ScintillaEditView& ed = V().editor;
    if (IsInBinaryMode(V().activeId)) return false;

    const sptr_t len = ed.Call(SCI_GETLENGTH);
    if (len <= 0) return false;
    std::string src(static_cast<size_t>(len), '\0');
    ed.Call(SCI_GETTEXT, static_cast<uptr_t>(len + 1),
        reinterpret_cast<sptr_t>(src.data()));
    src.resize(static_cast<size_t>(len));

    std::string out = PrettyPrintJson(src);
    if (out == src) return true;   // already formatted

    ed.Call(SCI_BEGINUNDOACTION);
    ed.Call(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(out.c_str()));
    ed.Call(SCI_ENDUNDOACTION);
    if (Buffer* b = BufferManager::Instance().Get(V().activeId)) {
        b->SetDirty(true);
        SyncTabLabel(V().activeId);
    }
    return true;
}

} // namespace npp
