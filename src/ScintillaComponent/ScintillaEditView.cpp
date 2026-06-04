#include "ScintillaEditView.h"

#include <Scintilla.h>
#include <ScintillaMessages.h>
#include <SciLexer.h>

#include <vector>

namespace npp {

// Scintilla exports Scintilla_RegisterClasses for DLL builds; when built as a
// static lib (STATIC_BUILD), it still exposes the function to register the
// Win32 window class "Scintilla".
extern "C" int Scintilla_RegisterClasses(void* hInstance);
extern "C" int Scintilla_ReleaseResources();

ScintillaEditView::~ScintillaEditView()
{
    Destroy();
}

void ScintillaEditView::Destroy()
{
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_      = nullptr;
        direct_    = nullptr;
        directPtr_ = 0;
    }
}

bool ScintillaEditView::Create(HWND parent, HINSTANCE hInst)
{
    static bool registered = false;
    if (!registered) {
        Scintilla_RegisterClasses(hInst);
        registered = true;
    }

    hwnd_ = ::CreateWindowExW(
        0, L"Scintilla", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 100, 100,
        parent, nullptr, hInst, nullptr);

    if (!hwnd_) return false;

    directPtr_ = static_cast<sptr_t>(
        ::SendMessageW(hwnd_, SCI_GETDIRECTPOINTER, 0, 0));
    direct_    = reinterpret_cast<DirectFn>(
        ::SendMessageW(hwnd_, SCI_GETDIRECTFUNCTION, 0, 0));

    ApplyDefaultStyle();
    return true;
}

sptr_t ScintillaEditView::Call(unsigned int msg, uptr_t wParam, sptr_t lParam) const
{
    if (direct_) return direct_(directPtr_, msg, wParam, lParam);
    return ::SendMessageW(hwnd_, msg, wParam, lParam);
}

