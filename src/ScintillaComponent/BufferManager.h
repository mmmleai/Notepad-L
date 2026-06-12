#pragma once
#include "Buffer.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

namespace npp {

class ScintillaEditView;

// Owns every Buffer in the process. All open/save operations go through here.
class BufferManager
{
public:
    static BufferManager& Instance();

    // Bind the "factory" Scintilla — we use it only to call SCI_CREATEDOCUMENT
    // and SCI_RELEASEDOCUMENT, never to render. Must be called once after the
    // first ScintillaEditView is created.
    void SetFactoryView(ScintillaEditView* v) { factory_ = v; }

    // Create a fresh empty "new N" buffer. Returns non-zero id.
    BufferID NewUntitled();

    // Open a file. If the path is already open, returns that buffer's id.
    // Returns kInvalidBufferID on read failure.
    BufferID OpenFile(const std::wstring& path, std::wstring* errorOut = nullptr);

    // Save the buffer to its current path (must be titled).
    bool SaveBuffer(BufferID id, std::wstring* errorOut = nullptr);

    // Change path and save (Save As). Also clears untitled.
    bool SaveBufferAs(BufferID id, const std::wstring& newPath,
                      std::wstring* errorOut = nullptr);

    // Save caller-provided document content (UTF-8) instead of the document's
    // current text, without touching the document itself. Used by binary
    // (hex) mode, where the live document holds a dump rather than the real
    // content. Sets the savepoint / clears dirty only on success.
    bool SaveBufferBytes(BufferID id, const std::string& utf8,
                         std::wstring* errorOut = nullptr);
    bool SaveBufferBytesAs(BufferID id, const std::wstring& newPath,
                           const std::string& utf8,
                           std::wstring* errorOut = nullptr);

    // Current document text (UTF-8 bytes) of any buffer, attached or not.
    std::string GetDocText(BufferID id);

    // Destroy a buffer (must be detached from all views first).
    void CloseBuffer(BufferID id);

    Buffer* Get(BufferID id);
    const Buffer* Get(BufferID id) const;

    // Lookup by absolute path (case-insensitive on Windows).
    BufferID FindByPath(const std::wstring& path) const;

    // Snapshot of all live ids (iteration order undefined).
    std::vector<BufferID> AllIds() const;

private:
    BufferManager() = default;

    // Read bytes from disk into the buffer's Scintilla document.
    bool LoadIntoDoc(Buffer& b, const std::wstring& path, std::wstring* errorOut);
    // Write the buffer's document contents to disk atomically.
    bool WriteFromDoc(Buffer& b, const std::wstring& path, std::wstring* errorOut);
    // Encode UTF-8 document content to the buffer's on-disk encoding + BOM,
    // then write atomically and record the new mtime.
    bool EncodeAndWrite(Buffer& b, const std::wstring& path,
                        const std::string& utf8, std::wstring* errorOut);

    ScintillaEditView* factory_ = nullptr;
    BufferID           nextId_  = 1;
    int                nextUntitled_ = 1;
    std::unordered_map<BufferID, std::unique_ptr<Buffer>> buffers_;
};

} // namespace npp
