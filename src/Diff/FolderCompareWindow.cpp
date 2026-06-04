#include "FolderCompareWindow.h"
#include "FolderScanner.h"
#include "TextCompareWindow.h"
#include "../MISC/Common/FileIO.h"
#include "../Parameters/Parameters.h"
#include "../resource.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <cstdio>
#include <thread>

namespace npp {

FolderCompareWindow* FolderCompareWindow::s_active = nullptr;

namespace {
constexpr wchar_t kCls[] = L"NotePadLFolderCompareWnd";
constexpr int kPadX      = 8;
constexpr int kPadY      = 6;
constexpr int kRowH      = 24;
constexpr int kLabelW    = 42;
constexpr int kBrowseW   = 28;
constexpr int kStatusH   = 22;
constexpr int kCheckW    = 90;
constexpr int kCompareW  = 90;

enum : int {
    kIdLeftEdit    = 7201,
    kIdLeftBrowse  = 7202,
    kIdRightEdit   = 7203,
    kIdRightBrowse = 7204,
    kIdRecursive   = 7205,
    kIdCompare     = 7206,
    kIdList        = 7207,
};

void RegisterOnce(HINSTANCE hInst)
{
    static bool reg = false;
    if (reg) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc   = FolderCompareWindow::Proc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kCls;
    ::RegisterClassW(&wc);
    reg = true;
}

// Standard WM_CTLCOLOR-friendly font for plain Win32 children.
HFONT UiFont()
{
    static HFONT f = []() {
        LOGFONTW lf{};
        lf.lfHeight = -14;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        ::wcscpy_s(lf.lfFaceName, L"Segoe UI");
        return ::CreateFontIndirectW(&lf);
    }();
    return f;
}

int CALLBACK BrowseCb(HWND h, UINT m, LPARAM /*l*/, LPARAM data)
{
    if (m == BFFM_INITIALIZED && data) {
        ::SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    }
    return 0;
}
}  // namespace

void FolderCompareWindow::Open(HWND owner, HINSTANCE hInst)
{
    RegisterOnce(hInst);
    if (!s_active) {
        auto* self = new FolderCompareWindow();
        s_active = self;
        HWND hw = ::CreateWindowExW(0, kCls, L"Folder Compare",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1100, 640,
            owner, nullptr, hInst, self);
        if (!hw) { delete self; s_active = nullptr; return; }
    }
    ::ShowWindow(s_active->hwnd_, SW_SHOW);
    ::SetForegroundWindow(s_active->hwnd_);
}

void FolderCompareWindow::BuildChildren(HINSTANCE hInst)
{
    ::InitCommonControls();

    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return ::CreateWindowExW(0, cls, text, style,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    };
    auto mkEx = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return ::CreateWindowExW(ex, cls, text, style,
            0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    };

    leftLabel_   = mk(L"STATIC", L"Left:",  WS_CHILD | WS_VISIBLE | SS_LEFT, 0);
    leftEdit_    = mkEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, kIdLeftEdit);
    leftBrowse_  = mk(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdLeftBrowse);
    rightLabel_  = mk(L"STATIC", L"Right:", WS_CHILD | WS_VISIBLE | SS_LEFT, 0);
    rightEdit_   = mkEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, kIdRightEdit);
    rightBrowse_ = mk(L"BUTTON", L"...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, kIdRightBrowse);
    recursive_   = mk(L"BUTTON", L"&Recursive",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, kIdRecursive);
    ::SendMessageW(recursive_, BM_SETCHECK, BST_CHECKED, 0);
    compareBtn_  = mk(L"BUTTON", L"&Compare",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, kIdCompare);

    HFONT f = UiFont();
    for (HWND w : { leftLabel_, leftEdit_, leftBrowse_, rightLabel_, rightEdit_,
                    rightBrowse_, recursive_, compareBtn_ }) {
        ::SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    }

    statusBar_ = ::CreateWindowExW(0, STATUSCLASSNAMEW, L"Pick two folders and press Compare.",
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(9101)), hInst, nullptr);

    list_.Create(hwnd_, hInst, kIdList);
    list_.SetOnActivate([this](int r){ OnActivateEntry(r); });

    // Apply dark theme to the self-drawn list when active.
    if (Parameters::Instance().DarkMode()) {
        list_.bg_        = RGB(0x1E, 0x1E, 0x1E);
        list_.fg_        = RGB(0xE0, 0xE0, 0xE0);
        list_.bgHeader_  = RGB(0x2A, 0x2A, 0x2A);
        list_.bgAlt_     = RGB(0x24, 0x24, 0x24);
        list_.bgSelect_  = RGB(0x3C, 0x5A, 0x80);
        list_.bgDiff_    = RGB(0x70, 0x60, 0x20);
        list_.bgMissing_ = RGB(0x2E, 0x2E, 0x2E);
        list_.fgMuted_   = RGB(0x90, 0x90, 0x90);
        list_.fgGutter_  = RGB(0xC0, 0xC0, 0xC0);
        list_.colSep_    = RGB(0x40, 0x40, 0x40);
    }

    // Prefill last-used roots so a repeat compare is one click away.
    auto& params = Parameters::Instance();
    if (!params.cmpFolderLeft.empty())
        ::SetWindowTextW(leftEdit_,  params.cmpFolderLeft.c_str());
    if (!params.cmpFolderRight.empty())
        ::SetWindowTextW(rightEdit_, params.cmpFolderRight.c_str());
}

