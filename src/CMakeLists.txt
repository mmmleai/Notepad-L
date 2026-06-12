set(NPP_SOURCES
    WinMain.cpp
    Notepad_plus/Notepad_plus.cpp
    Notepad_plus/Notepad_plus_Window.cpp
    Notepad_plus/NppBigSwitch.cpp
    Notepad_plus/NppCommands.cpp
    Notepad_plus/NppIO.cpp
    ScintillaComponent/ScintillaEditView.cpp
    ScintillaComponent/Buffer.cpp
    ScintillaComponent/BufferManager.cpp
    ScintillaComponent/FindReplaceDlg.cpp
    WinControls/TabBar/DocTabView.cpp
    WinControls/Docking/DockingManager.cpp
    WinControls/Docking/FunctionListPanel.cpp
    WinControls/Docking/DocMapPanel.cpp
    WinControls/Docking/FolderWorkspacePanel.cpp
    MISC/Common/StringUtil.cpp
    MISC/Common/FileIO.cpp
    Parameters/Parameters.cpp
    Parameters/LangType.cpp
    Parameters/Stylers.cpp
    Diff/LineDiff.cpp
    Diff/TextCompareWindow.cpp
    Diff/FileMapping.cpp
    Diff/HexCompareView.cpp
    Diff/HexCompareWindow.cpp
    Diff/FolderScanner.cpp
    Diff/FolderCompareView.cpp
    Diff/FolderCompareWindow.cpp
)

set(NPP_HEADERS
    Notepad_plus/Notepad_plus.h
    Notepad_plus/Notepad_plus_Window.h
    ScintillaComponent/ScintillaEditView.h
    ScintillaComponent/Buffer.h
    ScintillaComponent/BufferManager.h
    ScintillaComponent/FindReplaceDlg.h
    WinControls/TabBar/DocTabView.h
    WinControls/Docking/DockPanel.h
    WinControls/Docking/DockingManager.h
    WinControls/Docking/FunctionListPanel.h
    WinControls/Docking/DocMapPanel.h
    WinControls/Docking/FolderWorkspacePanel.h
    MISC/Common/StringUtil.h
    MISC/Common/FileIO.h
    Parameters/Parameters.h
    Parameters/LangType.h
    Parameters/Stylers.h
    Diff/LineDiff.h
    Diff/TextCompareWindow.h
    Diff/FileMapping.h
    Diff/HexCompareView.h
    Diff/HexCompareWindow.h
    Diff/FolderScanner.h
    Diff/FolderCompareView.h
    Diff/FolderCompareWindow.h
    resource.h
)

set(NPP_RESOURCES
    ../resources/NotePad-L.rc
)

add_executable(notepadl WIN32
    ${NPP_SOURCES}
    ${NPP_HEADERS}
    ${NPP_RESOURCES})

target_include_directories(notepadl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/resources
)

target_link_libraries(notepadl PRIVATE
    scintilla
    lexilla
    comctl32
    comdlg32
    shell32
    shlwapi
    user32
    gdi32
    advapi32
    ole32
    uuid
    version
    uxtheme
    dwmapi
)

set_target_properties(notepadl PROPERTIES
    OUTPUT_NAME "notepad-l"
    VS_DPI_AWARE "PerMonitor")

# Manifest (common controls v6 + DPI awareness)
target_sources(notepadl PRIVATE ../resources/NotePad-L.manifest)

# --- Install rules ----------------------------------------------------------
install(TARGETS notepadl RUNTIME DESTINATION .)
