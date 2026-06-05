#include "Notepad_plus_Window.h"
#include "../Parameters/Parameters.h"
#include "../Parameters/LangType.h"
#include "../Parameters/Stylers.h"
#include "../ScintillaComponent/BufferManager.h"
#include "../MISC/Common/StringUtil.h"
#include "../MISC/Common/FileIO.h"
#include "../Diff/TextCompareWindow.h"
#include "../Diff/HexCompareWindow.h"
#include "../Diff/FolderCompareWindow.h"
#include "../resource.h"

#include <Scintilla.h>
#include <ScintillaMessages.h>

#include <commctrl.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <cstdint>

#pragma comment(lib, "dwmapi.lib")

namespace npp {

static constexpr wchar_t kClassName[] = L"NotePadL_MainFrame";

const wchar_t* Notepad_plus_Window::ClassName() { return kClassName; }

// ---- Dark mode helpers -------------------------------------------------

// DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 1809+, officially Win10 2004+)
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
// Win11 22H2+ backdrop + rounded corners
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2  // Mica
#endif
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

static void SetTitleBarDark(HWND hwnd, bool dark)
{
    BOOL useDark = dark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
        &useDark, sizeof(useDark));
}

static void ApplyWin11Chrome(HWND hwnd)
{
    // Rounded corners (ignored on older Windows).
    DWORD corner = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corner, sizeof(corner));
    // Mica backdrop — pulls desktop wallpaper through the frame.
    DWORD backdrop = DWMSBT_MAINWINDOW;
    ::DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
        &backdrop, sizeof(backdrop));
}

static HBRUSH g_chromeBrush = nullptr;

static HBRUSH GetChromeBrush()
{
    const UiPalette& u = Ui();
    if (g_chromeBrush) { ::DeleteObject(g_chromeBrush); g_chromeBrush = nullptr; }
    g_chromeBrush = ::CreateSolidBrush(u.chromeBg);
    return g_chromeBrush;
}

static void ApplyFrameTheme(HWND hwnd, HWND statusBar, HWND toolbar)
{
    bool isDarkFam = Parameters::Instance().DarkMode();
    SetTitleBarDark(hwnd, isDarkFam);
    GetChromeBrush();  // refresh for current theme

    const UiPalette& u = Ui();

    if (toolbar) {
        ::InvalidateRect(toolbar, nullptr, TRUE);
    }
    if (statusBar) {
        ::SendMessageW(statusBar, SB_SETBKCOLOR, 0,
            static_cast<LPARAM>(u.statusBg));
        ::InvalidateRect(statusBar, nullptr, TRUE);
    }

    ::InvalidateRect(hwnd, nullptr, TRUE);
    ::UpdateWindow(hwnd);
}

namespace {
    constexpr int kSmartHighlightIndic = 30;
    constexpr int kBinaryLinkIndic     = 29;

    // Toolbar tip text indexed by command id (filled in CreateToolbar,
    // consumed by WM_NOTIFY → TTN_GETDISPINFOW).
    std::unordered_map<int, std::wstring> g_tbTips;

    // Column of the first hex digit of byte `i` in a MakeHexDump line.
    // Layout: 8-char offset + "  " + 16*"XX " cells + extra " " after byte 7.
    constexpr int HexByteCol(int i) { return 10 + i * 3 + (i >= 8 ? 1 : 0); }
    constexpr int kHexRegionEnd = HexByteCol(15) + 2;   // exclusive

    bool IsHexChar(wchar_t c) {
        return (c >= L'0' && c <= L'9') ||
               (c >= L'a' && c <= L'f') ||
               (c >= L'A' && c <= L'F');
    }

    // Per-editor subclass that locks typing to the hex columns while the
    // buffer is in binary mode. Stores the chained WndProc, the Notepad_plus
    // we talk back to, and which view slot we belong to.
    struct BinaryEditHook {
        WNDPROC        prev = nullptr;
        Notepad_plus*  app  = nullptr;
        int            view = -1;
    };
    std::unordered_map<HWND, BinaryEditHook> g_binaryHooks;