void ScintillaEditView::ApplyDefaultStyle()
{
    Call(SCI_SETCODEPAGE, SC_CP_UTF8);
    Call(SCI_SETEOLMODE, SC_EOL_CRLF);
    Call(SCI_SETTABWIDTH, 4);
    Call(SCI_SETUSETABS, 0);
    Call(SCI_SETINDENT, 4);
    Call(SCI_SETVIEWWS, SCWS_INVISIBLE);
    // Virtual space only while a rectangular selection is active: column
    // selects can still reach past EOL / into tab gaps so the block stays
    // visually aligned, but a normal click or arrow move past line end snaps
    // the caret back to the newline like a regular editor.
    Call(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION);
    // Treat tab stops as exact multi-character columns (matters once docs
    // contain literal tabs — tab width is already 4 above).
    Call(SCI_SETMOUSESELECTIONRECTANGULARSWITCH, 1);
    // Column typing: a rectangular selection is N per-line ranges, and these
    // make typing / Backspace / Delete / paste act on every range instead of
    // just the main caret (N++/VS Code behavior). Also enables Ctrl+click
    // multi-caret editing.
    Call(SCI_SETMULTIPLESELECTION, 1);
    Call(SCI_SETADDITIONALSELECTIONTYPING, 1);
    Call(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);

    // DirectWrite gives consistent sub-pixel metrics — important so the
    // GB2312 fallback below renders with stable widths for column selects.
    // DirectWrite requires Win7 Platform Update (KB2670838) or Win8+;
    // fall back to GDI if SetTechnology fails (returns 0 on unsupported OS).
    {
        OSVERSIONINFOW ovi{};
        ovi.dwOSVersionInfoSize = sizeof(ovi);
        #pragma warning(suppress: 4996) // GetVersionExW deprecated but works on Win7
        ::GetVersionExW(&ovi);
        if (ovi.dwMajorVersion > 6 ||
            (ovi.dwMajorVersion == 6 && ovi.dwMinorVersion >= 2)) {
            // Win8+ — DirectWrite always available. Plain DIRECTWRITE, not
            // DIRECTWRITERETAIN: the "retain" flavor reuses the previous
            // frame's surface and on some drivers leaves stale blank bands
            // between lines after edits that shift text (e.g. pressing
            // Enter), until the next full repaint. Notepad++ ships plain
            // DirectWrite for the same reason.
            Call(SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DIRECTWRITE);
        }
        // Win7: stay with default GDI to avoid Platform Update dependency
    }

    // Default style: Consolas 10 pt with a GB2312 charset hint.
    // The charset hint pushes Windows' CJK glyph fallback from the *proportional*
    // "Microsoft YaHei" toward the *monospace* "NSimSun / 新宋体", so each CJK
    // character renders at exactly 2× the ASCII width — which is what makes
    // rectangular (column) selections line up visually across mixed lines.
    Call(SCI_STYLESETFONT, STYLE_DEFAULT,
        reinterpret_cast<sptr_t>("Consolas"));
    Call(SCI_STYLESETSIZE,        STYLE_DEFAULT, 10);
    Call(SCI_STYLESETCHARACTERSET,STYLE_DEFAULT, GB2312_CHARSET);
    Call(SCI_STYLESETFORE,        STYLE_DEFAULT, 0x1E1E1E);
    Call(SCI_STYLESETBACK,        STYLE_DEFAULT, 0xFFFFFF);
    Call(SCI_STYLECLEARALL);

    // Line numbers: muted gray foreground on a very light sidebar.
    Call(SCI_STYLESETFORE, STYLE_LINENUMBER, 0x909090);
    Call(SCI_STYLESETBACK, STYLE_LINENUMBER, 0xF5F5F5);

    // Indent guides + brace matching.
    Call(SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH);
    Call(SCI_STYLESETFORE, STYLE_INDENTGUIDE, 0xD8D8D8);
    Call(SCI_STYLESETFORE, STYLE_BRACELIGHT, 0x0066CC);
    Call(SCI_STYLESETBACK, STYLE_BRACELIGHT, 0xE8F1FB);
    Call(SCI_STYLESETBOLD, STYLE_BRACELIGHT, 1);
    Call(SCI_STYLESETFORE, STYLE_BRACEBAD,   0x3030FF);
    Call(SCI_STYLESETBOLD, STYLE_BRACEBAD,   1);

    // Softer selection & caret colors (feel modern, not high-contrast).
    Call(SCI_SETSELFORE, 0, 0);
    Call(SCI_SETSELBACK, 1, 0xE8D4A6);         // pale blue (BGR)
    Call(SCI_SETSELALPHA, 96);
    Call(SCI_SETADDITIONALSELALPHA, 96);
    Call(SCI_SETCARETFORE, 0x303030);
    Call(SCI_SETCARETWIDTH, 2);
    // Keep the caret a vertical line at all times: unbind Insert so the user
    // can't toggle overtype (whose caret renders as an underscore). Binary
    // mode still enables overtype programmatically for hex-column editing.
    Call(SCI_ASSIGNCMDKEY, SCK_INSERT, SCI_NULL);

    // Line numbers margin (margin 0), width grown on demand later.
    Call(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    Call(SCI_SETMARGINWIDTHN, 0, 44);

    // Bookmark margin (margin 1).
    Call(SCI_SETMARGINTYPEN,  1, SC_MARGIN_SYMBOL);
    Call(SCI_SETMARGINMASKN,  1, 1 << 24);          // bookmark marker #24
    Call(SCI_SETMARGINSENSITIVEN, 1, 1);
    Call(SCI_SETMARGINWIDTHN, 1, 16);
    Call(SCI_MARKERDEFINE,    24, SC_MARK_BOOKMARK);
    Call(SCI_MARKERSETFORE,   24, RGB(0xFF,0xFF,0xFF));
    Call(SCI_MARKERSETBACK,   24, RGB(0x00,0x78,0xD7));

    // Fold margin (margin 2) — width grows when folding is turned on.
    Call(SCI_SETMARGINTYPEN, 2, SC_MARGIN_SYMBOL);
    Call(SCI_SETMARGINMASKN, 2, SC_MASK_FOLDERS);
    Call(SCI_SETMARGINWIDTHN, 2, 0);

    Call(SCI_SETCARETLINEVISIBLE, 1);
    Call(SCI_SETCARETLINEBACK, 0xF5F8FA);
    Call(SCI_SETCARETLINEBACKALPHA, 256);

    // Softer EOL whitespace (not shown unless toggled, but ready).
    Call(SCI_SETWHITESPACEFORE, 1, 0xC8C8C8);

    // Suppress Scintilla's built-in popup so our WM_CONTEXTMENU hook owns it.
    Call(SCI_USEPOPUP, SC_POPUP_NEVER);
}

void ScintillaEditView::Resize(const RECT& rc)
{
    if (!hwnd_) return;
    ::MoveWindow(hwnd_,
        rc.left, rc.top,
        rc.right - rc.left, rc.bottom - rc.top,
        TRUE);
}

void ScintillaEditView::SetText(const std::string& utf8)
{
    Call(SCI_CLEARALL);
    Call(SCI_SETUNDOCOLLECTION, 0);
    Call(SCI_ADDTEXT, utf8.size(),
        reinterpret_cast<sptr_t>(utf8.data()));
    Call(SCI_SETUNDOCOLLECTION, 1);
    Call(SCI_EMPTYUNDOBUFFER);
    Call(SCI_SETSAVEPOINT);
    Call(SCI_GOTOPOS, 0);
}

std::string ScintillaEditView::GetText() const
{
    const sptr_t len = Call(SCI_GETLENGTH);
    std::string out(static_cast<size_t>(len), '\0');
    if (len > 0) {
        Call(SCI_GETTEXT,
            static_cast<uptr_t>(len + 1),
            reinterpret_cast<sptr_t>(out.data()));
    }
    return out;
}

bool ScintillaEditView::IsDirty() const
{
    return Call(SCI_GETMODIFY) != 0;
}

void ScintillaEditView::MarkSaved()
{
    Call(SCI_SETSAVEPOINT);
}

void ScintillaEditView::ClearAll()
{
    Call(SCI_CLEARALL);
    Call(SCI_EMPTYUNDOBUFFER);
    Call(SCI_SETSAVEPOINT);
}

void ScintillaEditView::AttachDocument(sptr_t docHandle)
{
    // SCI_SETDOCPOINTER releases the previously-attached document (decrementing
    // its refcount) and adds a reference to the new one.
    Call(SCI_SETDOCPOINTER, 0, docHandle);
}

void ScintillaEditView::SetFocus()
{
    if (hwnd_) ::SetFocus(hwnd_);
}

} // namespace npp
