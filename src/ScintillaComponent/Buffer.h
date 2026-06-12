#pragma once
#include "../Parameters/LangType.h"
#include <string>
#include <cstdint>
#include <windows.h>

typedef intptr_t sptr_t;

namespace npp {

using BufferID = uint32_t;
constexpr BufferID kInvalidBufferID = 0;

// A Buffer owns a Scintilla document handle. Multiple views can attach the same
// handle via SCI_SETDOCPOINTER; reference counting is performed by Scintilla
// (SCI_ADDREFDOCUMENT / SCI_RELEASEDOCUMENT).
class Buffer
{
public:
    Buffer(BufferID id, sptr_t docHandle, std::wstring path, bool untitled, int untitledIndex);

    BufferID       Id() const           { return id_; }
    sptr_t         DocHandle() const    { return docHandle_; }
    const std::wstring& Path() const    { return path_; }
    bool           IsUntitled() const   { return untitled_; }
    int            UntitledIndex() const{ return untitledIndex_; }

    // Display name: "new N" when untitled, otherwise file name.
    std::wstring   DisplayName() const;

    // Set after a successful save.
    void SetPath(const std::wstring& newPath);

    // Dirty = unsaved edits OR a pending encoding conversion. The encoding
    // bit is sticky across savepoint notifications (undoing back to the
    // savepoint restores the text, not the encoding choice) and is cleared
    // only by a successful save.
    bool IsDirty() const         { return dirty_ || encodingDirty_; }
    void SetDirty(bool d)        { dirty_ = d; }
    void MarkEncodingDirty()     { encodingDirty_ = true; }
    void ClearEncodingDirty()    { encodingDirty_ = false; }

    FILETIME LastWriteTime() const { return lastWrite_; }
    void     SetLastWriteTime(FILETIME t) { lastWrite_ = t; }

    // On-disk encoding / EOL. Encoding controls how bytes map to the
    // SCI_CP_UTF8 internal document on read + serialization on write.
    enum class Encoding {
        Utf8,        // no BOM
        Utf8Bom,
        Utf16LeBom,
        Utf16BeBom,
        Ansi,        // current ACP
    };
    enum class Eol { Crlf, Lf, Cr };

    Encoding GetEncoding() const        { return encoding_; }
    void     SetEncoding(Encoding e)    { encoding_ = e; }
    Eol      GetEol() const             { return eol_; }
    void     SetEol(Eol e)              { eol_ = e; }
    LangType GetLang() const            { return lang_; }
    void     SetLang(LangType l)        { lang_ = l; }

    // Set by SCN_MODIFIED on user edits; the frame's debounce timer runs
    // the whole-document identifier / fence pass only when typing pauses.
    // Also re-runs on activation if we switched away before the timer fired.
    bool StyleDirty() const         { return styleDirty_; }
    void SetStyleDirty(bool d)      { styleDirty_ = d; }

    // Cached view state (updated on deactivate, restored on activate).
    struct ViewState {
        sptr_t firstVisibleLine = 0;
        sptr_t caretPos         = 0;
        sptr_t anchorPos        = 0;
        sptr_t xOffset          = 0;
    };
    ViewState& View()            { return view_; }
    const ViewState& View() const{ return view_; }

private:
    BufferID        id_;
    sptr_t          docHandle_;
    std::wstring    path_;          // empty when untitled
    bool            untitled_;
    int             untitledIndex_; // 1-based "new N"
    bool            dirty_ = false;
    bool            encodingDirty_ = false;
    bool            styleDirty_ = false;
    FILETIME        lastWrite_{};
    ViewState       view_{};
    Encoding        encoding_ = Encoding::Utf8;
    Eol             eol_      = Eol::Crlf;
    LangType        lang_     = LangType::Text;
};

} // namespace npp