    LRESULT CALLBACK BinaryEditSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l);

    void InstallBinaryEditHook(HWND h, Notepad_plus* app, int view) {
        if (!h || g_binaryHooks.count(h)) return;
        LONG_PTR prev = ::SetWindowLongPtrW(h, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(BinaryEditSubclassProc));
        g_binaryHooks[h] = { reinterpret_cast<WNDPROC>(prev), app, view };
    }

    // Per-editor subclass that owns WM_CONTEXTMENU so we can inject the
    // "Format JSON" item when the active buffer is a .json file.
    struct EditorCtxHook {
        WNDPROC        prev = nullptr;
        Notepad_plus*  app  = nullptr;
        HWND           frame = nullptr;
    };
    std::unordered_map<HWND, EditorCtxHook> g_ctxHooks;

    LRESULT CALLBACK EditorCtxSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto it = g_ctxHooks.find(h);
        if (it == g_ctxHooks.end())
            return ::DefWindowProcW(h, m, w, l);
        EditorCtxHook hk = it->second;
        if (m == WM_CONTEXTMENU && hk.app && hk.frame) {
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            if (pt.x == -1 && pt.y == -1) {
                RECT rc; ::GetWindowRect(h, &rc);
                pt.x = rc.left + 8; pt.y = rc.top + 8;
            }
            HMENU menu = ::CreatePopupMenu();
            ::AppendMenuW(menu, MF_STRING, IDM_EDIT_CUT,   L"Cut\tCtrl+X");
            ::AppendMenuW(menu, MF_STRING, IDM_EDIT_COPY,  L"Copy\tCtrl+C");
            ::AppendMenuW(menu, MF_STRING, IDM_EDIT_PASTE, L"Paste\tCtrl+V");
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(menu, MF_STRING, IDM_EDIT_SELECTALL, L"Select All\tCtrl+A");
            if (hk.app->ActiveBufferIsJson()) {
                ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                ::AppendMenuW(menu, MF_STRING, IDM_EDIT_JSON_FORMAT,
                              L"Format JSON");
            }
            UINT cmd = ::TrackPopupMenu(menu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                pt.x, pt.y, 0, hk.frame, nullptr);
            ::DestroyMenu(menu);
            if (cmd != 0) {
                ::SendMessageW(hk.frame, WM_COMMAND,
                    MAKEWPARAM(cmd, 0), 0);
            }
            return 0;
        }
        return ::CallWindowProcW(hk.prev, h, m, w, l);
    }

    void InstallEditorCtxHook(HWND h, Notepad_plus* app, HWND frame) {
        if (!h || g_ctxHooks.count(h)) return;
        LONG_PTR prev = ::SetWindowLongPtrW(h, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(EditorCtxSubclassProc));
        g_ctxHooks[h] = { reinterpret_cast<WNDPROC>(prev), app, frame };
    }

    LRESULT CALLBACK BinaryEditSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto it = g_binaryHooks.find(h);
        if (it == g_binaryHooks.end())
            return ::DefWindowProcW(h, m, w, l);
        BinaryEditHook hk = it->second;
        Notepad_plus* app = hk.app;
        int view = hk.view;
        if (!app || !app->IsInBinaryMode(app->V(view).activeId))
            return ::CallWindowProcW(hk.prev, h, m, w, l);

        ScintillaEditView& ed = app->V(view).editor;

        if (m == WM_KEYDOWN) {
            // Block structural edits that would break column alignment.
            switch (w) {
            case VK_BACK: case VK_DELETE: case VK_RETURN:
            case VK_TAB:  case VK_INSERT:
                return 0;
            default:
                break;
            }
            // Paste/cut would splice arbitrary text into the fixed-width hex
            // table — Scintilla binds them on the keydown. Copy stays usable.
            if ((::GetKeyState(VK_CONTROL) & 0x8000) &&
                (w == 'V' || w == 'X'))
                return 0;
        }

        if (m == WM_CHAR) {
            wchar_t ch = static_cast<wchar_t>(w);
            if (!IsHexChar(ch)) return 0;

            sptr_t pos     = ed.Call(SCI_GETCURRENTPOS);
            sptr_t ln      = ed.Call(SCI_LINEFROMPOSITION, static_cast<uptr_t>(pos));
            sptr_t linePos = ed.Call(SCI_POSITIONFROMLINE, static_cast<uptr_t>(ln));
            int    col     = static_cast<int>(pos - linePos);

            // Snap caret to the next hex-digit cell if we're on a gap/offset.
            int byteIdx = -1, sub = 0;
            for (int i = 0; i < 16; ++i) {
                int c0 = HexByteCol(i);
                if (col == c0)      { byteIdx = i; sub = 0; break; }
                if (col == c0 + 1)  { byteIdx = i; sub = 1; break; }
            }
            if (byteIdx < 0) {
                for (int i = 0; i < 16; ++i) {
                    if (HexByteCol(i) >= col) { byteIdx = i; sub = 0; break; }
                }
                if (byteIdx < 0) return 0;   // past hex region on this line
                pos = linePos + HexByteCol(byteIdx);
                ed.Call(SCI_GOTOPOS, static_cast<uptr_t>(pos));
            }

            // Overwrite exactly the single digit at the caret.
            char buf[2] = { static_cast<char>(ch), 0 };
            ed.Call(SCI_SETTARGETRANGE,
                    static_cast<uptr_t>(pos),
                    static_cast<sptr_t>(pos + 1));
            ed.Call(SCI_REPLACETARGET, 1, reinterpret_cast<sptr_t>(buf));

            // Advance to the next hex cell (skipping the separating space).
            int nextCol;
            if (sub == 0) nextCol = HexByteCol(byteIdx) + 1;
            else {
                int ni = byteIdx + 1;
                if (ni < 16) nextCol = HexByteCol(ni);
                else         nextCol = -1;   // wrap to next line below
            }
            if (nextCol >= 0) {
                ed.Call(SCI_GOTOPOS,
                    static_cast<uptr_t>(linePos + nextCol));
            } else {
                sptr_t total = ed.Call(SCI_GETLINECOUNT);
                if (ln + 1 < total) {
                    sptr_t np = ed.Call(SCI_POSITIONFROMLINE,
                        static_cast<uptr_t>(ln + 1));
                    ed.Call(SCI_GOTOPOS,
                        static_cast<uptr_t>(np + HexByteCol(0)));
                }
            }

            app->SyncBinaryGutter(ed, ln);
            return 0;
        }
        return ::CallWindowProcW(hk.prev, h, m, w, l);
    }

    // In binary (hex-dump) mode: if the selection lies inside the hex-columns
    // region, paint the matching byte(s) in the text gutter with indicator 29.
    // Line layout (matches MakeHexDump): 8-char offset + "  " + 16 hex cells
    // (3 chars each, +1 extra space after byte 7) + " |" + gutter + "|".
    // Paint one sub-range [s, e] — called once per Scintilla selection.
    void PaintBinaryLinkRange(ScintillaEditView& ed, sptr_t s, sptr_t e) {
        if (e < s) std::swap(s, e);
        sptr_t lineS = ed.Call(SCI_LINEFROMPOSITION, static_cast<uptr_t>(s));
        sptr_t lineE = ed.Call(SCI_LINEFROMPOSITION, static_cast<uptr_t>(e));
        for (sptr_t ln = lineS; ln <= lineE; ++ln) {
            sptr_t linePos = ed.Call(SCI_POSITIONFROMLINE, static_cast<uptr_t>(ln));
            sptr_t lineLen = ed.Call(SCI_LINELENGTH,      static_cast<uptr_t>(ln));
            if (lineLen <= 0) continue;
            std::string txt(static_cast<size_t>(lineLen) + 1, '\0');
            ed.Call(SCI_GETLINE, static_cast<uptr_t>(ln),
                    reinterpret_cast<sptr_t>(txt.data()));
            txt.resize(static_cast<size_t>(lineLen));

            sptr_t colStart = (ln == lineS) ? (s - linePos) : 0;
            sptr_t colEnd   = (ln == lineE) ? (e - linePos) : lineLen;
            if (ln != lineE && colEnd == 0) continue;
            if (colEnd <= colStart) {
                // Caret-only: still highlight the byte under caret.
                colEnd = colStart + 1;
            }

            int byteFirst = -1, byteLast = -1;
            for (int i = 0; i < 16; ++i) {
                int c0 = 10 + i * 3 + (i >= 8 ? 1 : 0);
                int c1 = c0 + 2;
                if (c1 > colStart && c0 < colEnd) {
                    if (byteFirst < 0) byteFirst = i;
                    byteLast = i;
                }
            }
            if (byteFirst < 0) continue;

            size_t pipe = txt.find(" |");
            if (pipe == std::string::npos) continue;
            size_t g = pipe + 2;
            int byteIdx = 0;
            size_t hiStart = 0, hiEnd = 0;
            bool started = false;
            // Walk each glyph in the gutter; include any glyph whose byte
            // range [byteIdx, byteIdx+seqLen) overlaps [byteFirst, byteLast].
            while (byteIdx < 16 && g < txt.size() && txt[g] != '|') {
                unsigned char c = static_cast<unsigned char>(txt[g]);
                size_t seqLen = 1;
                if      ((c & 0x80) == 0x00) seqLen = 1;
                else if ((c & 0xE0) == 0xC0) seqLen = 2;
                else if ((c & 0xF0) == 0xE0) seqLen = 3;
                else if ((c & 0xF8) == 0xF0) seqLen = 4;
                int nextIdx = byteIdx + static_cast<int>(seqLen);
                bool overlaps = (nextIdx > byteFirst) && (byteIdx <= byteLast);
                if (overlaps) {
                    if (!started) { hiStart = g; started = true; }
                    hiEnd = g + seqLen;
                }
                byteIdx = nextIdx;
                g += seqLen;
                if (byteIdx > byteLast) break;
            }
            if (!started) continue;
            sptr_t absS = linePos + static_cast<sptr_t>(hiStart);
            sptr_t absE = linePos + static_cast<sptr_t>(hiEnd);
            ed.Call(SCI_INDICATORFILLRANGE,
                    static_cast<uptr_t>(absS), absE - absS);
        }
    }

    void RefreshBinaryLink(ScintillaEditView& ed) {
        sptr_t docLen = ed.Call(SCI_GETLENGTH);
        ed.Call(SCI_SETINDICATORCURRENT, kBinaryLinkIndic);
        ed.Call(SCI_INDICATORCLEARRANGE, 0, docLen);
        ed.Call(SCI_INDICSETSTYLE, kBinaryLinkIndic, INDIC_STRAIGHTBOX);
        ed.Call(SCI_INDICSETFORE,  kBinaryLinkIndic, 0xFF6030); // blue (BGR)
        ed.Call(SCI_INDICSETALPHA, kBinaryLinkIndic, 120);
        ed.Call(SCI_INDICSETOUTLINEALPHA, kBinaryLinkIndic, 200);
        ed.Call(SCI_INDICSETUNDER, kBinaryLinkIndic, 1);

        // Iterate every sub-selection: handles both stream and rectangular.
        sptr_t n = ed.Call(SCI_GETSELECTIONS);
        if (n <= 0) n = 1;
        for (sptr_t i = 0; i < n; ++i) {
            sptr_t s = ed.Call(SCI_GETSELECTIONNSTART, static_cast<uptr_t>(i));
            sptr_t e = ed.Call(SCI_GETSELECTIONNEND,   static_cast<uptr_t>(i));
            PaintBinaryLinkRange(ed, s, e);
        }
    }

    void ClearSmartHighlight(ScintillaEditView& ed) {
        sptr_t len = ed.Call(SCI_GETLENGTH);
        ed.Call(SCI_SETINDICATORCURRENT, kSmartHighlightIndic);
        ed.Call(SCI_INDICATORCLEARRANGE, 0, len);
    }

    // Highlight every occurrence of `ed`'s current selection using indicator
    // 30. Scans only the visible lines plus a margin — the previous whole-
    // document scan made every click and shift-drag an O(doc) pass on large
    // files. SC_UPDATE_V_SCROLL re-runs it so occurrences scrolled into view
    // get painted. No-op if selection is empty / multi-line / huge.
    void RefreshSmartHighlight(ScintillaEditView& ed) {
        sptr_t s = ed.Call(SCI_GETSELECTIONSTART);
        sptr_t e = ed.Call(SCI_GETSELECTIONEND);
        if (e <= s) { ClearSmartHighlight(ed); return; }
        sptr_t lineS = ed.Call(SCI_LINEFROMPOSITION, static_cast<uptr_t>(s));
        sptr_t lineE = ed.Call(SCI_LINEFROMPOSITION, static_cast<uptr_t>(e));
        if (lineS != lineE) { ClearSmartHighlight(ed); return; }
        sptr_t n = e - s;
        if (n < 1 || n > 256) { ClearSmartHighlight(ed); return; }

        std::string buf(static_cast<size_t>(n) + 1, '\0');
        Sci_TextRangeFull tr{};
        tr.chrg.cpMin = s;
        tr.chrg.cpMax = e;
        tr.lpstrText  = buf.data();
        ed.Call(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>(&tr));
        // Skip if selection contains whitespace/control chars (not an identifier).
        for (sptr_t i = 0; i < n; ++i) {
            unsigned char c = static_cast<unsigned char>(buf[i]);
            if (c <= 0x20) { ClearSmartHighlight(ed); return; }
        }

        // (Re)configure the indicator each call — cheap, makes per-buffer use safe.
        // Follows the theme's accent so the highlight always reads as "same family"
        // as selection/tabs rather than a hard-coded orange.
        ed.Call(SCI_INDICSETSTYLE, kSmartHighlightIndic, INDIC_ROUNDBOX);
        ed.Call(SCI_INDICSETFORE,  kSmartHighlightIndic, Ui().accent);
        ed.Call(SCI_INDICSETALPHA, kSmartHighlightIndic, 70);
        ed.Call(SCI_INDICSETOUTLINEALPHA, kSmartHighlightIndic, 140);
        ed.Call(SCI_SETINDICATORCURRENT, kSmartHighlightIndic);

        sptr_t docLen = ed.Call(SCI_GETLENGTH);
        ed.Call(SCI_INDICATORCLEARRANGE, 0, docLen);

        // Visible range ± margin, mapped to document lines (wrap-aware).
        constexpr sptr_t kMarginLines = 100;
        sptr_t firstVis  = ed.Call(SCI_GETFIRSTVISIBLELINE);
        sptr_t onScreen  = ed.Call(SCI_LINESONSCREEN);
        sptr_t docFirst  = ed.Call(SCI_DOCLINEFROMVISIBLE,
            static_cast<uptr_t>(firstVis));
        sptr_t docLast   = ed.Call(SCI_DOCLINEFROMVISIBLE,
            static_cast<uptr_t>(firstVis + onScreen));
        sptr_t lastLine  = ed.Call(SCI_GETLINECOUNT) - 1;
        sptr_t scanFirst = std::max<sptr_t>(0, docFirst - kMarginLines);
        sptr_t scanLast  = std::min<sptr_t>(lastLine, docLast + kMarginLines);
        sptr_t scanStart = ed.Call(SCI_POSITIONFROMLINE,
            static_cast<uptr_t>(scanFirst));
        sptr_t scanEnd   = ed.Call(SCI_GETLINEENDPOSITION,
            static_cast<uptr_t>(scanLast));

        ed.Call(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
        sptr_t start = scanStart;
        while (start < scanEnd) {
            ed.Call(SCI_SETTARGETRANGE, static_cast<uptr_t>(start), scanEnd);
            sptr_t pos = ed.Call(SCI_SEARCHINTARGET,
                static_cast<uptr_t>(n),
                reinterpret_cast<sptr_t>(buf.data()));
            if (pos < 0) break;
            sptr_t tend = ed.Call(SCI_GETTARGETEND);
            ed.Call(SCI_INDICATORFILLRANGE,
                static_cast<uptr_t>(pos), tend - pos);
            start = (tend == pos) ? pos + 1 : tend;
        }
    }

    int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    // Decode one "file:///..."-style URI (UTF-8, percent-encoded) → wide path.
    std::wstring DecodeFileUri(const char* uri, size_t len) {
        std::string raw;
        raw.reserve(len);
        const char* p = uri;
        const char* end = uri + len;
        // Strip "file://" or "file:///" prefix.
        if (len >= 7 && _strnicmp(p, "file://", 7) == 0) {
            p += 7;
            if (p < end && *p == '/') ++p;
        }
        while (p < end) {
            if (*p == '%' && p + 2 < end) {
                int hi = hexVal(p[1]);
                int lo = hexVal(p[2]);
                if (hi >= 0 && lo >= 0) {
                    raw.push_back(static_cast<char>((hi << 4) | lo));
                    p += 3; continue;
                }
            }
            raw.push_back(*p++);
        }
        int wn = ::MultiByteToWideChar(CP_UTF8, 0, raw.data(),
                                       static_cast<int>(raw.size()), nullptr, 0);
        std::wstring w(wn, L'\0');
        if (wn > 0)
            ::MultiByteToWideChar(CP_UTF8, 0, raw.data(),
                                  static_cast<int>(raw.size()), w.data(), wn);
        // Normalize forward slashes.
        for (auto& c : w) if (c == L'/') c = L'\\';
        return w;
    }
}

bool Notepad_plus_Window::Init(HINSTANCE hInst, int nCmdShow)
{
    hInst_ = hInst;

    INITCOMMONCONTROLSEX icc{ sizeof(icc),
        ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_COOL_CLASSES |
        ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
    ::InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = &Notepad_plus_Window::StaticWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = ::LoadIconW(hInst, MAKEINTRESOURCEW(IDI_NOTEPADL));
    if (!wc.hIcon) wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // painted in WM_ERASEBKGND
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc)) return false;

    menu_  = ::LoadMenuW(hInst, MAKEINTRESOURCEW(IDR_MAIN_MENU));
    accel_ = ::LoadAcceleratorsW(hInst, MAKEINTRESOURCEW(IDR_MAIN_ACCEL));

    Parameters::Instance().Load();

    hwnd_ = ::CreateWindowExW(
        0, kClassName, L"NotePad-L",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 750,
        nullptr, menu_, hInst, this);
    if (!hwnd_) return false;

    ::ShowWindow(hwnd_, nCmdShow);
    ::UpdateWindow(hwnd_);
    return true;
}

bool Notepad_plus_Window::OpenFromCommandLine(const std::wstring& path)
{
    if (path.empty()) return false;
    // Shell associations usually hand us an absolute path, but users launching
    // from a terminal may pass a relative one. Resolve against the current
    // working directory; long-path-aware APIs handle >MAX_PATH results.
    DWORD need = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    std::wstring full = path;
    if (need) {
        std::wstring buf(need, L'\0');
        DWORD got = ::GetFullPathNameW(path.c_str(), need, buf.data(), nullptr);
        if (got && got < need) { buf.resize(got); full = std::move(buf); }
    }
    BufferID id = app_.DoOpen(full);
    if (id != kInvalidBufferID) RebuildRecentMenu();
    return id != kInvalidBufferID;
}

