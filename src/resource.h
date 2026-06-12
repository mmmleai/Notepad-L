#pragma once

#define IDI_NOTEPADL                101
#define IDR_MAIN_MENU               102
#define IDR_MAIN_ACCEL              103
#define IDR_TAB_CONTEXT             104

// File
#define IDM_FILE_NEW                40001
#define IDM_FILE_OPEN               40002
#define IDM_FILE_SAVE               40003
#define IDM_FILE_SAVEAS             40004
#define IDM_FILE_SAVEALL            40005
#define IDM_FILE_CLOSE              40006
#define IDM_FILE_CLOSEALL           40007
#define IDM_FILE_PRINT              40008
#define IDM_FILE_EXIT               40009
#define IDM_FILE_RECENT_BASE        40100   // 40100..40119 reserved for MRU
#define IDM_FILE_RECENT_MAX         40119
#define IDM_FILE_RECENT_CLEAR       40120

// Edit
#define IDM_EDIT_UNDO               40201
#define IDM_EDIT_REDO               40202
#define IDM_EDIT_CUT                40203
#define IDM_EDIT_COPY               40204
#define IDM_EDIT_PASTE              40205
#define IDM_EDIT_SELECTALL          40206
#define IDM_EDIT_DELETE             40207
#define IDM_EDIT_DUP_LINE           40208
#define IDM_EDIT_DEL_LINE           40209
#define IDM_EDIT_MOVE_UP            40210
#define IDM_EDIT_MOVE_DOWN          40211
#define IDM_EDIT_SPLIT_LINES        40212
#define IDM_EDIT_JOIN_LINES         40213
#define IDM_EDIT_TRIM_TRAIL         40214
#define IDM_EDIT_UPPERCASE          40215
#define IDM_EDIT_LOWERCASE          40216
#define IDM_EDIT_SORT_ASC           40217
#define IDM_EDIT_SORT_DESC          40218
#define IDM_EDIT_INDENT             40219
#define IDM_EDIT_OUTDENT            40220
#define IDM_EDIT_COLUMN_MODE        40222
#define IDM_EDIT_BINARY_MODE        40223
#define IDM_EDIT_JSON_FORMAT        40224

// Search
#define IDM_SEARCH_FIND             40401
#define IDM_SEARCH_REPLACE          40402
#define IDM_SEARCH_FINDNEXT         40403
#define IDM_SEARCH_FINDPREV         40404
#define IDM_SEARCH_GOTO             40405
#define IDM_SEARCH_BMK_TOGGLE       40410
#define IDM_SEARCH_BMK_NEXT         40411
#define IDM_SEARCH_BMK_PREV         40412
#define IDM_SEARCH_BMK_CLEAR        40413

// Encoding
#define IDM_ENC_UTF8                40501
#define IDM_ENC_UTF8_BOM            40502
#define IDM_ENC_UTF16LE_BOM         40503
#define IDM_ENC_UTF16BE_BOM         40504
#define IDM_ENC_ANSI                40505
#define IDM_ENC_CONVERT_UTF8        40511
#define IDM_ENC_CONVERT_UTF8_BOM    40512
#define IDM_ENC_CONVERT_UTF16LE     40513
#define IDM_ENC_CONVERT_UTF16BE     40514
#define IDM_ENC_CONVERT_ANSI        40515

// Format (EOL)
#define IDM_EOL_CRLF                40601
#define IDM_EOL_LF                  40602
#define IDM_EOL_CR                  40603

// Language submenu (base id; each language offset by LangType index)
#define IDM_LANG_BASE               41000
#define IDM_LANG_MAX                41200

// Tab context menu / extra close commands
#define IDM_FILE_CLOSEBUT           40010   // Close All But Active
#define IDM_FILE_CLOSETOLEFT        40011
#define IDM_FILE_CLOSETORIGHT       40012
#define IDM_TAB_COPYPATH            40013
#define IDM_TAB_OPENFOLDER          40014

// Window navigation
#define IDM_WIN_NEXT                40301
#define IDM_WIN_PREV                40302

// View / Docking panels
#define IDM_VIEW_FOLDER_WORKSPACE   40701
#define IDM_VIEW_FUNCTION_LIST      40702
#define IDM_VIEW_DOC_MAP            40703
#define IDM_VIEW_FOLDER_OPEN        40710   // open folder in workspace
#define IDM_VIEW_WORD_WRAP          40720   // toggle line wrapping

// Tools — Compare
#define IDM_TOOL_COMPARE_TEXT       40882   // Beyond-Compare-style text compare dialog
#define IDM_TOOL_COMPARE_HEX        40883   // Hex compare
#define IDM_TOOL_COMPARE_FOLDER     40884   // Folder compare
#define IDM_TOOL_COMPARE_NEXT_DIFF  40885
#define IDM_TOOL_COMPARE_PREV_DIFF  40886

// Hex Compare dialog
#define IDD_HEX_COMPARE             220
#define IDC_HEXCMP_LEFT_PATH        2201
#define IDC_HEXCMP_LEFT_BROWSE      2202
#define IDC_HEXCMP_RIGHT_PATH       2203
#define IDC_HEXCMP_RIGHT_BROWSE     2204
#define IDC_HEXCMP_LEFT_VIEW        2205
#define IDC_HEXCMP_RIGHT_VIEW       2206
#define IDC_HEXCMP_STATUS           2207
#define IDC_HEXCMP_NEXT             2208
#define IDC_HEXCMP_PREV             2209

// Folder Compare dialog
#define IDD_FOLDER_COMPARE          221
#define IDC_FCMP_LEFT_PATH          2211
#define IDC_FCMP_LEFT_BROWSE        2212
#define IDC_FCMP_RIGHT_PATH         2213
#define IDC_FCMP_RIGHT_BROWSE       2214
#define IDC_FCMP_RECURSIVE          2215
#define IDC_FCMP_FILTERS            2216
#define IDC_FCMP_COMPARE_BTN        2217
#define IDC_FCMP_LIST               2218
#define IDC_FCMP_STATUS             2219

// Text Compare picker dialog (pick two files from disk)
#define IDD_TEXT_COMPARE_PICK       222
#define IDC_TXCMP_LEFT_PATH         2221
#define IDC_TXCMP_LEFT_BROWSE       2222
#define IDC_TXCMP_RIGHT_PATH        2223
#define IDC_TXCMP_RIGHT_BROWSE      2224
#define IDC_TXCMP_IGNORE_WS         2225
#define IDC_TXCMP_IGNORE_CASE       2226

// Settings
#define IDM_VIEW_DARKMODE           40800
// Theme picker: View → Theme → (6 items). Keep contiguous so we can check-radio them.
#define IDM_VIEW_THEME_BASE         40810
#define IDM_VIEW_THEME_LIGHT        40810  // ModernLight
#define IDM_VIEW_THEME_DARK         40811  // DarkPro
#define IDM_VIEW_THEME_HIGHCONTRAST 40812
#define IDM_VIEW_THEME_MINT         40813
#define IDM_VIEW_THEME_NORDIC       40814
#define IDM_VIEW_THEME_DEEPBLUE     40815
#define IDM_VIEW_THEME_LAST         40815

// Help
#define IDM_HELP_ABOUT              40901
