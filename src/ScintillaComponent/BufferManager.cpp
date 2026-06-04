#include "BufferManager.h"
#include "ScintillaEditView.h"
#include "../MISC/Common/FileIO.h"
#include "../MISC/Common/StringUtil.h"
#include "../Parameters/Stylers.h"

#include <Scintilla.h>
#include <ScintillaMessages.h>

#include <windows.h>
#include <algorithm>

namespace npp {

namespace {
// The factory view doubles as the *visible* editor of view 0, so loading or
// saving a non-active buffer temporarily attaches a foreign document to it.
// This guard swaps the document in and, on scope exit, restores the previous
// document together with its scroll/caret state (re-attaching a document
// resets the view position). Without it, every DoOpen clobbered the previous
// tab's remembered position and DoSaveAll left the wrong document on screen.
struct DocSwapGuard {
    ScintillaEditView* v = nullptr;
    sptr_t prevDoc = 0;
    sptr_t firstVisible = 0, caret = 0, anchor = 0, xOffset = 0;
    bool   swapped = false;

    DocSwapGuard(ScintillaEditView& view, sptr_t targetDoc) : v(&view) {
        prevDoc = v->Call(SCI_GETDOCPOINTER);
        if (prevDoc == targetDoc) return;          // already attached — no-op
        firstVisible = v->Call(SCI_GETFIRSTVISIBLELINE);
        caret        = v->Call(SCI_GETCURRENTPOS);
        anchor       = v->Call(SCI_GETANCHOR);
        xOffset      = v->Call(SCI_GETXOFFSET);
        v->AttachDocument(targetDoc);
        swapped = true;
    }
    ~DocSwapGuard() {
        if (!swapped) return;
        v->AttachDocument(prevDoc);
        // Same restore order as RestoreViewState: selection first, since
        // SCI_SETSEL scrolls the caret into view.
        v->Call(SCI_SETSEL, static_cast<uptr_t>(anchor), caret);
        v->Call(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(firstVisible));
        v->Call(SCI_SETXOFFSET, static_cast<uptr_t>(xOffset));
    }
};
} // namespace

BufferManager& BufferManager::Instance()
{
    static BufferManager m;
    return m;
}

Buffer* BufferManager::Get(BufferID id)
{
    auto it = buffers_.find(id);
    return (it != buffers_.end()) ? it->second.get() : nullptr;
}

const Buffer* BufferManager::Get(BufferID id) const
{
    auto it = buffers_.find(id);
    return (it != buffers_.end()) ? it->second.get() : nullptr;
}

BufferID BufferManager::FindByPath(const std::wstring& path) const
{
    for (const auto& kv : buffers_) {
        const Buffer& b = *kv.second;
        if (!b.IsUntitled() && ::_wcsicmp(b.Path().c_str(), path.c_str()) == 0)
            return b.Id();
    }
    return kInvalidBufferID;
}

std::vector<BufferID> BufferManager::AllIds() const
{
    std::vector<BufferID> out;
    out.reserve(buffers_.size());
    for (const auto& kv : buffers_) out.push_back(kv.first);
    return out;
}

BufferID BufferManager::NewUntitled()
{
    if (!factory_) return kInvalidBufferID;
    const sptr_t doc = factory_->Call(SCI_CREATEDOCUMENT, 0,
        static_cast<sptr_t>(SC_DOCUMENTOPTION_DEFAULT));
    // SCI_CREATEDOCUMENT returns with refcount 1 — we now own it.
    const BufferID id = nextId_++;
    auto b = std::make_unique<Buffer>(id, doc, L"", true, nextUntitled_++);
    buffers_.emplace(id, std::move(b));
    return id;
}

bool BufferManager::LoadIntoDoc(Buffer& b, const std::wstring& path, std::wstring* errorOut)
{
    std::vector<char> bytes;
    if (!ReadFileAll(path, bytes)) {
        if (errorOut) *errorOut = L"Cannot open file:\n" + path;
        return false;
    }

    const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t size = bytes.size();
    Buffer::Encoding enc = Buffer::Encoding::Utf8;
    // For UTF-8 (with or without BOM) we hand raw bytes straight to SCI_ADDTEXT
    // via (textData, textLen) — skipping an intermediate std::string copy that
    // doubles peak memory on large files. Converted paths (UTF-16 / ANSI) still
    // need the `utf8` scratch buffer.
    std::string utf8;
    const char* textData = nullptr;
    size_t      textLen  = 0;

    // BOM-based detection.
    if (size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        enc = Buffer::Encoding::Utf8Bom;
        textData = reinterpret_cast<const char*>(raw + 3);
        textLen  = size - 3;
    } else if (size >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        enc = Buffer::Encoding::Utf16LeBom;
        const wchar_t* w = reinterpret_cast<const wchar_t*>(raw + 2);
        int wlen = static_cast<int>((size - 2) / sizeof(wchar_t));
        int n = ::WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
        utf8.resize(static_cast<size_t>(n));
        ::WideCharToMultiByte(CP_UTF8, 0, w, wlen, utf8.data(), n, nullptr, nullptr);
        textData = utf8.data(); textLen = utf8.size();
    } else if (size >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        enc = Buffer::Encoding::Utf16BeBom;
        std::vector<wchar_t> w((size - 2) / 2);
        for (size_t i = 0; i < w.size(); ++i) {
            w[i] = static_cast<wchar_t>((raw[2 + i*2] << 8) | raw[2 + i*2 + 1]);
        }
        int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
            nullptr, 0, nullptr, nullptr);
        utf8.resize(static_cast<size_t>(n));
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
            utf8.data(), n, nullptr, nullptr);
        textData = utf8.data(); textLen = utf8.size();
    } else {
        // No BOM: try to validate as UTF-8; fall back to ANSI.
        DWORD flags = MB_ERR_INVALID_CHARS;
        int wn = ::MultiByteToWideChar(CP_UTF8, flags,
            reinterpret_cast<const char*>(raw), static_cast<int>(size),
            nullptr, 0);
        if (wn > 0 || size == 0) {
            enc = Buffer::Encoding::Utf8;
            textData = reinterpret_cast<const char*>(raw);
            textLen  = size;
        } else {
            enc = Buffer::Encoding::Ansi;
            int wlen = ::MultiByteToWideChar(CP_ACP, 0,
                reinterpret_cast<const char*>(raw), static_cast<int>(size),
                nullptr, 0);
            std::wstring w(static_cast<size_t>(wlen), L'\0');
            ::MultiByteToWideChar(CP_ACP, 0,
                reinterpret_cast<const char*>(raw), static_cast<int>(size),
                w.data(), wlen);
            int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen,
                nullptr, 0, nullptr, nullptr);
            utf8.resize(static_cast<size_t>(n));
            ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen,
                utf8.data(), n, nullptr, nullptr);
            textData = utf8.data(); textLen = utf8.size();
        }
    }

    // EOL detection (first matching sequence wins).
    Buffer::Eol eol = Buffer::Eol::Crlf;
    for (size_t i = 0; i < textLen; ++i) {
        if (textData[i] == '\r') {
            eol = (i + 1 < textLen && textData[i+1] == '\n')
                ? Buffer::Eol::Crlf : Buffer::Eol::Cr;
            break;
        }
        if (textData[i] == '\n') { eol = Buffer::Eol::Lf; break; }
    }
    b.SetEncoding(enc);
    b.SetEol(eol);

    // Attach the document to the factory view briefly so we can stuff bytes
    // in; the guard restores the previously visible document on return.
    DocSwapGuard swap(*factory_, b.DocHandle());
    factory_->Call(SCI_CLEARALL);
    factory_->Call(SCI_SETCODEPAGE, SC_CP_UTF8);
    switch (eol) {
    case Buffer::Eol::Crlf: factory_->Call(SCI_SETEOLMODE, SC_EOL_CRLF); break;
    case Buffer::Eol::Lf:   factory_->Call(SCI_SETEOLMODE, SC_EOL_LF);   break;
    case Buffer::Eol::Cr:   factory_->Call(SCI_SETEOLMODE, SC_EOL_CR);   break;
    }
    factory_->Call(SCI_SETUNDOCOLLECTION, 0);
    if (textLen > 0) {
        factory_->Call(SCI_ADDTEXT, static_cast<uptr_t>(textLen),
            reinterpret_cast<sptr_t>(textData));
    }
    factory_->Call(SCI_SETUNDOCOLLECTION, 1);
    factory_->Call(SCI_EMPTYUNDOBUFFER);
    factory_->Call(SCI_SETSAVEPOINT);
    factory_->Call(SCI_GOTOPOS, 0);
    b.SetDirty(false);

    // Record mtime for stale-on-disk detection (M7).
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME ft{};
        ::GetFileTime(h, nullptr, nullptr, &ft);
        b.SetLastWriteTime(ft);
        ::CloseHandle(h);
    }
    return true;
}