int Notepad_plus_Window::MessageLoop()
{
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND findHwnd = app_.FindDlg().Hwnd();
        // Route messages to the modeless find/replace dialog first so that
        // standard edit shortcuts (Ctrl+C/V/X/Z, Tab navigation, etc.) and
        // dialog navigation work inside it instead of being eaten by the
        // frame's accelerator table.
        if (findHwnd && (msg.hwnd == findHwnd || ::IsChild(findHwnd, msg.hwnd))) {
            if (::IsDialogMessageW(findHwnd, &msg)) continue;
        }
        if (!::TranslateAcceleratorW(hwnd_, accel_, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK Notepad_plus_Window::StaticWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Notepad_plus_Window* self = nullptr;
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<Notepad_plus_Window*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    } else {
        self = reinterpret_cast<Notepad_plus_Window*>(
            ::GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->WndProc(h, m, w, l);
    return ::DefWindowProcW(h, m, w, l);
}

void Notepad_plus_Window::OnCreate(HWND h)
{
    statusBar_ = ::CreateWindowExW(
        0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, h, nullptr, hInst_, nullptr);

    LOGFONTW lf{};
    lf.lfHeight = -12;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT sbFont = ::CreateFontIndirectW(&lf);
    if (sbFont)
        ::SendMessageW(statusBar_, WM_SETFONT,
            reinterpret_cast<WPARAM>(sbFont), TRUE);

    int widths[] = { 240, 380, 470, 560, -1 };
    ::SendMessageW(statusBar_, SB_SETPARTS,
        ARRAYSIZE(widths), reinterpret_cast<LPARAM>(widths));

    CreateToolbar();

    app_.Attach(h, hInst_);

    // Accept file drops on the frame (non-editor areas).
    ::DragAcceptFiles(h, TRUE);
    // UAC: allow lower-IL Explorer drags to reach our window.
    ::ChangeWindowMessageFilterEx(h, WM_DROPFILES,       MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(h, WM_COPYDATA,        MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(h, 0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ALLOW, nullptr);

    // Tab context menu callback needs the frame HWND, so wire it here.
    WireTabContextForView(0);

    // Right-click context menu on the editor (Format JSON, etc.).
    InstallEditorCtxHook(app_.V(0).editor.Hwnd(), &app_, hwnd_);

    // Win11 rounded corners + Mica backdrop (silently ignored pre-22H2).
    ApplyWin11Chrome(h);
    // Apply theme (light or dark) to frame chrome.
    ApplyFrameTheme(h, statusBar_, toolbar_);

    // If WinMain handed us files (shell association, drag-onto-exe, terminal),
    // open them instead of creating the placeholder "new 1" tab — otherwise
    // the user sees an empty scratch tab next to their file on every launch.
    bool opened = false;
    for (const auto& path : initialFiles_) {
        if (OpenFromCommandLine(path)) opened = true;
    }
    if (!opened) app_.DoNew();
    RebuildRecentMenu();
    BuildLanguageMenu();
    if (Parameters::Instance().WordWrap()) {
        for (int v = 0; v < 2; ++v) {
            if (app_.V(v).editor.Hwnd())
                app_.V(v).editor.Call(SCI_SETWRAPMODE, SC_WRAP_WORD);
        }
    }
    UpdateCheckedMenus();
    OnSize(h);
    app_.UpdateTitle(h);
    app_.UpdateStatusBar(statusBar_);
    app_.Editor().SetFocus();
}

void Notepad_plus_Window::OnSize(HWND h)
{
    RECT rc;
    ::GetClientRect(h, &rc);

    int sbH = 0;
    if (statusBar_) {
        ::SendMessageW(statusBar_, WM_SIZE, 0, 0);
        RECT sbRc;
        ::GetWindowRect(statusBar_, &sbRc);
        sbH = sbRc.bottom - sbRc.top;
    }

    const int tbH = ToolbarHeight();
    if (toolbar_ && tbH > 0) {
        ::SetWindowPos(toolbar_, nullptr,
            rc.left, rc.top, rc.right - rc.left, tbH,
            SWP_NOZORDER | SWP_SHOWWINDOW);
        rc.top += tbH;
    }
    app_.Layout(rc, sbH);
}

int Notepad_plus_Window::ToolbarHeight() const
{
    if (!toolbar_) return 0;
    RECT r{};
    ::SendMessageW(toolbar_, TB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&r));
    int h = r.bottom - r.top;
    if (h <= 0) h = 26;
    return h + 4;
}

void Notepad_plus_Window::RebuildRecentMenu()
{
    HMENU fileMenu = ::GetSubMenu(menu_, 0);
    if (!fileMenu) return;

    HMENU recent = nullptr;
    int count = ::GetMenuItemCount(fileMenu);
    for (int i = 0; i < count; ++i) {
        HMENU sub = ::GetSubMenu(fileMenu, i);
        if (sub) {
            UINT id0 = ::GetMenuItemID(sub, 0);
            if (id0 == IDM_FILE_RECENT_BASE ||
                id0 == static_cast<UINT>(-1) ||
                (id0 >= IDM_FILE_RECENT_BASE && id0 <= IDM_FILE_RECENT_MAX)) {
                recent = sub;
                break;
            }
        }
    }
    if (!recent) return;

    while (::GetMenuItemCount(recent) > 0)
        ::DeleteMenu(recent, 0, MF_BYPOSITION);

    const auto& mru = Parameters::Instance().RecentFiles();
    if (mru.empty()) {
        ::AppendMenuW(recent, MF_STRING | MF_GRAYED,
            IDM_FILE_RECENT_BASE, L"(empty)");
    } else {
        UINT id = IDM_FILE_RECENT_BASE;
        for (const auto& p : mru) {
            if (id > IDM_FILE_RECENT_MAX) break;
            std::wstring label = std::to_wstring(id - IDM_FILE_RECENT_BASE + 1);
            label += L": ";
            label += p;
            ::AppendMenuW(recent, MF_STRING, id++, label.c_str());
        }
        ::AppendMenuW(recent, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(recent, MF_STRING,
            IDM_FILE_RECENT_CLEAR, L"Clear Recent Files List");
    }
    ::DrawMenuBar(hwnd_);
}

void Notepad_plus_Window::BuildLanguageMenu()
{
    HMENU bar = ::GetMenu(hwnd_);
    if (!bar) return;
    int count = ::GetMenuItemCount(bar);
    HMENU langMenu = nullptr;
    for (int i = 0; i < count; ++i) {
        HMENU sub = ::GetSubMenu(bar, i);
        if (!sub) continue;
        UINT id0 = ::GetMenuItemID(sub, 0);
        if (id0 == IDM_LANG_BASE) { langMenu = sub; break; }
    }
    if (!langMenu) return;
    while (::GetMenuItemCount(langMenu) > 0)
        ::DeleteMenu(langMenu, 0, MF_BYPOSITION);

    // Sort languages by display name once for readable grouping.
    struct Entry { int idx; const wchar_t* name; };
    std::vector<Entry> langs;
    langs.reserve(static_cast<size_t>(LangType::Count_));
    for (int i = 0; i < static_cast<int>(LangType::Count_); ++i) {
        const wchar_t* nm = LangTypeName(static_cast<LangType>(i));
        if (nm && *nm) langs.push_back({ i, nm });
    }
    std::sort(langs.begin(), langs.end(),
        [](const Entry& a, const Entry& b) {
            return _wcsicmp(a.name, b.name) < 0;
        });

    // Pin "Normal Text" to the very top (most-used "reset" choice).
    auto txtIt = std::find_if(langs.begin(), langs.end(), [](const Entry& e){
        return e.idx == static_cast<int>(LangType::Text);
    });
    if (txtIt != langs.end()) {
        ::AppendMenuW(langMenu, MF_STRING,
            IDM_LANG_BASE + static_cast<UINT>(txtIt->idx), txtIt->name);
        ::AppendMenuW(langMenu, MF_SEPARATOR, 0, nullptr);
        langs.erase(txtIt);
    }

    // Bucket into balanced letter ranges so each submenu is a comfortable size.
    static const struct { wchar_t lo, hi; const wchar_t* label; } kBuckets[] = {
        { L'A', L'C', L"A - C" },
        { L'D', L'G', L"D - G" },
        { L'H', L'L', L"H - L" },
        { L'M', L'R', L"M - R" },
        { L'S', L'Z', L"S - Z" },
    };
    for (const auto& bk : kBuckets) {
        HMENU sub = ::CreatePopupMenu();
        int added = 0;
        for (const auto& e : langs) {
            wchar_t c = static_cast<wchar_t>(::towupper(e.name[0]));
            if (c >= bk.lo && c <= bk.hi) {
                ::AppendMenuW(sub, MF_STRING,
                    IDM_LANG_BASE + static_cast<UINT>(e.idx), e.name);
                ++added;
            }
        }
        if (added > 0) {
            ::AppendMenuW(langMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(sub), bk.label);
        } else {
            ::DestroyMenu(sub);
        }
    }
    ::DrawMenuBar(hwnd_);
}

void Notepad_plus_Window::UpdateCheckedMenus()
{
    HMENU bar = ::GetMenu(hwnd_);
    if (!bar) return;
    const Buffer* b = BufferManager::Instance().Get(app_.ActiveBuffer());
    if (!b) return;

    // Encoding group
    ::CheckMenuRadioItem(bar, IDM_ENC_UTF8, IDM_ENC_ANSI,
        IDM_ENC_UTF8 + static_cast<int>(b->GetEncoding()), MF_BYCOMMAND);
    // EOL group
    UINT eolCmd = IDM_EOL_CRLF + static_cast<int>(b->GetEol());
    ::CheckMenuRadioItem(bar, IDM_EOL_CRLF, IDM_EOL_CR, eolCmd, MF_BYCOMMAND);
    // Binary (hex) mode toggle.
    ::CheckMenuItem(bar, IDM_EDIT_BINARY_MODE,
        MF_BYCOMMAND | (app_.IsInBinaryMode(b->Id()) ? MF_CHECKED : MF_UNCHECKED));
    // Word-wrap toggle.
    ::CheckMenuItem(bar, IDM_VIEW_WORD_WRAP,
        MF_BYCOMMAND | (Parameters::Instance().WordWrap() ? MF_CHECKED : MF_UNCHECKED));
    // Language radio: clear all, check the active one.
    UINT firstLang = IDM_LANG_BASE;
    UINT lastLang  = IDM_LANG_BASE + static_cast<UINT>(LangType::Count_) - 1;
    UINT activeCmd = IDM_LANG_BASE + static_cast<UINT>(b->GetLang());
    ::CheckMenuRadioItem(bar, firstLang, lastLang, activeCmd, MF_BYCOMMAND);
    // Theme radio group.
    UINT themeCmd = IDM_VIEW_THEME_BASE +
        static_cast<UINT>(Parameters::Instance().Theme());
    ::CheckMenuRadioItem(bar, IDM_VIEW_THEME_BASE, IDM_VIEW_THEME_LAST,
        themeCmd, MF_BYCOMMAND);
}

void Notepad_plus_Window::ShowGoToLineDialog()
{
    wchar_t prompt[128];
    ::swprintf_s(prompt, L"Line (1..%lld):",
        static_cast<long long>(app_.Editor().Call(SCI_GETLINECOUNT)));
    wchar_t input[32] = L"";
    // Minimal input: use a dialog-indirect for simplicity.
    // Emulate a tiny prompt via a blocking message box substitute: use
    // a modeless edit in the status bar? Simpler — a plain MessageBox can't
    // take input, so roll a tiny dialog box inline here.
    struct Shared { wchar_t buf[32]; const wchar_t* prompt; };
    Shared sh{}; sh.prompt = prompt;
    auto proc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> INT_PTR {
        static Shared* shared = nullptr;
        if (m == WM_INITDIALOG) {
            shared = reinterpret_cast<Shared*>(l);
            ::SetDlgItemTextW(h, 100, shared->prompt);
            ::SetFocus(::GetDlgItem(h, 101));
            return FALSE;
        }
        if (m == WM_COMMAND) {
            if (LOWORD(w) == IDOK) {
                ::GetDlgItemTextW(h, 101, shared->buf, 32);
                ::EndDialog(h, IDOK);
                return TRUE;
            }
            if (LOWORD(w) == IDCANCEL) { ::EndDialog(h, IDCANCEL); return TRUE; }
        }
        return FALSE;
    };
    // Build an in-memory template for the tiny dialog.
    std::vector<BYTE> bytes;
    auto push = [&](const void* p, size_t n){
        const BYTE* b = reinterpret_cast<const BYTE*>(p);
        bytes.insert(bytes.end(), b, b + n);
    };
    auto align = [&](size_t n){ while (bytes.size() % n) bytes.push_back(0); };
    DLGTEMPLATE dt{};
    dt.style = WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_SETFONT|DS_MODALFRAME|DS_CENTER;
    dt.cdit = 3;
    dt.x=0; dt.y=0; dt.cx=160; dt.cy=60;
    push(&dt, sizeof(dt));
    WORD z = 0; push(&z, 2); push(&z, 2);
    const wchar_t* title = L"Go To Line"; for (const wchar_t* p=title; *p; ++p) push(p,2); push(&z,2);
    WORD fs = 9; push(&fs, 2);
    const wchar_t* fn = L"Segoe UI"; for (const wchar_t* p=fn; *p; ++p) push(p,2); push(&z,2);

    auto addItem = [&](DWORD st, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text){
        align(4);
        DLGITEMTEMPLATE it{};
        it.style = st | WS_CHILD | WS_VISIBLE;
        it.x=x; it.y=y; it.cx=cx; it.cy=cy; it.id=id;
        push(&it, sizeof(it));
        WORD a = 0xFFFF; push(&a, 2); push(&cls, 2);
        for (const wchar_t* p=text; *p; ++p) push(p,2); push(&z, 2);
        push(&z, 2);
    };
    addItem(SS_LEFT,                 8,  8,  140, 10, 100, 0x0082, L"");
    addItem(WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP, 8, 22, 140, 12, 101, 0x0081, L"");
    addItem(BS_DEFPUSHBUTTON|WS_TABSTOP, 96, 40, 52, 14, IDOK, 0x0080, L"OK");

    INT_PTR r = ::DialogBoxIndirectParamW(hInst_,
        reinterpret_cast<LPCDLGTEMPLATE>(bytes.data()),
        hwnd_, proc, reinterpret_cast<LPARAM>(&sh));
    if (r != IDOK) return;
    int line = _wtoi(sh.buf) - 1;
    if (line < 0) return;
    app_.Editor().Call(SCI_GOTOLINE, static_cast<uptr_t>(line));
    app_.Editor().SetFocus();
}

void Notepad_plus_Window::CopyPathToClipboard(BufferID id)
{
    const Buffer* b = BufferManager::Instance().Get(id);
    if (!b || b->IsUntitled()) return;
    const std::wstring& s = b->Path();
    if (!::OpenClipboard(hwnd_)) return;
    ::EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        if (auto* p = reinterpret_cast<wchar_t*>(::GlobalLock(mem))) {
            memcpy(p, s.c_str(), bytes);
            ::GlobalUnlock(mem);
            if (!::SetClipboardData(CF_UNICODETEXT, mem))
                ::GlobalFree(mem);
        } else {
            ::GlobalFree(mem);
        }
    }
    ::CloseClipboard();
}

void Notepad_plus_Window::OpenContainingFolder(BufferID id)
{
    const Buffer* b = BufferManager::Instance().Get(id);
    if (!b || b->IsUntitled()) return;
    std::wstring param = L"/select,\"" + b->Path() + L"\"";
    ::ShellExecuteW(hwnd_, L"open", L"explorer.exe",
        param.c_str(), nullptr, SW_SHOWNORMAL);
}

namespace {

// Tiny GDI helpers for drawing toolbar icons into a 32bpp DIB.
struct Px { uint8_t b, g, r, a; };

struct IconCanvas {
    int w = 16, h = 16;
    std::vector<Px> px;
    IconCanvas() {
        COLORREF bg = Icons().bg;
        Px fill{ GetBValue(bg), GetGValue(bg), GetRValue(bg), 0xFF };
        px.assign(16 * 16, fill);
    }
    void Dot(int x, int y, COLORREF c, uint8_t a = 255) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        px[y * w + x] = Px{ GetBValue(c), GetGValue(c), GetRValue(c), a };
    }
    void HLine(int x1, int x2, int y, COLORREF c) {
        if (x1 > x2) std::swap(x1, x2);
        for (int x = x1; x <= x2; ++x) Dot(x, y, c);
    }
    void VLine(int x, int y1, int y2, COLORREF c) {
        if (y1 > y2) std::swap(y1, y2);
        for (int y = y1; y <= y2; ++y) Dot(x, y, c);
    }
    void Rect(int x1, int y1, int x2, int y2, COLORREF c) {
        HLine(x1, x2, y1, c); HLine(x1, x2, y2, c);
        VLine(x1, y1, y2, c); VLine(x2, y1, y2, c);
    }
    void Fill(int x1, int y1, int x2, int y2, COLORREF c) {
        for (int y = y1; y <= y2; ++y)
            for (int x = x1; x <= x2; ++x) Dot(x, y, c);
    }
};

HBITMAP CanvasToBitmap(const IconCanvas& c)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = c.w;
    bi.bmiHeader.biHeight      = -c.h;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = ::GetDC(nullptr);
    HBITMAP bmp = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, dc);
    if (bmp && bits) {
        std::memcpy(bits, c.px.data(), c.px.size() * sizeof(Px));
    }
    return bmp;
}

// Each drawer renders one 16×16 icon into `c`.
// Flat outline 16×16 toolbar icons. Ink/accent/fill resolve from the active
// theme's IconPalette, so icons always blend with the surrounding toolbar.
namespace clr {
    inline COLORREF kInk()    { return Icons().ink; }
    inline COLORREF kAccent() { return Icons().accent; }
    inline COLORREF kAccDk()  { return Icons().accentDk; }
    inline COLORREF kMute()   { return Icons().mute; }
    inline COLORREF kFill()   { return Icons().fill; }
    inline COLORREF kRed()    { return Icons().red; }
}

// All icons follow the same convention: ink outlines in kInk, accent
// strokes/badges in kAccent, white/transparent interior — matches the
// neutral-gray + accent-blue look of the rest of the UI.

void DrawPageOutline(IconCanvas& c)
{
    c.Fill(3, 2, 11, 13, clr::kFill());
    c.Rect(3, 2, 11, 13, clr::kInk());
    c.HLine(9, 11, 4, clr::kInk());
    c.VLine(11, 2, 4, clr::kInk());
    c.Dot(10, 3, clr::kInk()); c.Dot(9, 3, clr::kInk());
    c.HLine(5, 9, 7,  clr::kMute());
    c.HLine(5, 9, 9,  clr::kMute());
    c.HLine(5, 10, 11, clr::kMute());
}
void DrawNew(IconCanvas& c) {
    DrawPageOutline(c);
    // Accent plus badge, bottom-right.
    c.Fill(9, 9, 14, 14, clr::kAccent());
    c.HLine(10, 12, 11, clr::kFill());
    c.HLine(10, 12, 12, clr::kFill());
    c.VLine(11, 10, 13, clr::kFill());
    c.VLine(12, 10, 13, clr::kFill());
}
void DrawOpen(IconCanvas& c) {
    // Outline folder; accent only on the front face top edge.
    c.Rect(1, 5, 7, 7, clr::kInk());
    c.Rect(1, 6, 14, 13, clr::kInk());
    c.Fill(2, 7, 13, 12, clr::kFill());
    c.HLine(1, 14, 6, clr::kAccent());
}
void DrawSave(IconCanvas& c) {
    // Outline floppy with accent shutter.
    c.Rect(2, 2, 13, 13, clr::kInk());
    c.Fill(3, 3, 12, 12, clr::kFill());
    // Shutter strip top in accent.
    c.Fill(4, 3, 11, 6, clr::kAccent());
    c.Fill(9, 4, 10, 5, clr::kFill());
    // Two lines on the label.
    c.HLine(4, 11, 9,  clr::kMute());
    c.HLine(4, 11, 11, clr::kMute());
}
void DrawSaveAll(IconCanvas& c) {
    // Back floppy outline.
    c.Rect(5, 5, 14, 14, clr::kInk()); c.Fill(6, 6, 13, 13, clr::kFill());
    c.Fill(8, 6, 12, 8, clr::kAccent());
    // Front floppy.
    c.Rect(1, 1, 10, 10, clr::kInk()); c.Fill(2, 2, 9, 9, clr::kFill());
    c.Fill(3, 2, 8, 4, clr::kAccent());
    c.HLine(3, 7, 7, clr::kMute());
}
void DrawClose(IconCanvas& c) {
    DrawPageOutline(c);
    for (int i = 0; i < 5; ++i) {
        c.Dot(5+i, 5+i, clr::kRed()); c.Dot(6+i, 5+i, clr::kRed());
        c.Dot(9-i, 5+i, clr::kRed()); c.Dot(10-i, 5+i, clr::kRed());
    }
}
void DrawCloseAll(IconCanvas& c) {
    // Back page suggestion.
    c.Rect(1, 0, 7, 4, clr::kInk());
    DrawPageOutline(c);
    for (int i = 0; i < 5; ++i) {
        c.Dot(5+i, 7+i, clr::kRed()); c.Dot(6+i, 7+i, clr::kRed());
        c.Dot(9-i, 7+i, clr::kRed()); c.Dot(10-i, 7+i, clr::kRed());
    }
}
void DrawCut(IconCanvas& c) {
    // Two crossing blades in ink.
    for (int i = 0; i < 7; ++i) {
        c.Dot(3 + i, 2 + i, clr::kInk()); c.Dot(4 + i, 2 + i, clr::kInk());
        c.Dot(12 - i, 2 + i, clr::kInk()); c.Dot(11 - i, 2 + i, clr::kInk());
    }
    // Two ring handles in accent (outline only).
    c.Rect(1, 10, 6, 15, clr::kAccent()); c.Rect(2, 11, 5, 14, clr::kAccent());
    c.Rect(9, 10, 14, 15, clr::kAccent()); c.Rect(10, 11, 13, 14, clr::kAccent());
}
void DrawCopy(IconCanvas& c) {
    // Two outline pages, front white, back lightly accented.
    c.Rect(5, 4, 13, 14, clr::kInk());
    c.Fill(6, 5, 12, 13, clr::kFill());
    c.Rect(2, 1, 10, 11, clr::kInk());
    c.Fill(3, 2, 9, 10, clr::kFill());
    c.HLine(4, 8, 4, clr::kAccent());
    c.HLine(4, 8, 6, clr::kMute());
    c.HLine(4, 8, 8, clr::kMute());
}
void DrawPaste(IconCanvas& c) {
    // Clipboard outline; accent clip at top.
    c.Rect(2, 3, 13, 14, clr::kInk());
    c.Fill(3, 4, 12, 13, clr::kFill());
    c.Fill(6, 1, 9, 4, clr::kAccent());
    c.Rect(6, 1, 9, 4, clr::kInk());
    c.HLine(5, 10, 7, clr::kMute());
    c.HLine(5, 10, 9, clr::kMute());
    c.HLine(5, 10, 11, clr::kMute());
}
void DrawArrow(IconCanvas& c, bool right)
{
    // 3/4 ring in accent.
    for (int t = 200; t < 360; t += 6) {
        double rad = t * 3.14159 / 180;
        int x = 8 + (int)(5.0 * std::cos(rad));
        int y = 8 + (int)(5.0 * std::sin(rad));
        c.Dot(x, y, clr::kAccent()); c.Dot(x, y + 1, clr::kAccent());
    }
    int hx = right ? 12 : 4;
    c.Fill(hx - 1, 6, hx + 1, 8, clr::kAccent());
    c.Dot(hx - 2, 7, clr::kAccent()); c.Dot(hx + 2, 7, clr::kAccent());
    c.Dot(hx - 1, 5, clr::kAccent()); c.Dot(hx + 1, 5, clr::kAccent());
    c.Dot(hx - 1, 9, clr::kAccent()); c.Dot(hx + 1, 9, clr::kAccent());
}
void DrawUndo(IconCanvas& c) { DrawArrow(c, false); }
void DrawRedo(IconCanvas& c) { DrawArrow(c, true); }
void DrawFind(IconCanvas& c) {
    int cx = 6, cy = 6, r = 4;
    // Lens rim only — outline.
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x) {
            int d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d >= (r - 1) * (r - 1) && d <= r * r) c.Dot(x, y, clr::kInk());
        }
    // Handle in accent.
    for (int i = 0; i < 5; ++i) {
        c.Dot(10 + i, 10 + i, clr::kAccent());
        c.Dot(11 + i, 10 + i, clr::kAccent());
        c.Dot(10 + i, 11 + i, clr::kAccent());
    }
}
void DrawReplace(IconCanvas& c) {
    DrawFind(c);
    // Small accent arrow inside lens.
    c.HLine(4, 7, 6, clr::kAccent());
    c.Dot(7, 5, clr::kAccent()); c.Dot(8, 6, clr::kAccent()); c.Dot(7, 7, clr::kAccent());
}
void DrawFindFiles(IconCanvas& c) {
    // Outline folder behind, then magnifier on top.
    c.Rect(0, 7, 10, 14, clr::kInk());
    c.Fill(1, 8, 9, 13, clr::kFill());
    c.HLine(0, 5, 6, clr::kInk());
    DrawFind(c);
}
void DrawBookmark(IconCanvas& c) {
    // Outline ribbon, accent fill.
    c.Rect(4, 1, 11, 13, clr::kInk());
    c.Fill(5, 2, 10, 12, clr::kAccent());
    // V notch.
    c.Dot(7, 13, 0); c.Dot(8, 13, 0);
    for (int y = 10; y <= 12; ++y) {
        for (int x = 5 + (12 - y); x <= 10 - (12 - y); ++x) {
            if (x >= 5 + (12 - y) && x <= 10 - (12 - y))
                c.Dot(x, y, clr::kFill());
        }
    }
}
void DrawSplit(IconCanvas& c) {
    c.Rect(1, 2, 14, 13, clr::kInk());
    c.Fill(2, 3, 13, 12, clr::kFill());
    c.VLine(7, 2, 13, clr::kInk()); c.VLine(8, 2, 13, clr::kInk());
    // Accent stripe across both panes top.
    c.HLine(2, 6, 3, clr::kAccent());
    c.HLine(9, 13, 3, clr::kAccent());
}
void DrawBinary(IconCanvas& c) {
    // "10" in accent — two simple glyphs.
    c.VLine(3, 2, 7, clr::kInk()); c.VLine(4, 2, 7, clr::kInk());
    c.Dot(2, 3, clr::kInk());
    c.Rect(6, 2, 9, 7, clr::kInk());
    c.Rect(2, 8, 5, 13, clr::kInk());
    c.VLine(8, 8, 13, clr::kAccent()); c.VLine(9, 8, 13, clr::kAccent());
    c.Dot(7, 9, clr::kAccent());
}