void FolderCompareWindow::Layout()
{
    if (!hwnd_) return;
    RECT rc; ::GetClientRect(hwnd_, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;

    int y = kPadY;
    int halfW = (W - kPadX * 3) / 2;

    int labW = kLabelW;
    int editW = halfW - labW - kBrowseW - 4;

    int lx = kPadX;
    ::SetWindowPos(leftLabel_,  nullptr, lx,                     y + 4,  labW,           kRowH - 6, SWP_NOZORDER);
    ::SetWindowPos(leftEdit_,   nullptr, lx + labW,              y,      editW,          kRowH,     SWP_NOZORDER);
    ::SetWindowPos(leftBrowse_, nullptr, lx + labW + editW + 2,  y,      kBrowseW,       kRowH,     SWP_NOZORDER);

    int rx = kPadX * 2 + halfW;
    ::SetWindowPos(rightLabel_,  nullptr, rx,                    y + 4,  labW,           kRowH - 6, SWP_NOZORDER);
    ::SetWindowPos(rightEdit_,   nullptr, rx + labW,             y,      editW,          kRowH,     SWP_NOZORDER);
    ::SetWindowPos(rightBrowse_, nullptr, rx + labW + editW + 2, y,      kBrowseW,       kRowH,     SWP_NOZORDER);

    y += kRowH + kPadY;
    ::SetWindowPos(recursive_,  nullptr, kPadX, y + 4, kCheckW,   kRowH - 6, SWP_NOZORDER);
    ::SetWindowPos(compareBtn_, nullptr, W - kCompareW - kPadX, y, kCompareW, kRowH, SWP_NOZORDER);

    y += kRowH + kPadY;
    int bottom = H - kStatusH;
    ::SetWindowPos(list_.Hwnd(), nullptr, 0, y, W, bottom - y, SWP_NOZORDER);
}

void FolderCompareWindow::BrowseInto(HWND edit)
{
    wchar_t cur[MAX_PATH] = {0};
    ::GetWindowTextW(edit, cur, MAX_PATH);
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"Pick a folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    bi.lpfn      = BrowseCb;
    bi.lParam    = cur[0] ? reinterpret_cast<LPARAM>(cur) : 0;
    LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t picked[MAX_PATH];
    if (::SHGetPathFromIDListW(pidl, picked)) {
        ::SetWindowTextW(edit, picked);
    }
    ::CoTaskMemFree(pidl);
}

void FolderCompareWindow::RunCompare()
{
    if (scanning_) return;   // already in flight

    wchar_t L[MAX_PATH], R[MAX_PATH];
    ::GetWindowTextW(leftEdit_,  L, MAX_PATH);
    ::GetWindowTextW(rightEdit_, R, MAX_PATH);
    if (!*L || !*R) {
        ::MessageBoxW(hwnd_, L"Pick a folder for both sides.", L"Folder Compare", MB_OK | MB_ICONINFORMATION);
        return;
    }
    DWORD la = ::GetFileAttributesW(L), ra = ::GetFileAttributesW(R);
    if (la == INVALID_FILE_ATTRIBUTES || !(la & FILE_ATTRIBUTE_DIRECTORY) ||
        ra == INVALID_FILE_ATTRIBUTES || !(ra & FILE_ATTRIBUTE_DIRECTORY)) {
        ::MessageBoxW(hwnd_, L"Both paths must be existing folders.", L"Folder Compare", MB_OK | MB_ICONWARNING);
        return;
    }

    leftRoot_  = L;
    rightRoot_ = R;
    auto& params = Parameters::Instance();
    params.cmpFolderLeft  = leftRoot_;
    params.cmpFolderRight = rightRoot_;
    params.Save();

    FolderScanOptions opt;
    opt.recursive = ::SendMessageW(recursive_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    scanning_ = true;
    ::EnableWindow(compareBtn_, FALSE);
    ::SetWindowTextW(statusBar_, L"Scanning...");

    HWND target = hwnd_;
    std::wstring lr = leftRoot_, rr = rightRoot_;
    // Detach: the worker owns the result vector; the UI thread takes
    // ownership when it receives kMsgScanDone. If the window was closed
    // mid-scan (alive_ false, or the post fails), the worker frees it.
    std::thread([target, lr, rr, opt, alive = alive_]() {
        auto* result = new std::vector<FolderEntry>(ScanFolders(lr, rr, opt));
        if (!alive->load() ||
            !::PostMessageW(target, kMsgScanDone, 0,
                            reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

void FolderCompareWindow::DeliverResults(std::vector<FolderEntry>* entries)
{
    scanning_ = false;
    ::EnableWindow(compareBtn_, TRUE);
    if (!entries) return;

    size_t same = 0, diff = 0, lo = 0, ro = 0;
    for (auto& e : *entries) {
        switch (e.status) {
        case FolderDiffStatus::Same:      ++same; break;
        case FolderDiffStatus::Different: ++diff; break;
        case FolderDiffStatus::LeftOnly:  ++lo;   break;
        case FolderDiffStatus::RightOnly: ++ro;   break;
        default: break;
        }
    }
    UpdateStatus(entries->size(), same, diff, lo, ro);
    list_.SetEntries(std::move(*entries));
    delete entries;
    ::SetFocus(list_.Hwnd());
}

void FolderCompareWindow::UpdateStatus(size_t total, size_t same, size_t diff,
                                        size_t leftOnly, size_t rightOnly)
{
    if (!statusBar_) return;
    wchar_t buf[192];
    ::swprintf_s(buf,
        L"Total: %zu    Same: %zu    Different: %zu    Left only: %zu    Right only: %zu",
        total, same, diff, leftOnly, rightOnly);
    ::SetWindowTextW(statusBar_, buf);
}

void FolderCompareWindow::OnActivateEntry(int row)
{
    const FolderEntry* e = list_.SelectedEntry();
    if (!e || e->isDir) return;
    if (e->status == FolderDiffStatus::LeftOnly || e->status == FolderDiffStatus::RightOnly) {
        ::MessageBoxW(hwnd_, L"This file exists on only one side.",
            L"Folder Compare", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring lp = leftRoot_  + L"\\" + e->relPath;
    std::wstring rp = rightRoot_ + L"\\" + e->relPath;
    std::vector<char> lb, rb;
    if (!ReadFileAll(lp, lb) || !ReadFileAll(rp, rb)) {
        ::MessageBoxW(hwnd_, L"Cannot read one of the files.", L"Folder Compare",
            MB_OK | MB_ICONWARNING);
        return;
    }
    LineDiffOptions opt;
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    TextCompareWindow::Open(hwnd_, hInst,
        e->relPath + L" (left)",  std::string(lb.begin(), lb.end()),
        e->relPath + L" (right)", std::string(rb.begin(), rb.end()), opt);
    (void)row;
}

LRESULT CALLBACK FolderCompareWindow::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    FolderCompareWindow* self = nullptr;
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(l);
        self = static_cast<FolderCompareWindow*>(cs->lpCreateParams);
        self->hwnd_ = h;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<FolderCompareWindow*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self) return ::DefWindowProcW(h, m, w, l);

    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(h, GWLP_HINSTANCE));

    switch (m) {
    case WM_CREATE:
        self->BuildChildren(hInst);
        return 0;
    case WM_SIZE:
        self->Layout();
        if (self->statusBar_) ::SendMessageW(self->statusBar_, WM_SIZE, 0, 0);
        return 0;
    case WM_COMMAND: {
        WORD id = LOWORD(w);
        if (id == kIdLeftBrowse)  { self->BrowseInto(self->leftEdit_);  return 0; }
        if (id == kIdRightBrowse) { self->BrowseInto(self->rightEdit_); return 0; }
        if (id == kIdCompare)     { self->RunCompare();                 return 0; }
        return 0;
    }
    case kMsgScanDone:
        self->DeliverResults(reinterpret_cast<std::vector<FolderEntry>*>(l));
        return 0;
    case WM_CLOSE:
        ::DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        self->alive_->store(false);   // tell an in-flight scan thread to bail
        if (s_active == self) s_active = nullptr;
        delete self;
        return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
}

} // namespace npp
