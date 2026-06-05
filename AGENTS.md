# NotePad-L

## Build environment

- Visual Studio 2022 Professional is installed at: `F:\Microsoft Visual Studio\2022\Professional`
  - Use this path for `vcvarsall.bat`, `devenv.exe`, `msbuild.exe`, etc.
  - Example: `"F:\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64`
- GitHub CLI (`gh.exe`) is at: `F:\GitHub CLI\gh.exe` (not on PATH — invoke with the full path).

## Architecture

Win32 GUI app built on Scintilla/Lexilla (static-linked). One frame window hosts
the app core, which owns up to two view slots; each slot = one Scintilla view +
one tab bar. All documents are owned by a single `BufferManager`.

```
            ┌─────────────────── WinMain.cpp ────────────────────┐
            │ single-instance mutex · CommandLine → initialFiles │
            │ existing instance? forward via WM_COPYDATA         │
            └──────────────────────────┬─────────────────────────┘
                                       ▼
                       ┌──────── Notepad_plus_Window ────────┐
                       │ frame HWND · menu · toolbar · status│
                       │ WndProc: WM_DROPFILES / WM_COPYDATA │
                       │ WM_NOTIFY · accelerators · theming  │
                       └──────────────────┬──────────────────┘
                                          ▼
        ┌─────────────────────── Notepad_plus ───────────────────────┐
        │ app core · commands (Do{New,Open,Save,Close,Find,…})       │
        │ ViewSlot views_[2] = { ScintillaEditView + DocTabView }    │
        │ ActivateBuffer · StashViewState/RestoreViewState           │
        │ binary-mode · column-edit · JSON format                    │
        └──┬──────────────┬──────────────────┬─────────────┬─────────┘
           │              │                  │             │
           ▼              ▼                  ▼             ▼
  ┌────────────────┐  ┌──────────┐  ┌─────────────────┐  ┌──────────┐
  │ ScintillaComp. │  │Parameters│  │  WinControls    │  │   Diff   │
  │                │  │          │  │                 │  │          │
  │ Buffer         │  │ Parameters│ │ TabBar/DocTab   │  │ Text /   │
  │ BufferManager  │  │ (singleton)│ │ Docking/       │  │ Hex /    │
  │   (singleton)  │  │ LangType │  │   DockingManager│  │ Folder   │
  │ ScintillaEdit  │  │ Stylers  │  │   FindResults   │  │ Compare  │
  │   View         │  │ (ApplyLang│ │   FunctionList  │  │ Windows  │
  │ FindReplaceDlg │  │  palettes)│ │   DocMap        │  │          │
  │ FindInFilesDlg │  │          │  │   FolderWkspc   │  │ LineDiff │
  └───────┬────────┘  └────┬─────┘  └────────┬────────┘  │ FileMap  │
          │                │                 │           │ FolderScan│
          │                │                 │           └──────────┘
          ▼                ▼                 ▼
  ┌────────────────────────────────────────────────────────┐
  │ scintilla/ · lexilla/ (static libs, git submodules)    │
  │ MISC/Common: FileIO (read-all / atomic write) · StrUtil│
  └────────────────────────────────────────────────────────┘
```

### Key runtime flows

- **Open file** — `WinMain` / `WM_DROPFILES` / `WM_COPYDATA`
  → `Notepad_plus::DoOpen`
  → `BufferManager::OpenFile` (creates Scintilla doc, reads bytes via
    `FileIO::ReadFileAll`, BOM/UTF-8 sniff, SCI_ADDTEXT on hidden factory view)
  → `OpenBufferInTab` → `DocTabView::AddTab(activate=true)`
  → tab notify → `ActivateBuffer` → `editor.AttachDocument` →
    `RefreshEditorForActiveBuffer` → `Stylers::ApplyLanguage`
    (lexer + COLOURISE + `HighlightFunctionNames`, wrapped in `WM_SETREDRAW`
     + temporary wrap-off for batched paint).

- **Tab switch** — `DocTabView` TCN_SELCHANGE → `ActivateBuffer`
  → `StashViewState(old)` · `AttachDocument(new)` · `ApplyLanguage` (fast
     path when same lang: skips `SCI_SETILEXER` / COLOURISE) ·
     `RestoreViewState(new)` (SETSEL before SETFIRSTVISIBLELINE so the
     caret-into-view scroll doesn't clobber topLine).

- **Theme change** — `Notepad_plus_Window::ApplyCurrentTheme`
  → per-view `ApplyLanguage` (re-runs palette/style setup only; existing
    style bytes re-render with new mappings).

### Conventions

- `namespace npp` everywhere; no global state outside singletons
  (`Parameters`, `BufferManager`).
- Scintilla is used via `ScintillaEditView::Call(msg, wParam, lParam)` —
  equivalent to `SendMessage` but via the direct function pointer.
- Document bytes inside Scintilla are always UTF-8 (`SCI_SETCODEPAGE =
  SC_CP_UTF8`); the `Buffer::Encoding` enum tracks on-disk encoding and is
  re-encoded on save.
- `src/Macro/` (MacroRecorder) is present but not wired into the app.
