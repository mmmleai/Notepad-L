#include "FindInFilesDlg.h"
#include "../resource.h"
#include "../Parameters/Parameters.h"
#include "../Parameters/Stylers.h"
#include <commctrl.h>
#include <shlobj.h>
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace npp {

namespace {
INT_PTR HandleThemedCtlColor(UINT m, WPARAM w, LPARAM l)
{
    if (!Parameters::Instance().DarkMode()) return 0;
    const UiPalette& u = Ui();
    HDC hdc = reinterpret_cast<HDC>(w);
    ::SetTextColor(hdc, u.text);
    bool isEdit = (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX);
    COLORREF bg = isEdit ? u.editorBg : u.chromeBg;
    ::SetBkColor(hdc, bg);
    // Color-tracked: theme switches between dark families rebuild the
    // brushes instead of keeping the first theme's colors forever.
    static HBRUSH   brChrome = nullptr, brEditor = nullptr;
    static COLORREF brChromeClr = CLR_INVALID, brEditorClr = CLR_INVALID;
    if (!brChrome || brChromeClr != u.chromeBg) {
        if (brChrome) ::DeleteObject(brChrome);
        brChrome = ::CreateSolidBrush(u.chromeBg);
        brChromeClr = u.chromeBg;
    }
    if (!brEditor || brEditorClr != u.editorBg) {
        if (brEditor) ::DeleteObject(brEditor);
        brEditor = ::CreateSolidBrush(u.editorBg);
        brEditorClr = u.editorBg;
    }
    (void)l;
    return reinterpret_cast<INT_PTR>(isEdit ? brEditor : brChrome);
}
}

namespace {

struct State {
    FindInFilesParams in;
    FindInFilesParams out;
    bool              ok = false;
};

void BrowseFolder(HWND owner, std::wstring& dirInOut)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Select folder to search in";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH];
    if (::SHGetPathFromIDListW(pidl, path)) dirInOut = path;
    ::CoTaskMemFree(pidl);
}

INT_PTR CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    State* s = reinterpret_cast<State*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_INITDIALOG: {
        s = reinterpret_cast<State*>(l);
        ::SetWindowLongPtrW(h, GWLP_USERDATA, l);
        BOOL dark = Parameters::Instance().DarkMode() ? TRUE : FALSE;
        ::DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE,
            &dark, sizeof(dark));
        ::SetDlgItemTextW(h, IDC_FIF_DIR, s->in.dir.c_str());
        ::SetDlgItemTextW(h, IDC_FIF_FILTERS, s->in.filters.c_str());
        ::SetDlgItemTextW(h, IDC_FIF_WHAT, s->in.what.c_str());
        ::CheckDlgButton(h, IDC_FIF_CASE,    s->in.matchCase ? BST_CHECKED : BST_UNCHECKED);
        ::CheckDlgButton(h, IDC_FIF_WORD,    s->in.wholeWord ? BST_CHECKED : BST_UNCHECKED);
        ::CheckDlgButton(h, IDC_FIF_REGEX,   s->in.regex     ? BST_CHECKED : BST_UNCHECKED);
        ::CheckDlgButton(h, IDC_FIF_SUBDIRS, s->in.subdirs   ? BST_CHECKED : BST_UNCHECKED);
        // Anchor to top-right of owner (frame's editor area approx).
        HWND owner = ::GetWindow(h, GW_OWNER);
        if (owner) {
            RECT er{}, dr{};
            ::GetClientRect(owner, &er);
            ::MapWindowPoints(owner, nullptr, reinterpret_cast<POINT*>(&er), 2);
            ::GetWindowRect(h, &dr);
            const int dw = dr.right - dr.left;
            const int dh = dr.bottom - dr.top;
            constexpr int kMargin = 12;
            int x = er.right - dw - kMargin;
            int y = er.top   + kMargin + 60; // below tab bar / menu
            if (x < er.left + kMargin) x = er.left + kMargin;
            ::SetWindowPos(h, HWND_TOP, x, y, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            (void)dh;
        }
        ::SetFocus(::GetDlgItem(h, IDC_FIF_WHAT));
        return FALSE;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(w);
        if (id == IDC_FIF_BROWSE) {
            wchar_t cur[MAX_PATH]{};
            ::GetDlgItemTextW(h, IDC_FIF_DIR, cur, MAX_PATH);
            std::wstring d = cur;
            BrowseFolder(h, d);
            if (!d.empty()) ::SetDlgItemTextW(h, IDC_FIF_DIR, d.c_str());
            return TRUE;
        }
        if (id == IDC_FIF_FINDALL || id == IDOK) {
            wchar_t buf[1024];
            ::GetDlgItemTextW(h, IDC_FIF_DIR, buf, 1024);      s->out.dir = buf;
            ::GetDlgItemTextW(h, IDC_FIF_FILTERS, buf, 1024);  s->out.filters = buf;
            ::GetDlgItemTextW(h, IDC_FIF_WHAT, buf, 1024);     s->out.what = buf;
            s->out.matchCase = ::IsDlgButtonChecked(h, IDC_FIF_CASE)    == BST_CHECKED;
            s->out.wholeWord = ::IsDlgButtonChecked(h, IDC_FIF_WORD)    == BST_CHECKED;
            s->out.regex     = ::IsDlgButtonChecked(h, IDC_FIF_REGEX)   == BST_CHECKED;
            s->out.subdirs   = ::IsDlgButtonChecked(h, IDC_FIF_SUBDIRS) == BST_CHECKED;
            if (s->out.dir.empty() || s->out.what.empty()) {
                ::MessageBoxW(h, L"Directory and search text are required.",
                    L"Find in Files", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            s->ok = true;
            ::EndDialog(h, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) { ::EndDialog(h, IDCANCEL); return TRUE; }
        break;
    }
    case WM_CLOSE: ::EndDialog(h, IDCANCEL); return TRUE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        INT_PTR r = HandleThemedCtlColor(m, w, l);
        if (r) { ::SetWindowLongPtrW(h, DWLP_MSGRESULT, r); return TRUE; }
        break;
    }
    }
    return FALSE;
}

} // namespace

bool ShowFindInFilesDlg(HWND owner, HINSTANCE hInst,
                        const FindInFilesParams& defaults,
                        FindInFilesParams& out)
{
    State st{ defaults, {}, false };
    INT_PTR r = ::DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_FIND_IN_FILES),
        owner, DlgProc, reinterpret_cast<LPARAM>(&st));
    if (r == IDOK && st.ok) { out = st.out; return true; }
    return false;
}

} // namespace npp