using Drawer = void(*)(IconCanvas&);

} // namespace

void Notepad_plus_Window::CreateToolbar()
{
    ::InitCommonControls();
    toolbar_ = ::CreateWindowExW(
        0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
        TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NODIVIDER | CCS_NORESIZE,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(0x900)),
        hInst_, nullptr);
    if (!toolbar_) return;

    ::SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE,
        static_cast<WPARAM>(sizeof(TBBUTTON)), 0);
    ::SendMessageW(toolbar_, TB_SETEXTENDEDSTYLE, 0,
        TBSTYLE_EX_DRAWDDARROWS | TBSTYLE_EX_DOUBLEBUFFER);
    ::SendMessageW(toolbar_, TB_SETBITMAPSIZE, 0, MAKELPARAM(16, 16));
    ::SendMessageW(toolbar_, TB_SETPADDING, 0, MAKELPARAM(6, 6));

    struct Entry { Drawer draw; int cmd; const wchar_t* tip; };
    Entry items[] = {
        { DrawNew,       IDM_FILE_NEW,            L"New" },
        { DrawOpen,      IDM_FILE_OPEN,           L"Open" },
        { DrawSave,      IDM_FILE_SAVE,           L"Save" },
        { DrawSaveAll,   IDM_FILE_SAVEALL,        L"Save All" },
        { DrawClose,     IDM_FILE_CLOSE,          L"Close" },
        { DrawCloseAll,  IDM_FILE_CLOSEALL,       L"Close All" },
        { nullptr,       0,                       nullptr },   // separator
        { DrawCut,       IDM_EDIT_CUT,            L"Cut" },
        { DrawCopy,      IDM_EDIT_COPY,           L"Copy" },
        { DrawPaste,     IDM_EDIT_PASTE,          L"Paste" },
        { DrawUndo,      IDM_EDIT_UNDO,           L"Undo" },
        { DrawRedo,      IDM_EDIT_REDO,           L"Redo" },
        { nullptr,       0,                       nullptr },
        { DrawFind,      IDM_SEARCH_FIND,         L"Find" },
        { DrawReplace,   IDM_SEARCH_REPLACE,      L"Replace" },
        { DrawFindFiles, IDM_SEARCH_FINDFILES,    L"Find in Files" },
        { nullptr,       0,                       nullptr },
        { DrawBookmark,  IDM_SEARCH_BMK_TOGGLE,   L"Toggle Bookmark" },
        { nullptr,       0,                       nullptr },
        { DrawBinary,    IDM_EDIT_BINARY_MODE,    L"Binary Mode" },
    };

    // Build the image list from drawers.
    HIMAGELIST il = ImageList_Create(16, 16, ILC_COLOR32, 0, 32);
    int imgIndex = 0;
    std::vector<TBBUTTON> btns;
    btns.reserve(ARRAYSIZE(items));
    for (const Entry& e : items) {
        if (!e.draw) {
            TBBUTTON b{};
            b.iBitmap = 0;
            b.idCommand = 0;
            b.fsState   = TBSTATE_ENABLED;
            b.fsStyle   = BTNS_SEP;
            btns.push_back(b);
            continue;
        }
        IconCanvas canvas;
        e.draw(canvas);
        HBITMAP bmp = CanvasToBitmap(canvas);
        int idx = ImageList_Add(il, bmp, nullptr);
        ::DeleteObject(bmp);
        TBBUTTON b{};
        b.iBitmap   = idx;
        b.idCommand = e.cmd;
        b.fsState   = TBSTATE_ENABLED;
        b.fsStyle   = BTNS_BUTTON;     // no text under glyph
        b.iString   = 0;
        btns.push_back(b);
        if (e.tip) g_tbTips[e.cmd] = e.tip;
        ++imgIndex;
    }
    ::SendMessageW(toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(il));
    tbImgs_ = il;
    ::SendMessageW(toolbar_, TB_ADDBUTTONS,
        static_cast<WPARAM>(btns.size()),
        reinterpret_cast<LPARAM>(btns.data()));
    ::SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
}

