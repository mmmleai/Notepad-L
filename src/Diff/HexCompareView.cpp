#include "HexCompareView.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace npp {

namespace {
constexpr wchar_t kCls[] = L"NotePadLHexView";

void RegisterOnce(HINSTANCE hInst)
{
    static bool reg = false;
    if (reg) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc   = HexCompareView::StaticProc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_IBEAM);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kCls;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    ::RegisterClassW(&wc);
    reg = true;
}

// Layout in character cells:
//   [ 0 .. 7 ]         offset (8 hex)
//   [ 8 ]              space
//   [ 9 .. 9+3*16-1 ]  bytes:  "HH " × 16 (with extra space between byte 7 and 8)
// We lay out bytes one at a time, so per-byte rects are precise for diff-bg.
//
// Columns (in character units), computed in code:
//   OFFSET_END   = 8
//   BYTE_START   = 9
//   BYTE_STRIDE  = 3           (2 hex chars + 1 space)
//   HALF_GAP     = 1 extra char between bytes #7 and #8
//   ASCII_GAP    = 2
//   ASCII_START  = BYTE_START + BYTE_STRIDE*16 + HALF_GAP + ASCII_GAP
constexpr int kOffCells        = 8;
constexpr int kByteStartCell   = kOffCells + 1;
constexpr int kByteStrideCells = 3;
constexpr int kHalfGapCells    = 1;
constexpr int kAsciiStartCell  = kByteStartCell + kByteStrideCells * 16 + kHalfGapCells + 2;
constexpr int kTotalCells      = kAsciiStartCell + 1 + 16 + 1;  // |ascii|

inline char Printable(uint8_t b) { return (b >= 0x20 && b < 0x7f) ? static_cast<char>(b) : '.'; }

}  // namespace

LRESULT CALLBACK HexCompareView::StaticProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    HexCompareView* self = nullptr;
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(l);
        self = static_cast<HexCompareView*>(cs->lpCreateParams);
        self->hwnd_ = h;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<HexCompareView*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self) return ::DefWindowProcW(h, m, w, l);
    return self->Proc(h, m, w, l);
}

bool HexCompareView::Create(HWND parent, HINSTANCE hInst, int id)
{
    RegisterOnce(hInst);
    hwnd_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, kCls, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
        0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        hInst, this);
    return hwnd_ != nullptr;
}

void HexCompareView::EnsureFont()
{
    if (font_) return;
    font_ = ::CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
    if (!font_) {
        font_ = ::CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }
    HDC dc = ::GetDC(hwnd_);
    HFONT old = static_cast<HFONT>(::SelectObject(dc, font_));
    TEXTMETRIC tm{};
    ::GetTextMetrics(dc, &tm);
    cellW_ = tm.tmAveCharWidth;
    cellH_ = tm.tmHeight + 2;
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd_, dc);
}

int64_t HexCompareView::RowCount() const
{
    uint64_t bigger = std::max(size_, peerSz_);
    return static_cast<int64_t>((bigger + kBytesPerRow - 1) / kBytesPerRow);
}

void HexCompareView::SetBuffer(const uint8_t* data, uint64_t size)
{
    data_ = data; size_ = size;
    topRow_ = 0;
    UpdateScrollbar();
    Invalidate();
}

void HexCompareView::SetPeer(const uint8_t* data, uint64_t size)
{
    peer_ = data; peerSz_ = size;
    UpdateScrollbar();
    Invalidate();
}

void HexCompareView::SetTopRow(int64_t row)
{
    int64_t maxRow = std::max<int64_t>(0, RowCount() - rowsVisible_);
    row = std::clamp<int64_t>(row, 0, maxRow);
    if (row == topRow_) return;
    topRow_ = row;
    SCROLLINFO si{ sizeof(si) };
    si.fMask = SIF_POS;
    si.nPos  = static_cast<int>(std::min<int64_t>(topRow_, INT_MAX));
    ::SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    Invalidate();
}