BufferID BufferManager::OpenFile(const std::wstring& path, std::wstring* errorOut)
{
    if (BufferID existing = FindByPath(path); existing != kInvalidBufferID)
        return existing;

    const sptr_t doc = factory_->Call(SCI_CREATEDOCUMENT, 0,
        static_cast<sptr_t>(SC_DOCUMENTOPTION_DEFAULT));
    const BufferID id = nextId_++;
    auto b = std::make_unique<Buffer>(id, doc, path, false, 0);

    if (!LoadIntoDoc(*b, path, errorOut)) {
        // Release the doc we created.
        factory_->Call(SCI_RELEASEDOCUMENT, 0, doc);
        return kInvalidBufferID;
    }
    buffers_.emplace(id, std::move(b));
    return id;
}

bool BufferManager::WriteFromDoc(Buffer& b, const std::wstring& path, std::wstring* errorOut)
{
    DocSwapGuard swap(*factory_, b.DocHandle());
    const sptr_t len = factory_->Call(SCI_GETLENGTH);
    std::string utf8(static_cast<size_t>(len), '\0');
    if (len > 0) {
        factory_->Call(SCI_GETTEXT,
            static_cast<uptr_t>(len + 1),
            reinterpret_cast<sptr_t>(utf8.data()));
    }

    // Encode to target encoding + optional BOM.
    std::string out;
    switch (b.GetEncoding()) {
    case Buffer::Encoding::Utf8:
        out = std::move(utf8);
        break;
    case Buffer::Encoding::Utf8Bom:
        out.reserve(utf8.size() + 3);
        out.push_back('\xEF'); out.push_back('\xBB'); out.push_back('\xBF');
        out.append(utf8);
        break;
    case Buffer::Encoding::Utf16LeBom: {
        int wn = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
            nullptr, 0);
        std::wstring w(static_cast<size_t>(wn), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
            w.data(), wn);
        out.reserve(2 + w.size() * 2);
        out.push_back('\xFF'); out.push_back('\xFE');
        out.append(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
        break;
    }
    case Buffer::Encoding::Utf16BeBom: {
        int wn = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
            nullptr, 0);
        std::wstring w(static_cast<size_t>(wn), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
            w.data(), wn);
        out.reserve(2 + w.size() * 2);
        out.push_back('\xFE'); out.push_back('\xFF');
        for (wchar_t c : w) {
            out.push_back(static_cast<char>((c >> 8) & 0xFF));
            out.push_back(static_cast<char>(c & 0xFF));
        }
        break;
    }
    case Buffer::Encoding::Ansi: {
        int wn = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
            nullptr, 0);
        std::wstring w(static_cast<size_t>(wn), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
            w.data(), wn);
        int bn = ::WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()),
            nullptr, 0, nullptr, nullptr);
        out.resize(static_cast<size_t>(bn));
        ::WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()),
            out.data(), bn, nullptr, nullptr);
        break;
    }
    }

    if (!WriteFileAtomic(path, out.data(), out.size())) {
        if (errorOut) *errorOut = L"Cannot write file:\n" + path;
        return false;
    }
    factory_->Call(SCI_SETSAVEPOINT);
    b.SetDirty(false);

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME ft{};
        ::GetFileTime(h, nullptr, nullptr, &ft);
        b.SetLastWriteTime(ft);
        ::CloseHandle(h);
    }
    return true;
}

bool BufferManager::SaveBuffer(BufferID id, std::wstring* errorOut)
{
    Buffer* b = Get(id);
    if (!b || b->IsUntitled()) return false;
    return WriteFromDoc(*b, b->Path(), errorOut);
}

bool BufferManager::SaveBufferAs(BufferID id, const std::wstring& newPath,
                                 std::wstring* errorOut)
{
    Buffer* b = Get(id);
    if (!b) return false;
    if (!WriteFromDoc(*b, newPath, errorOut)) return false;
    b->SetPath(newPath);
    return true;
}

void BufferManager::CloseBuffer(BufferID id)
{
    auto it = buffers_.find(id);
    if (it == buffers_.end()) return;
    const sptr_t doc = it->second->DocHandle();
    // Drop styling cache first — the handle may be recycled by a future
    // SCI_CREATEDOCUMENT and must not look "already styled".
    ForgetDocStyle(doc);
    // Scintilla releases the document memory once refcount hits zero.
    factory_->Call(SCI_RELEASEDOCUMENT, 0, doc);
    buffers_.erase(it);
}

} // namespace npp