void Notepad_plus_Window::RebuildToolbar()
{
    if (!toolbar_) return;
    // Destroy old image list
    if (tbImgs_) {
        ImageList_Destroy(reinterpret_cast<HIMAGELIST>(tbImgs_));
        tbImgs_ = nullptr;
    }
    // Remove all buttons
    while (::SendMessageW(toolbar_, TB_DELETEBUTTON, 0, 0)) {}
    g_tbTips.clear();

    // Recreate (CreateToolbar reuses toolbar_ HWND pieces we need to rebuild)
    // We just rebuild the image list and buttons in place.
    // Call the same logic as CreateToolbar but reuse the existing HWND.
    ::SendMessageW(toolbar_, TB_SETBITMAPSIZE, 0, MAKELPARAM(16, 16));

    struct Entry { Drawer draw; int cmd; const wchar_t* tip; };
    Entry items[] = {
        { DrawNew,       IDM_FILE_NEW,            L"New" },
        { DrawOpen,      IDM_FILE_OPEN,           L"Open" },
        { DrawSave,      IDM_FILE_SAVE,           L"Save" },
        { DrawSaveAll,   IDM_FILE_SAVEALL,        L"Save All" },
        { DrawClose,     IDM_FILE_CLOSE,          L"Close" },
        { DrawCloseAll,  IDM_FILE_CLOSEALL,       L"Close All" },
        { nullptr,       0,                       nullptr },
        { DrawCut,       IDM_EDIT_CUT,            L"Cut" },
        { DrawCopy,      IDM_EDIT_COPY,           L"Copy" },
        { DrawPaste,     IDM_EDIT_PASTE,          L"Paste" },
        { DrawUndo,      IDM_EDIT_UNDO,           L"Undo" },
        { DrawRedo,      IDM_EDIT_REDO,           L"Redo" },
        { nullptr,       0,                       nullptr },
        { DrawFind,      IDM_SEARCH_FIND,         L"Find" },
        { DrawReplace,   IDM_SEARCH_REPLACE,      L"Replace" },
        { DrawFindFiles, IDM_SEARCH_FINDFILES,    L"Find in Files" },
        { nullptr,       0,                       nullptr },
        { DrawBookmark,  IDM_SEARCH_BMK_TOGGLE,   L"Toggle Bookmark" },
        { nullptr,       0,                       nullptr },
        { DrawBinary,    IDM_EDIT_BINARY_MODE,    L"Binary Mode" },
    };

    HIMAGELIST il = ImageList_Create(16, 16, ILC_COLOR32, 0, 32);
    std::vector<TBBUTTON> btns;
    btns.reserve(ARRAYSIZE(items));
    for (const Entry& e : items) {
        if (!e.draw) {
            TBBUTTON b{};
            b.iBitmap = 0; b.idCommand = 0;
            b.fsState = TBSTATE_ENABLED; b.fsStyle = BTNS_SEP;
            btns.push_back(b);
            continue;
        }
        IconCanvas canvas;
        e.draw(canvas);
        HBITMAP bmp = CanvasToBitmap(canvas);
        int idx = ImageList_Add(il, bmp, nullptr);
        ::DeleteObject(bmp);
        TBBUTTON b{};
        b.iBitmap = idx; b.idCommand = e.cmd;
        b.fsState = TBSTATE_ENABLED; b.fsStyle = BTNS_BUTTON;
        btns.push_back(b);
        if (e.tip) g_tbTips[e.cmd] = e.tip;
    }
    ::SendMessageW(toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(il));
    tbImgs_ = il;
    ::SendMessageW(toolbar_, TB_ADDBUTTONS,
        static_cast<WPARAM>(btns.size()),
        reinterpret_cast<LPARAM>(btns.data()));
    ::SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
    ::InvalidateRect(toolbar_, nullptr, TRUE);
}

void Notepad_plus_Window::ApplyCurrentTheme()
{
    ApplyFrameTheme(hwnd_, statusBar_, toolbar_);
    RebuildToolbar();

    for (int v = 0; v < 2; ++v) {
        if (!app_.V(v).editor.Hwnd()) continue;
        BufferID bid = app_.V(v).activeId;
        if (bid == kInvalidBufferID) continue;
        const Buffer* buf = BufferManager::Instance().Get(bid);
        if (buf) ApplyLanguage(app_.V(v).editor, buf->GetLang());
    }
    for (int v = 0; v < 2; ++v) {
        if (app_.V(v).tabs.Hwnd())
            ::InvalidateRect(app_.V(v).tabs.Hwnd(), nullptr, TRUE);
    }

    UpdateCheckedMenus();
    OnSize(hwnd_);
}

void Notepad_plus_Window::SetTheme(ThemeId t)
{
    Parameters& p = Parameters::Instance();
    if (p.Theme() == t) return;
    p.SetTheme(t);
    p.Save();
    ApplyCurrentTheme();
}