void HexCompareView::Invalidate()
{
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void HexCompareView::UpdateScrollbar()
{
    if (!hwnd_) return;
    int64_t rc = RowCount();
    SCROLLINFO si{ sizeof(si) };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin  = 0;
    si.nMax  = static_cast<int>(std::min<int64_t>(rc ? rc - 1 : 0, INT_MAX));
    si.nPage = static_cast<UINT>(std::max(1, rowsVisible_));
    si.nPos  = static_cast<int>(std::min<int64_t>(topRow_, INT_MAX));
    ::SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void HexCompareView::OnSize()
{
    EnsureFont();
    RECT rc; ::GetClientRect(hwnd_, &rc);
    rowsVisible_ = std::max<int>(1, static_cast<int>((rc.bottom - rc.top) / cellH_));
    UpdateScrollbar();
}

void HexCompareView::ScrollBy(int64_t rows)
{
    SetTopRow(topRow_ + rows);
    if (onScroll_) onScroll_(topRow_);
}

void HexCompareView::OnVScroll(WPARAM w)
{
    SCROLLINFO si{ sizeof(si) };
    si.fMask = SIF_ALL;
    ::GetScrollInfo(hwnd_, SB_VERT, &si);
    int64_t target = topRow_;
    switch (LOWORD(w)) {
    case SB_LINEUP:       target -= 1; break;
    case SB_LINEDOWN:     target += 1; break;
    case SB_PAGEUP:       target -= rowsVisible_; break;
    case SB_PAGEDOWN:     target += rowsVisible_; break;
    case SB_TOP:          target = 0; break;
    case SB_BOTTOM:       target = RowCount(); break;
    case SB_THUMBPOSITION:
    case SB_THUMBTRACK:   target = si.nTrackPos; break;
    default: return;
    }
    int64_t maxRow = std::max<int64_t>(0, RowCount() - rowsVisible_);
    target = std::clamp<int64_t>(target, 0, maxRow);
    if (target != topRow_) {
        topRow_ = target;
        UpdateScrollbar();
        Invalidate();
        if (onScroll_) onScroll_(topRow_);
    }
}

void HexCompareView::Paint(HDC dc)
{
    EnsureFont();
    RECT rc; ::GetClientRect(hwnd_, &rc);

    // Double-buffer to avoid flicker on scroll.
    HDC mem = ::CreateCompatibleDC(dc);
    HBITMAP bmp = ::CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HBITMAP oldBmp = static_cast<HBITMAP>(::SelectObject(mem, bmp));
    HFONT oldFont = static_cast<HFONT>(::SelectObject(mem, font_));
    ::SetBkMode(mem, TRANSPARENT);

    HBRUSH bgBrush = ::CreateSolidBrush(bg_);
    ::FillRect(mem, &rc, bgBrush);
    ::DeleteObject(bgBrush);

    int64_t totalRows = RowCount();
    int64_t avail = totalRows - topRow_;
    if (avail < 0) avail = 0;
    // Bounded by rowsVisible_+1 (screen rows), so the narrowing cast is safe.
    const int rowsToDraw = static_cast<int>(
        std::min<int64_t>(rowsVisible_ + 1, avail));

    wchar_t buf[32];
    for (int r = 0; r < rowsToDraw; ++r) {
        int64_t row = topRow_ + r;
        uint64_t rowStart = static_cast<uint64_t>(row) * kBytesPerRow;
        int y = r * cellH_;

        // --- offset
        ::SetTextColor(mem, fgOffset_);
        int n = ::swprintf_s(buf, L"%08llX", static_cast<unsigned long long>(rowStart));
        ::TextOutW(mem, 0, y, buf, n);

        // --- bytes
        ::SetTextColor(mem, fg_);
        for (int c = 0; c < kBytesPerRow; ++c) {
            int cell = kByteStartCell + c * kByteStrideCells + (c >= 8 ? kHalfGapCells : 0);
            int x = cell * cellW_;
            uint64_t off = rowStart + c;
            bool hasMine  = off < size_;
            bool hasPeer  = off < peerSz_;
            bool differs  = hasMine && hasPeer && data_[off] != peer_[off];
            bool missing  = hasMine != hasPeer;

            if (differs || missing) {
                RECT cell_rc{ x, y, x + 2 * cellW_ + cellW_/2, y + cellH_ };
                HBRUSH br = ::CreateSolidBrush(differs ? bgDiff_ : bgMiss_);
                ::FillRect(mem, &cell_rc, br);
                ::DeleteObject(br);
            }
            if (hasMine) {
                uint8_t b = data_[off];
                ::swprintf_s(buf, L"%02X", static_cast<int>(b));
                ::TextOutW(mem, x, y, buf, 2);
            } else if (hasPeer) {
                // grey "  " placeholder — already filled above
            }
        }

        // --- ascii
        int ascBase = kAsciiStartCell * cellW_;
        // left pipe
        ::TextOutW(mem, (kAsciiStartCell - 1) * cellW_, y, L"|", 1);
        for (int c = 0; c < kBytesPerRow; ++c) {
            uint64_t off = rowStart + c;
            int x = ascBase + c * cellW_;
            bool hasMine = off < size_;
            bool hasPeer = off < peerSz_;
            bool differs = hasMine && hasPeer && data_[off] != peer_[off];
            bool missing = hasMine != hasPeer;
            if (differs || missing) {
                RECT cell_rc{ x, y, x + cellW_, y + cellH_ };
                HBRUSH br = ::CreateSolidBrush(differs ? bgDiff_ : bgMiss_);
                ::FillRect(mem, &cell_rc, br);
                ::DeleteObject(br);
            }
            if (hasMine) {
                char ch = Printable(data_[off]);
                wchar_t wc = static_cast<wchar_t>(static_cast<unsigned char>(ch));
                ::TextOutW(mem, x, y, &wc, 1);
            }
        }
        ::TextOutW(mem, (kAsciiStartCell + kBytesPerRow) * cellW_, y, L"|", 1);
    }

    ::BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    ::SelectObject(mem, oldBmp);
    ::SelectObject(mem, oldFont);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
}

LRESULT HexCompareView::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = ::BeginPaint(h, &ps);
        Paint(dc);
        ::EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:      OnSize(); return 0;
    case WM_VSCROLL:   OnVScroll(w); return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        UINT lines = 3;
        ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        ScrollBy(-(delta / WHEEL_DELTA) * static_cast<int>(lines));
        return 0;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_UP:     ScrollBy(-1); return 0;
        case VK_DOWN:   ScrollBy( 1); return 0;
        case VK_PRIOR:  ScrollBy(-rowsVisible_); return 0;
        case VK_NEXT:   ScrollBy( rowsVisible_); return 0;
        case VK_HOME:   ScrollBy(INT64_MIN / 2); return 0;
        case VK_END:    ScrollBy(INT64_MAX / 2); return 0;
        }
        break;
    case WM_LBUTTONDOWN: ::SetFocus(h); return 0;
    case WM_NCDESTROY:
        if (font_) { ::DeleteObject(font_); font_ = nullptr; }
        return ::DefWindowProcW(h, m, w, l);
    }
    return ::DefWindowProcW(h, m, w, l);
}

} // namespace npp