void Notepad_plus_Window::CycleTheme()
{
    Parameters& p = Parameters::Instance();
    int next = (static_cast<int>(p.Theme()) + 1) % static_cast<int>(ThemeId::Count_);
    SetTheme(static_cast<ThemeId>(next));
}

LRESULT Notepad_plus_Window::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        OnCreate(h);
        return 0;

    case WM_SIZE:
        OnSize(h);
        return 0;

    case WM_SETFOCUS:
        app_.Editor().SetFocus();
        return 0;

    case WM_TIMER:
        if (w == kStyleDebounceTimerId) {
            ::KillTimer(h, kStyleDebounceTimerId);
            FlushPendingStyle();
            return 0;
        }
        break;

    case kMsgUiRefresh:
        // Active buffer changed via a path that bypasses this WndProc
        // (tab ✕ close, split-view tab drop) — refresh frame chrome.
        UpdateCheckedMenus();
        app_.UpdateTitle(h);
        app_.UpdateStatusBar(statusBar_);
        return 0;

    case kMsgFindInFilesDone:
        // Worker thread finished a Find-in-Files scan; payload ownership
        // passes to DeliverFindInFiles.
        app_.DeliverFindInFiles(reinterpret_cast<void*>(l));
        return 0;

    case WM_ERASEBKGND: {
        const UiPalette& u = Ui();
        HDC hdc = reinterpret_cast<HDC>(w);
        RECT rc; ::GetClientRect(h, &rc);
        if (!g_chromeBrush) g_chromeBrush = ::CreateSolidBrush(u.chromeBg);
        ::FillRect(hdc, &rc, g_chromeBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HWND ctrl = reinterpret_cast<HWND>(l);
        if (ctrl == statusBar_) {
            const UiPalette& u = Ui();
            HDC hdc = reinterpret_cast<HDC>(w);
            ::SetTextColor(hdc, u.text);
            ::SetBkColor(hdc, u.statusBg);
            if (!g_chromeBrush) g_chromeBrush = ::CreateSolidBrush(u.chromeBg);
            return reinterpret_cast<LRESULT>(g_chromeBrush);
        }
        break;
    }

    case WM_DROPFILES: {
        HDROP hdrop = reinterpret_cast<HDROP>(w);
        const UINT n = ::DragQueryFileW(hdrop, 0xFFFFFFFFu, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH * 2] = L"";
            if (::DragQueryFileW(hdrop, i, path, ARRAYSIZE(path))) {
                DWORD attr = ::GetFileAttributesW(path);
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
                    app_.DoOpen(std::wstring(path));
            }
        }
        ::DragFinish(hdrop);
        RebuildRecentMenu();
        return 0;
    }

    case WM_COPYDATA: {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(l);
        if (!cds || cds->dwData != kOpenFilesMsgId || !cds->lpData || cds->cbData < sizeof(wchar_t))
            break;
        // Payload is a wchar_t buffer bounded by cbData; paths are '\n'-separated.
        const wchar_t* p    = static_cast<const wchar_t*>(cds->lpData);
        const size_t   wlen = cds->cbData / sizeof(wchar_t);
        std::wstring all(p, wlen);
        size_t start = 0;
        while (start <= all.size()) {
            size_t nl = all.find(L'\n', start);
            std::wstring one = all.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
            if (!one.empty() && one.back() == L'\0') one.pop_back();
            if (!one.empty()) OpenFromCommandLine(one);
            if (nl == std::wstring::npos) break;
            start = nl + 1;
        }
        // Bring our window to the foreground; the launching process transferred
        // foreground rights via AllowSetForegroundWindow before sending.
        if (::IsIconic(h)) ::ShowWindow(h, SW_RESTORE);
        ::SetForegroundWindow(h);
        return TRUE;
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(l);
        if (dis && (dis->CtlID == 5001 || dis->CtlID == 5002)) {
            int v = (dis->CtlID == 5001) ? 0 : 1;
            app_.V(v).tabs.HandleDrawItem(dis);
            return TRUE;
        }
        if (dis && dis->hwndItem == statusBar_) {
            const UiPalette& u = Ui();
            HBRUSH bg = ::CreateSolidBrush(u.statusBg);
            ::FillRect(dis->hDC, &dis->rcItem, bg);
            ::DeleteObject(bg);
            const wchar_t* text = reinterpret_cast<const wchar_t*>(dis->itemData);
            if (text && *text) {
                ::SetBkMode(dis->hDC, TRANSPARENT);
                ::SetTextColor(dis->hDC, u.text);
                RECT tr = dis->rcItem;
                tr.left += 6;
                ::DrawTextW(dis->hDC, text, -1, &tr,
                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(l);
        // Toolbar tooltip text — TTN_GETDISPINFOW arrives from the
        // toolbar's auto-created tooltip control with idFrom = command id.
        if (nmhdr && (nmhdr->code == TTN_GETDISPINFOW ||
                      nmhdr->code == static_cast<UINT>(-530)) /* TTN_NEEDTEXTW */) {
            auto* nm = reinterpret_cast<NMTTDISPINFOW*>(nmhdr);
            int cmd = static_cast<int>(nmhdr->idFrom);
            auto it = g_tbTips.find(cmd);
            if (it != g_tbTips.end()) {
                nm->lpszText = const_cast<wchar_t*>(it->second.c_str());
                nm->hinst    = nullptr;
                return 0;
            }
        }
        for (int v = 0; v < 2; ++v) {
            if (nmhdr && nmhdr->hwndFrom == app_.V(v).tabs.Hwnd()) {
                app_.SetActiveView(v);
                app_.V(v).tabs.HandleNotify(nmhdr);
                UpdateCheckedMenus();
                app_.UpdateTitle(h);
                app_.UpdateStatusBar(statusBar_);
                return 0;
            }
        }
        int editorView = -1;
        for (int v = 0; v < 2; ++v) {
            if (nmhdr && app_.V(v).editor.Hwnd() && nmhdr->hwndFrom == app_.V(v).editor.Hwnd()) {
                editorView = v; break;
            }
        }
        if (editorView >= 0) {
            // Ignore notifications generated while a foreign document is
            // temporarily attached (BufferManager load/save via the factory
            // view). Attributing those savepoint/modify events to the active
            // buffer silently cleared its dirty flag — and a clean-looking
            // dirty buffer closes without a save prompt.
            if (Buffer* ab = BufferManager::Instance().Get(
                    app_.V(editorView).activeId)) {
                if (app_.V(editorView).editor.Call(SCI_GETDOCPOINTER) !=
                        ab->DocHandle())
                    return 0;
            }
            auto* scn = reinterpret_cast<SCNotification*>(l);
            switch (scn->nmhdr.code) {
            case SCN_SAVEPOINTREACHED:
                if (Buffer* b = BufferManager::Instance().Get(app_.ActiveBuffer())) {
                    b->SetDirty(false);
                    app_.Tabs().SetLabel(b->Id(), b->DisplayName(), false);
                }
                app_.UpdateTitle(h);
                app_.UpdateStatusBar(statusBar_);
                break;
            case SCN_SAVEPOINTLEFT:
                if (Buffer* b = BufferManager::Instance().Get(app_.ActiveBuffer())) {
                    b->SetDirty(true);
                    app_.Tabs().SetLabel(b->Id(), b->DisplayName(), true);
                }
                app_.UpdateTitle(h);
                app_.UpdateStatusBar(statusBar_);
                break;
            case SCN_MARGINCLICK:
                if (scn->margin == 1) {
                    sptr_t line = app_.Editor().Call(SCI_LINEFROMPOSITION,
                        static_cast<uptr_t>(scn->position));
                    sptr_t state = app_.Editor().Call(SCI_MARKERGET,
                        static_cast<uptr_t>(line));
                    if (state & (1 << 24))
                        app_.Editor().Call(SCI_MARKERDELETE,
                            static_cast<uptr_t>(line), 24);
                    else
                        app_.Editor().Call(SCI_MARKERADD,
                            static_cast<uptr_t>(line), 24);
                }
                break;
            case SCN_UPDATEUI:
                if (scn->updated &
                    (SC_UPDATE_SELECTION | SC_UPDATE_CONTENT | SC_UPDATE_V_SCROLL)) {
                    app_.UpdateStatusBar(statusBar_);
                }
                if (app_.Dock().IsShown(DockSide::Right) &&
                    app_.Dock().Panel(DockSide::Right) == &app_.DocMapPane()) {
                    app_.RefreshDocMapViewport();
                }
                // V_SCROLL included so viewport-scoped smart highlight covers
                // occurrences that scroll into view.
                if (scn->updated & (SC_UPDATE_SELECTION | SC_UPDATE_V_SCROLL)) {
                    ScintillaEditView& ved = app_.V(editorView).editor;
                    if (app_.IsInBinaryMode(app_.V(editorView).activeId)) {
                        if (scn->updated & SC_UPDATE_SELECTION)
                            RefreshBinaryLink(ved);
                    } else {
                        RefreshSmartHighlight(ved);
                    }
                }
                if ((scn->updated & SC_UPDATE_CONTENT) &&
                    app_.IsInBinaryMode(app_.V(editorView).activeId))
                {
                    ScintillaEditView& ved = app_.V(editorView).editor;
                    sptr_t cur = ved.Call(SCI_GETCURRENTPOS);
                    sptr_t ln  = ved.Call(SCI_LINEFROMPOSITION,
                        static_cast<uptr_t>(cur));
                    app_.SyncBinaryGutter(ved, ln);
                }
                break;
            case SCN_DOUBLECLICK:
                RefreshSmartHighlight(app_.V(editorView).editor);
                break;
            case SCN_MODIFIED:
                // Re-style only on user-initiated edits / undo / redo. File loads
                // and other programmatic SCI_SETTEXT carry none of these perf
                // flags, so the heavy whole-buffer pass is skipped during open
                // (ApplyLanguage runs it once at end of load).
                if ((scn->modificationType &
                        (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) &&
                    (scn->modificationType &
                        (SC_PERFORMED_USER | SC_PERFORMED_UNDO | SC_PERFORMED_REDO)))
                {
                    // Defer the whole-document identifier / fence pass — running
                    // it synchronously on every keystroke makes typing sluggish
                    // on large files. Timer fires once typing pauses.
                    if (Buffer* mb = BufferManager::Instance().Get(
                            app_.V(editorView).activeId)) {
                        mb->SetStyleDirty(true);
                    }
                    ::SetTimer(h, kStyleDebounceTimerId, kStyleDebounceMs, nullptr);
                }
                if ((scn->modificationType &
                        (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) &&
                    app_.IsInBinaryMode(app_.V(editorView).activeId))
                {
                    ScintillaEditView& ved = app_.V(editorView).editor;
                    sptr_t startLine = ved.Call(SCI_LINEFROMPOSITION,
                        static_cast<uptr_t>(scn->position));
                    sptr_t endLine = startLine;
                    if (scn->linesAdded == 0 && scn->length > 0) {
                        endLine = ved.Call(SCI_LINEFROMPOSITION,
                            static_cast<uptr_t>(scn->position + scn->length));
                    }
                    for (sptr_t ln = startLine; ln <= endLine; ++ln)
                        app_.SyncBinaryGutter(ved, ln);
                }
                break;
            case SCN_URIDROPPED:
                if (scn->text) {
                    const char* s   = scn->text;
                    const char* end = s + strlen(s);
                    while (s < end) {
                        const char* nl = s;
                        while (nl < end && *nl != '\n' && *nl != '\r') ++nl;
                        if (nl > s) {
                            std::wstring path = DecodeFileUri(s, nl - s);
                            if (!path.empty()) {
                                DWORD attr = ::GetFileAttributesW(path.c_str());
                                if (attr != INVALID_FILE_ATTRIBUTES &&
                                    !(attr & FILE_ATTRIBUTE_DIRECTORY))
                                    app_.DoOpen(path);
                            }
                        }
                        s = nl;
                        while (s < end && (*s == '\n' || *s == '\r')) ++s;
                    }
                    RebuildRecentMenu();
                    app_.UpdateTitle(h);
                    app_.UpdateStatusBar(statusBar_);
                }
                break;
            }
            return 0;
        }
        // Panels (find results list / trees).
        return app_.RouteNotify(l);
    }

    case WM_COMMAND: {
        const UINT id = LOWORD(w);
        if (id >= IDM_LANG_BASE && id <= IDM_LANG_MAX) {
            int langIdx = static_cast<int>(id - IDM_LANG_BASE);
            if (langIdx >= 0 && langIdx < static_cast<int>(LangType::Count_)) {
                app_.ChangeLanguage(static_cast<LangType>(langIdx));
            }
            break;
        }
        if (id >= IDM_FILE_RECENT_BASE && id <= IDM_FILE_RECENT_MAX) {
            const auto& mru = Parameters::Instance().RecentFiles();
            size_t idx = id - IDM_FILE_RECENT_BASE;
            if (idx < mru.size()) {
                app_.DoOpen(mru[idx]);
                RebuildRecentMenu();
            }
            break;
        }
        switch (id) {
        case IDM_FILE_NEW:       app_.DoNew(); break;
        case IDM_FILE_OPEN:      app_.DoOpenDialog(h); RebuildRecentMenu(); break;
        case IDM_FILE_SAVE:      app_.DoSave(h, app_.ActiveBuffer()); RebuildRecentMenu(); break;
        case IDM_FILE_SAVEAS:    app_.DoSaveAs(h, app_.ActiveBuffer()); RebuildRecentMenu(); break;
        case IDM_FILE_SAVEALL:   app_.DoSaveAll(h); RebuildRecentMenu(); break;
        case IDM_FILE_CLOSE: {
            BufferID target = (ctxMenuBuffer_ != kInvalidBufferID)
                ? ctxMenuBuffer_ : app_.ActiveBuffer();
            app_.DoClose(h, target);
            ctxMenuBuffer_ = kInvalidBufferID;
            break;
        }
        case IDM_FILE_CLOSEALL:      app_.DoCloseAll(h); break;
        case IDM_FILE_CLOSEBUT:
            app_.DoCloseAllBut(h, app_.ActiveBuffer()); break;
        case IDM_FILE_CLOSETOLEFT:
            app_.DoCloseToLeft(h, app_.ActiveBuffer()); break;
        case IDM_FILE_CLOSETORIGHT:
            app_.DoCloseToRight(h, app_.ActiveBuffer()); break;
        case IDM_TAB_COPYPATH:
            CopyPathToClipboard(ctxMenuBuffer_ != kInvalidBufferID
                ? ctxMenuBuffer_ : app_.ActiveBuffer());
            ctxMenuBuffer_ = kInvalidBufferID;
            break;
        case IDM_TAB_OPENFOLDER:
            OpenContainingFolder(ctxMenuBuffer_ != kInvalidBufferID
                ? ctxMenuBuffer_ : app_.ActiveBuffer());
            ctxMenuBuffer_ = kInvalidBufferID;
            break;
        case IDM_WIN_NEXT: {
            int n = app_.Tabs().TabCount();
            if (n > 1) {
                int cur = app_.Tabs().IndexOf(app_.ActiveBuffer());
                int nxt = (cur + 1) % n;
                app_.Tabs().Activate(app_.Tabs().BufferAt(nxt));
            }
            break;
        }
        case IDM_WIN_PREV: {
            int n = app_.Tabs().TabCount();
            if (n > 1) {
                int cur = app_.Tabs().IndexOf(app_.ActiveBuffer());
                int nxt = (cur - 1 + n) % n;
                app_.Tabs().Activate(app_.Tabs().BufferAt(nxt));
            }
            break;
        }
        case IDM_FILE_RECENT_CLEAR:
            Parameters::Instance().ClearRecent();
            Parameters::Instance().Save();
            RebuildRecentMenu();
            break;
        case IDM_FILE_EXIT:
            ::SendMessageW(h, WM_CLOSE, 0, 0);
            break;
        case IDM_HELP_ABOUT:
            ::MessageBoxW(h,
                L"NotePad-L 0.6\n"
                L"A Scintilla-based multi-tab editor.\n"
                L"Created by Claude Code (Anthropic Claude Opus 4.6).",
                L"About NotePad-L", MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_SEARCH_FIND:     app_.ShowFind(h, hInst_); break;
        case IDM_SEARCH_REPLACE:  app_.ShowReplace(h, hInst_); break;
        case IDM_SEARCH_FINDNEXT: app_.FindNextRepeat(); break;
        case IDM_SEARCH_FINDPREV: app_.FindPrevRepeat(); break;
        case IDM_SEARCH_GOTO:     ShowGoToLineDialog(); break;
        case IDM_SEARCH_BMK_TOGGLE: app_.ToggleBookmark(); break;
        case IDM_SEARCH_BMK_NEXT:   app_.NextBookmark();   break;
        case IDM_SEARCH_BMK_PREV:   app_.PrevBookmark();   break;
        case IDM_SEARCH_BMK_CLEAR:  app_.ClearAllBookmarks(); break;

        case IDM_ENC_UTF8:        app_.ChangeEncoding(Buffer::Encoding::Utf8);       break;
        case IDM_ENC_UTF8_BOM:    app_.ChangeEncoding(Buffer::Encoding::Utf8Bom);    break;
        case IDM_ENC_UTF16LE_BOM: app_.ChangeEncoding(Buffer::Encoding::Utf16LeBom); break;
        case IDM_ENC_UTF16BE_BOM: app_.ChangeEncoding(Buffer::Encoding::Utf16BeBom); break;
        case IDM_ENC_ANSI:        app_.ChangeEncoding(Buffer::Encoding::Ansi);       break;
        case IDM_ENC_CONVERT_UTF8:     app_.ConvertEncoding(Buffer::Encoding::Utf8);       break;
        case IDM_ENC_CONVERT_UTF8_BOM: app_.ConvertEncoding(Buffer::Encoding::Utf8Bom);    break;
        case IDM_ENC_CONVERT_UTF16LE:  app_.ConvertEncoding(Buffer::Encoding::Utf16LeBom); break;
        case IDM_ENC_CONVERT_UTF16BE:  app_.ConvertEncoding(Buffer::Encoding::Utf16BeBom); break;
        case IDM_ENC_CONVERT_ANSI:     app_.ConvertEncoding(Buffer::Encoding::Ansi);       break;

        case IDM_EOL_CRLF: app_.ChangeEol(Buffer::Eol::Crlf); break;
        case IDM_EOL_LF:   app_.ChangeEol(Buffer::Eol::Lf);   break;
        case IDM_EOL_CR:   app_.ChangeEol(Buffer::Eol::Cr);   break;

        case IDM_VIEW_FOLDER_WORKSPACE:
            app_.ToggleDock(DockSide::Left);
            break;
        case IDM_VIEW_FUNCTION_LIST:
            app_.Dock().SwapPanel(DockSide::Right, &app_.FunctionListPane());
            app_.ToggleDock(DockSide::Right);
            break;
        case IDM_VIEW_DOC_MAP:
            app_.Dock().SwapPanel(DockSide::Right, &app_.DocMapPane());
            app_.ToggleDock(DockSide::Right);
            break;
        case IDM_VIEW_FIND_RESULTS:
            app_.ToggleDock(DockSide::Bottom);
            break;
        case IDM_VIEW_WORD_WRAP: {
            bool on = !Parameters::Instance().WordWrap();
            Parameters::Instance().SetWordWrap(on);
            for (int v = 0; v < 2; ++v) {
                if (app_.V(v).editor.Hwnd())
                    app_.V(v).editor.Call(SCI_SETWRAPMODE,
                        on ? SC_WRAP_WORD : SC_WRAP_NONE);
            }
            UpdateCheckedMenus();
            Parameters::Instance().Save();
            break;
        }
        case IDM_VIEW_DARKMODE:
            CycleTheme();
            break;
        case IDM_VIEW_THEME_LIGHT:
        case IDM_VIEW_THEME_DARK:
        case IDM_VIEW_THEME_HIGHCONTRAST:
        case IDM_VIEW_THEME_MINT:
        case IDM_VIEW_THEME_NORDIC:
        case IDM_VIEW_THEME_DEEPBLUE:
            SetTheme(static_cast<ThemeId>(LOWORD(w) - IDM_VIEW_THEME_BASE));
            break;
        case IDM_EDIT_COL_EDITOR:
            ShowColumnEditorDialog();
            break;
        case IDM_EDIT_COLUMN_MODE:
            // Informational, Notepad++-style. Column selection is modifier-
            // driven (Alt), not a sticky editor mode: the old
            // SCI_SETSELECTIONMODE toggle was silently dropped by Scintilla
            // on Esc/click, leaving a stale menu checkmark — and its OFF
            // branch also disabled the press-Alt-mid-drag switch.
            ::MessageBoxW(h,
                L"Column (rectangular) selection:\n\n"
                L"  \x2022  Hold Alt and drag with the mouse\n"
                L"  \x2022  Or hold Alt+Shift and use the arrow keys\n\n"
                L"You can also press Alt during a drag to turn the\n"
                L"current selection rectangular.\n\n"
                L"Use Edit > Column Editor (Alt+C) to insert text or\n"
                L"number sequences into the selected columns.",
                L"Column Mode", MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_EDIT_BINARY_MODE: {
            int vBefore = app_.ActiveView();
            app_.ToggleBinaryMode();
            // Ensure the editor that hosts the hex-dump has our input filter.
            InstallBinaryEditHook(app_.V(vBefore).editor.Hwnd(), &app_, vBefore);
            UpdateCheckedMenus();
            app_.UpdateStatusBar(statusBar_);
            app_.UpdateTitle(hwnd_);
            break;
        }
        case IDM_EDIT_JSON_FORMAT:
            if (app_.FormatActiveAsJson()) {
                app_.UpdateStatusBar(statusBar_);
                app_.UpdateTitle(hwnd_);
            }
            break;
        case IDM_TOOL_COMPARE_TEXT:
            ShowTextComparePicker();
            break;
        case IDM_TOOL_COMPARE_HEX:
            ShowHexComparePicker();
            break;
        case IDM_TOOL_COMPARE_FOLDER:
            FolderCompareWindow::Open(hwnd_, hInst_);
            break;
        case IDM_TOOL_COMPARE_NEXT_DIFF:
            TextCompareWindow::NextDiffActive();
            break;
        case IDM_TOOL_COMPARE_PREV_DIFF:
            TextCompareWindow::PreviousDiffActive();
            break;
        case IDM_VIEW_FOLDER_OPEN: {
            BROWSEINFOW bi{};
            bi.hwndOwner = h;
            bi.lpszTitle = L"Add folder to workspace";
            bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH];
                if (::SHGetPathFromIDListW(pidl, path)) {
                    app_.FolderPane().AddRoot(path);
                    if (!app_.Dock().IsShown(DockSide::Left))
                        app_.ToggleDock(DockSide::Left);
                }
                ::CoTaskMemFree(pidl);
            }
            break;
        }
        case IDM_SEARCH_FINDFILES: {
            FindInFilesParams in{}, out{};
            in.subdirs = true;
            in.filters = L"*.*";
            out = in;
            if (ShowFindInFilesDlg(h, hInst_, in, out)) {
                app_.RunFindInFiles(out);
            }
            break;
        }

        default:
            if ((id >= IDM_EDIT_UNDO && id <= IDM_EDIT_OUTDENT)) {
                app_.OnEdit(id);
            }
            break;
        }
        UpdateCheckedMenus();
        app_.UpdateTitle(h);
        app_.UpdateStatusBar(statusBar_);
        return 0;
    }

    case WM_CLOSE:
        if (!app_.CanQuit(h)) return 0;
        app_.Shutdown();   // release Scintilla docs/windows before teardown
        ::DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        Parameters::Instance().Save();
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
}

namespace {

struct ColEditState {
    Notepad_plus::ColumnEditParams p{};
    bool ok = false;
};

INT_PTR CALLBACK ColEditDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<ColEditState*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_INITDIALOG:
        ::SetWindowLongPtrW(h, GWLP_USERDATA, l);
        ::CheckRadioButton(h, IDC_COL_RADIO_TEXT, IDC_COL_RADIO_NUM, IDC_COL_RADIO_TEXT);
        ::CheckRadioButton(h, IDC_COL_BASE_DEC,  IDC_COL_BASE_BIN,  IDC_COL_BASE_DEC);
        ::SetDlgItemTextW(h, IDC_COL_NUM_INIT, L"1");
        ::SetDlgItemTextW(h, IDC_COL_NUM_INC,  L"1");
        ::SetDlgItemTextW(h, IDC_COL_NUM_PAD,  L"0");
        ::SetFocus(::GetDlgItem(h, IDC_COL_TEXT));
        return FALSE;
    case WM_COMMAND: {
        WORD id = LOWORD(w);
        if (id == IDOK) {
            st = reinterpret_cast<ColEditState*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
            wchar_t buf[1024]{};
            st->p.insertNumber = (::IsDlgButtonChecked(h, IDC_COL_RADIO_NUM) == BST_CHECKED);
            ::GetDlgItemTextW(h, IDC_COL_TEXT, buf, 1024);
            st->p.text = buf;
            ::GetDlgItemTextW(h, IDC_COL_NUM_INIT, buf, 64);
            st->p.initial   = ::_wtoi64(buf);
            ::GetDlgItemTextW(h, IDC_COL_NUM_INC,  buf, 64);
            st->p.increment = ::_wtoi64(buf);
            ::GetDlgItemTextW(h, IDC_COL_NUM_PAD,  buf, 16);
            st->p.padWidth  = ::_wtoi(buf);
            if (::IsDlgButtonChecked(h, IDC_COL_BASE_HEX) == BST_CHECKED) st->p.base = 16;
            else if (::IsDlgButtonChecked(h, IDC_COL_BASE_OCT) == BST_CHECKED) st->p.base = 8;
            else if (::IsDlgButtonChecked(h, IDC_COL_BASE_BIN) == BST_CHECKED) st->p.base = 2;
            else st->p.base = 10;
            st->p.leadingZeros = (::IsDlgButtonChecked(h, IDC_COL_LEADZERO) == BST_CHECKED);
            st->ok = true;
            ::EndDialog(h, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) { ::EndDialog(h, IDCANCEL); return TRUE; }
        break;
    }
    case WM_CLOSE: ::EndDialog(h, IDCANCEL); return TRUE;
    }
    return FALSE;
}

}  // namespace

void Notepad_plus_Window::ShowColumnEditorDialog()
{
    ColEditState st;
    ::DialogBoxParamW(hInst_, MAKEINTRESOURCEW(IDD_COL_EDITOR),
        hwnd_, ColEditDlgProc, reinterpret_cast<LPARAM>(&st));
    if (st.ok) app_.ColumnEdit(st.p);
}

// ---- Text Compare picker ----------------------------------------------------

namespace {

struct TxCmpPickState {
    std::wstring left;
    std::wstring right;
    bool ignoreWS   = false;
    bool ignoreCase = false;
    bool ok = false;
};

// Shared by the text and hex pickers — both use the same file-path + Browse
// layout, so a common helper keeps behavior identical.
void BrowseInto(HWND parent, HWND edit)
{
    wchar_t path[MAX_PATH] = {0};
    ::GetWindowTextW(edit, path, MAX_PATH);
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = parent;
    ofn.lpstrFile = path;
    ofn.nMaxFile  = MAX_PATH;
    ofn.lpstrFilter = L"All Files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (::GetOpenFileNameW(&ofn)) {
        ::SetWindowTextW(edit, path);
    }
}

INT_PTR CALLBACK TxCmpDlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    TxCmpPickState* st = reinterpret_cast<TxCmpPickState*>(
        ::GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_INITDIALOG: {
        ::SetWindowLongPtrW(h, GWLP_USERDATA, l);
        auto* s = reinterpret_cast<TxCmpPickState*>(l);
        ::SetDlgItemTextW(h, IDC_TXCMP_LEFT_PATH,  s->left.c_str());
        ::SetDlgItemTextW(h, IDC_TXCMP_RIGHT_PATH, s->right.c_str());
        return TRUE;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(w);
        if (id == IDC_TXCMP_LEFT_BROWSE)  { BrowseInto(h, ::GetDlgItem(h, IDC_TXCMP_LEFT_PATH));  return TRUE; }
        if (id == IDC_TXCMP_RIGHT_BROWSE) { BrowseInto(h, ::GetDlgItem(h, IDC_TXCMP_RIGHT_PATH)); return TRUE; }
        if (id == IDOK) {
            wchar_t L[MAX_PATH], R[MAX_PATH];
            ::GetDlgItemTextW(h, IDC_TXCMP_LEFT_PATH,  L, MAX_PATH);
            ::GetDlgItemTextW(h, IDC_TXCMP_RIGHT_PATH, R, MAX_PATH);
            if (!*L || !*R) {
                ::MessageBoxW(h, L"Pick a file for both sides.", L"Compare Text", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            st->left = L; st->right = R;
            st->ignoreWS   = ::SendMessageW(::GetDlgItem(h, IDC_TXCMP_IGNORE_WS),   BM_GETCHECK, 0, 0) == BST_CHECKED;
            st->ignoreCase = ::SendMessageW(::GetDlgItem(h, IDC_TXCMP_IGNORE_CASE), BM_GETCHECK, 0, 0) == BST_CHECKED;
            st->ok = true;
            ::EndDialog(h, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) { ::EndDialog(h, IDCANCEL); return TRUE; }
        return FALSE;
    }
    }
    return FALSE;
}

}  // namespace

void Notepad_plus_Window::ShowTextComparePicker()
{
    TxCmpPickState st;
    auto& params = Parameters::Instance();
    st.left  = params.cmpTextLeft;
    st.right = params.cmpTextRight;
    INT_PTR r = ::DialogBoxParamW(hInst_, MAKEINTRESOURCEW(IDD_TEXT_COMPARE_PICK),
        hwnd_, TxCmpDlgProc, reinterpret_cast<LPARAM>(&st));
    if (r != IDOK || !st.ok) return;
    params.cmpTextLeft  = st.left;
    params.cmpTextRight = st.right;
    params.Save();

    std::vector<char> lbytes, rbytes;
    if (!ReadFileAll(st.left, lbytes)) {
        ::MessageBoxW(hwnd_, (L"Cannot read:\n" + st.left).c_str(),
            L"Compare Text", MB_OK | MB_ICONERROR);
        return;
    }
    if (!ReadFileAll(st.right, rbytes)) {
        ::MessageBoxW(hwnd_, (L"Cannot read:\n" + st.right).c_str(),
            L"Compare Text", MB_OK | MB_ICONERROR);
        return;
    }

    auto fileName = [](const std::wstring& p) -> std::wstring {
        size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? p : p.substr(slash + 1);
    };

    LineDiffOptions opt{ st.ignoreWS, st.ignoreCase };
    TextCompareWindow::Open(hwnd_, hInst_,
        fileName(st.left),  std::string(lbytes.begin(), lbytes.end()),
        fileName(st.right), std::string(rbytes.begin(), rbytes.end()), opt);
}

// ---- Hex Compare picker -----------------------------------------------------

namespace {

struct HexCmpState {
    std::wstring left;
    std::wstring right;
    bool ok = false;
};

INT_PTR CALLBACK HexCmpDlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    HexCmpState* st = reinterpret_cast<HexCmpState*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_INITDIALOG: {
        ::SetWindowLongPtrW(h, GWLP_USERDATA, l);
        auto* s = reinterpret_cast<HexCmpState*>(l);
        ::SetDlgItemTextW(h, IDC_HEXCMP_LEFT_PATH,  s->left.c_str());
        ::SetDlgItemTextW(h, IDC_HEXCMP_RIGHT_PATH, s->right.c_str());
        return TRUE;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(w);
        if (id == IDC_HEXCMP_LEFT_BROWSE)  { BrowseInto(h, ::GetDlgItem(h, IDC_HEXCMP_LEFT_PATH));  return TRUE; }
        if (id == IDC_HEXCMP_RIGHT_BROWSE) { BrowseInto(h, ::GetDlgItem(h, IDC_HEXCMP_RIGHT_PATH)); return TRUE; }
        if (id == IDOK) {
            wchar_t L[MAX_PATH], R[MAX_PATH];
            ::GetDlgItemTextW(h, IDC_HEXCMP_LEFT_PATH,  L, MAX_PATH);
            ::GetDlgItemTextW(h, IDC_HEXCMP_RIGHT_PATH, R, MAX_PATH);
            if (!*L || !*R) {
                ::MessageBoxW(h, L"Pick a file for both sides.", L"Compare Hex", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            st->left = L; st->right = R; st->ok = true;
            ::EndDialog(h, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) { ::EndDialog(h, IDCANCEL); return TRUE; }
        return FALSE;
    }
    }
    return FALSE;
}

}  // namespace

void Notepad_plus_Window::ShowHexComparePicker()
{
    HexCmpState st;
    auto& params = Parameters::Instance();
    st.left  = params.cmpHexLeft;
    st.right = params.cmpHexRight;
    INT_PTR r = ::DialogBoxParamW(hInst_, MAKEINTRESOURCEW(IDD_HEX_COMPARE),
        hwnd_, HexCmpDlgProc, reinterpret_cast<LPARAM>(&st));
    if (r != IDOK || !st.ok) return;
    params.cmpHexLeft  = st.left;
    params.cmpHexRight = st.right;
    params.Save();
    HexCompareWindow::Open(hwnd_, hInst_, st.left, st.right);
}

void Notepad_plus_Window::FlushPendingStyle()
{
    Buffer* b = BufferManager::Instance().Get(app_.ActiveBuffer());
    if (!b || !b->StyleDirty()) return;
    ScintillaEditView& ed = app_.Editor();
    // Wrap in WM_SETREDRAW so the thousands of SCI_SETSTYLING calls coalesce
    // into a single repaint — same pattern ApplyLanguage's heavy path uses.
    if (ed.Hwnd()) ::SendMessageW(ed.Hwnd(), WM_SETREDRAW, FALSE, 0);
    if (b->GetLang() == LangType::Markdown) StyleMarkdownFences(ed);
    else                                    HighlightFunctionNames(ed, b->GetLang());
    if (ed.Hwnd()) {
        ::SendMessageW(ed.Hwnd(), WM_SETREDRAW, TRUE, 0);
        ::InvalidateRect(ed.Hwnd(), nullptr, FALSE);
    }
    b->SetStyleDirty(false);
}

void Notepad_plus_Window::WireTabContextForView(int v)
{
    if (!app_.V(v).tabs.Hwnd()) return;
    app_.V(v).tabs.SetOnContext([this, v](BufferID id, POINT screenPt) {
        ctxMenuBuffer_ = id;
        app_.SetActiveView(v);
        HMENU tm = ::LoadMenuW(hInst_, MAKEINTRESOURCEW(IDR_TAB_CONTEXT));
        HMENU pop = ::GetSubMenu(tm, 0);
        app_.V(v).tabs.Activate(id);
        ::TrackPopupMenu(pop, TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0,
            hwnd_, nullptr);
        ::DestroyMenu(tm);
    });
}

} // namespace npp
