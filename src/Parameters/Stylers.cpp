#include "Stylers.h"
#include "Parameters.h"

#include <Scintilla.h>
#include <ScintillaMessages.h>
#include <SciLexer.h>
#include <ILexer.h>
#include <Lexilla.h>

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace npp {

// ---- Shared UI palette ----------------------------------------------------
namespace {
    constexpr int kThemeCount = static_cast<int>(ThemeId::Count_);

    constexpr UiPalette kUiPalettes[kThemeCount] = {
        // 0  ModernLight (现代清爽 浅色)
        { /*chromeBg*/   RGB(0xEC,0xEE,0xF2),
          /*editorBg*/   RGB(0xFA,0xFA,0xFA),
          /*statusBg*/   RGB(0xE3,0xE5,0xE9),
          /*border*/     RGB(0xD8,0xDA,0xDE),
          /*text*/       RGB(0x1E,0x1E,0x1E),
          /*textMuted*/  RGB(0x6B,0x71,0x80),
          /*accent*/     RGB(0x2A,0x6D,0xF4),
          /*accentDim*/  RGB(0x1D,0x5B,0xD6),
          /*dirty*/      RGB(0xE5,0x48,0x4D),
          /*caretLine*/  RGB(0xF0,0xF2,0xF5),
          /*selection*/  RGB(0xAD,0xD6,0xFF),
          /*hotBg*/      RGB(0xE0,0xE4,0xEC) },
        // 1  DarkPro (VS Code Dark+ — modern dark chrome, no signature blue strip)
        { /*chromeBg*/  RGB(0x25,0x25,0x26),   // sidebar / activity bar
          /*editorBg*/  RGB(0x1E,0x1E,0x1E),   // editor surface
          /*statusBg*/  RGB(0x18,0x18,0x18),   // matte dark, blends with chrome
          /*border*/    RGB(0x1E,0x1E,0x1E),
          /*text*/      RGB(0xE6,0xE6,0xE6),
          /*textMuted*/ RGB(0xA0,0xA0,0xA0),
          /*accent*/    RGB(0x00,0x7A,0xCC),
          /*accentDim*/ RGB(0x00,0x5A,0x9E),
          /*dirty*/     RGB(0xF4,0x87,0x71),
          /*caretLine*/ RGB(0x2A,0x2D,0x2E),
          /*selection*/ RGB(0x26,0x4F,0x78),
          /*hotBg*/     RGB(0x2A,0x2D,0x2E) },
        // 2  HighContrast (简洁高对比)
        { RGB(0xF4,0xF4,0xF4), RGB(0xFF,0xFF,0xFF), RGB(0xE8,0xE8,0xE8),
          RGB(0x90,0x90,0x90), RGB(0x00,0x00,0x00), RGB(0x44,0x44,0x44),
          RGB(0x00,0x57,0xB7), RGB(0x00,0x40,0x90), RGB(0xC8,0x1D,0x25),
          RGB(0xEE,0xF3,0xFA), RGB(0x99,0xC7,0xFF), RGB(0xDC,0xE4,0xEE) },
        // 3  Mint (柔和护眼 墨绿)
        { RGB(0xD6,0xE3,0xC9), RGB(0xE4,0xEF,0xDB), RGB(0xCB,0xDB,0xB8),
          RGB(0xA8,0xBE,0x92), RGB(0x27,0x4E,0x37), RGB(0x5E,0x7B,0x5D),
          RGB(0x3B,0x8E,0x5A), RGB(0x2A,0x6E,0x42), RGB(0xB3,0x50,0x3A),
          RGB(0xDC,0xE8,0xC4), RGB(0xBE,0xD6,0x94), RGB(0xC9,0xD8,0xAE) },
        // 4  Nordic (北欧极简 淡灰冷色)
        { RGB(0xE2,0xE6,0xED), RGB(0xEC,0xEF,0xF4), RGB(0xD8,0xDE,0xE9),
          RGB(0xB5,0xBE,0xCC), RGB(0x2E,0x34,0x40), RGB(0x61,0x6E,0x7E),
          RGB(0x5E,0x81,0xAC), RGB(0x46,0x6A,0x94), RGB(0xBF,0x61,0x6A),
          RGB(0xE5,0xEA,0xF0), RGB(0xBF,0xD3,0xE8), RGB(0xD8,0xDE,0xE9) },
        // 5  DeepBlue (代码风格 深蓝) — navy chrome around a VS Code Dark+
        //     editor; status bar stays in the navy family, no bright-blue strip.
        { /*chromeBg*/  RGB(0x11,0x20,0x36),
          /*editorBg*/  RGB(0x1E,0x1E,0x1E),
          /*statusBg*/  RGB(0x0C,0x18,0x28),
          /*border*/    RGB(0x22,0x36,0x54),
          /*text*/      RGB(0xE6,0xE6,0xE6),
          /*textMuted*/ RGB(0xA0,0xA0,0xA0),
          /*accent*/    RGB(0x4E,0xC9,0xB0),
          /*accentDim*/ RGB(0x35,0xA6,0x92),
          /*dirty*/     RGB(0xF4,0x87,0x71),
          /*caretLine*/ RGB(0x2A,0x2D,0x2E),
          /*selection*/ RGB(0x26,0x4F,0x78),
          /*hotBg*/     RGB(0x1D,0x31,0x4F) },
    };

    constexpr IconPalette kIconPalettes[kThemeCount] = {
        // 0  ModernLight: ink gray + sky-blue accent, white fill
        { /*bg*/ RGB(0xFA,0xF7,0xF5), RGB(0x9A,0xA4,0xB0), RGB(0x6E,0xB4,0xEE),
          RGB(0x4C,0x9A,0xD9), RGB(0xC8,0xCE,0xD6), RGB(0xFF,0xFF,0xFF),
          RGB(0xE0,0x8A,0x8A) },
        // 1  DarkPro (VS Code Dark+): icon bg matches sidebar
        { RGB(0x25,0x25,0x26), RGB(0x9A,0xA4,0xB0), RGB(0x00,0x7A,0xCC),
          RGB(0x00,0x5A,0x9E), RGB(0x5C,0x63,0x70), RGB(0xE8,0xEB,0xF0),
          RGB(0xF4,0x87,0x71) },
        // 2  HighContrast: strong black ink on white
        { RGB(0xF4,0xF4,0xF4), RGB(0x20,0x20,0x20), RGB(0x00,0x57,0xB7),
          RGB(0x00,0x40,0x90), RGB(0x70,0x70,0x70), RGB(0xFF,0xFF,0xFF),
          RGB(0xC8,0x1D,0x25) },
        // 3  Mint: dark green ink on sage background
        { RGB(0xD6,0xE3,0xC9), RGB(0x4E,0x6A,0x4F), RGB(0x3B,0x8E,0x5A),
          RGB(0x2A,0x6E,0x42), RGB(0x95,0xA9,0x88), RGB(0xE4,0xEF,0xDB),
          RGB(0xB3,0x50,0x3A) },
        // 4  Nordic: slate ink + frost-blue accent
        { RGB(0xE2,0xE6,0xED), RGB(0x4C,0x56,0x6A), RGB(0x5E,0x81,0xAC),
          RGB(0x46,0x6A,0x94), RGB(0x9C,0xA8,0xBB), RGB(0xEC,0xEF,0xF4),
          RGB(0xBF,0x61,0x6A) },
        // 5  DeepBlue: teal-accent ink on deep blue (bg matches chromeBg).
        { RGB(0x11,0x20,0x36), RGB(0x8A,0x99,0xAE), RGB(0x4E,0xC9,0xB0),
          RGB(0x35,0xA6,0x92), RGB(0x4A,0x55,0x68), RGB(0xCD,0xD6,0xE4),
          RGB(0xF0,0x7B,0x8A) },
    };

    inline int ThemeIndex(ThemeId t) {
        int i = static_cast<int>(t);
        if (i < 0 || i >= kThemeCount) i = 0;
        return i;
    }
}

const UiPalette& Ui(ThemeId t) { return kUiPalettes[ThemeIndex(t)]; }
const UiPalette& Ui() { return Ui(Parameters::Instance().Theme()); }

const IconPalette& Icons(ThemeId t) { return kIconPalettes[ThemeIndex(t)]; }
const IconPalette& Icons() { return Icons(Parameters::Instance().Theme()); }

// ---- Per-theme syntax palette --------------------------------------------
namespace {

struct SyntaxPalette {
    COLORREF fg;         // default text
    COLORREF bg;         // editor background (must match UiPalette.editorBg)
    COLORREF comment;
    COLORREF number;
    COLORREF string;
    COLORREF character;
    COLORREF keyword;
    COLORREF keyword2;   // secondary keyword / type
    COLORREF preproc;
    COLORREF op;
    COLORREF identifier;
    COLORREF tagName;
    COLORREF attrName;
    COLORREF func;       // function-name identifiers (post-lex heuristic)
    COLORREF caretLine;
    COLORREF margin;
    COLORREF marginFg;
};

constexpr SyntaxPalette kSyntax[kThemeCount] = {
    // 0 ModernLight
    { RGB(0x00,0x00,0x00), RGB(0xFA,0xFA,0xFA),
      RGB(0x00,0x80,0x00), RGB(0xFF,0x80,0x00),
      RGB(0x80,0x80,0x80), RGB(0x80,0x80,0x80),
      RGB(0x00,0x00,0xFF), RGB(0x80,0x00,0x80),
      RGB(0x80,0x40,0x20), RGB(0x00,0x00,0x80),
      RGB(0x00,0x00,0x00), RGB(0x80,0x00,0x00),
      RGB(0xFF,0x00,0x00),
      /*func*/ RGB(0x79,0x5E,0x26),
      RGB(0xF0,0xF2,0xF5), RGB(0xF2,0xF3,0xF6), RGB(0x98,0xA0,0xAE) },
    // 1 DarkPro — VS Code Dark+ tone, but slightly brighter / more saturated
    //            so contrast pops on the #1E1E1E surface.
    { /*fg*/        RGB(0xE6,0xE6,0xE6), /*bg*/        RGB(0x1E,0x1E,0x1E),
      /*comment*/   RGB(0x7E,0xB6,0x65), /*number*/    RGB(0xC8,0xE0,0xB6),
      /*string*/    RGB(0xE6,0xA1,0x7E), /*character*/ RGB(0xE6,0xA1,0x7E),
      /*keyword*/   RGB(0x69,0xB3,0xEF), /*keyword2*/  RGB(0xDC,0x95,0xD7),
      /*preproc*/   RGB(0xDC,0x95,0xD7), /*op*/        RGB(0xE6,0xE6,0xE6),
      /*identifier*/RGB(0xB3,0xE2,0xFF), /*tagName*/   RGB(0x69,0xB3,0xEF),
      /*attrName*/  RGB(0xB3,0xE2,0xFF), /*func*/      RGB(0xEF,0xEC,0xA8),
      /*caretLine*/ RGB(0x2A,0x2D,0x2E),
      /*margin*/    RGB(0x1E,0x1E,0x1E), /*marginFg*/  RGB(0xA0,0xA0,0xA0) },
    // 2 HighContrast
    { RGB(0x00,0x00,0x00), RGB(0xFF,0xFF,0xFF),
      RGB(0x00,0x66,0x00), RGB(0xB0,0x45,0x00),
      RGB(0x85,0x15,0x85), RGB(0x85,0x15,0x85),
      RGB(0x00,0x00,0xC8), RGB(0x80,0x00,0x80),
      RGB(0x80,0x20,0x00), RGB(0x00,0x00,0x80),
      RGB(0x00,0x00,0x00), RGB(0x80,0x00,0x00),
      RGB(0xC0,0x00,0x00),
      /*func*/ RGB(0x5C,0x2D,0x00),
      RGB(0xEE,0xF3,0xFA), RGB(0xEE,0xEE,0xEE), RGB(0x66,0x66,0x66) },
    // 3 Mint
    { RGB(0x27,0x4E,0x37), RGB(0xE4,0xEF,0xDB),
      RGB(0x6F,0x8C,0x5A), RGB(0xB0,0x6A,0x2A),
      RGB(0x48,0x72,0x5A), RGB(0x48,0x72,0x5A),
      RGB(0x2B,0x63,0x90), RGB(0x75,0x3E,0x8C),
      RGB(0x8B,0x4A,0x2C), RGB(0x3F,0x67,0x52),
      RGB(0x27,0x4E,0x37), RGB(0x6C,0x35,0x76),
      RGB(0xB0,0x43,0x2E),
      /*func*/ RGB(0x4A,0x78,0x38),
      RGB(0xDC,0xE8,0xC4), RGB(0xDC,0xE8,0xCA), RGB(0x8A,0xA0,0x7E) },
    // 4 Nordic
    { RGB(0x2E,0x34,0x40), RGB(0xEC,0xEF,0xF4),
      RGB(0x81,0x8F,0x9E), RGB(0xB4,0x8E,0x4D),
      RGB(0xA3,0xBE,0x8C), RGB(0xA3,0xBE,0x8C),
      RGB(0x5E,0x81,0xAC), RGB(0xB4,0x8E,0xAD),
      RGB(0xD0,0x87,0x70), RGB(0x81,0xA1,0xC1),
      RGB(0x2E,0x34,0x40), RGB(0xBF,0x61,0x6A),
      RGB(0xEB,0xCB,0x8B),
      /*func*/ RGB(0xB4,0x8E,0xAD),
      RGB(0xE5,0xEA,0xF0), RGB(0xE2,0xE6,0xED), RGB(0x8D,0x99,0xAC) },
    // 5 DeepBlue — same brighter syntax palette as DarkPro on #1E1E1E editor.
    { /*fg*/        RGB(0xE6,0xE6,0xE6), /*bg*/        RGB(0x1E,0x1E,0x1E),
      /*comment*/   RGB(0x7E,0xB6,0x65), /*number*/    RGB(0xC8,0xE0,0xB6),
      /*string*/    RGB(0xE6,0xA1,0x7E), /*character*/ RGB(0xE6,0xA1,0x7E),
      /*keyword*/   RGB(0x69,0xB3,0xEF), /*keyword2*/  RGB(0xDC,0x95,0xD7),
      /*preproc*/   RGB(0xDC,0x95,0xD7), /*op*/        RGB(0xE6,0xE6,0xE6),
      /*identifier*/RGB(0xB3,0xE2,0xFF), /*tagName*/   RGB(0x69,0xB3,0xEF),
      /*attrName*/  RGB(0xB3,0xE2,0xFF), /*func*/      RGB(0xEF,0xEC,0xA8),
      /*caretLine*/ RGB(0x2A,0x2D,0x2E),
      /*margin*/    RGB(0x1E,0x1E,0x1E), /*marginFg*/  RGB(0xA0,0xA0,0xA0) },
};

inline const SyntaxPalette& Syntax(ThemeId t) { return kSyntax[ThemeIndex(t)]; }

// Active palette — set by ApplyLanguage before use.
static SyntaxPalette sP;

static void SelectPalette(ThemeId t) { sP = Syntax(t); }

void SetStyle(ScintillaEditView& v, int style, COLORREF fg, COLORREF bg)
{
    v.Call(SCI_STYLESETFORE, static_cast<uptr_t>(style), static_cast<sptr_t>(fg));
    v.Call(SCI_STYLESETBACK, static_cast<uptr_t>(style), static_cast<sptr_t>(bg));
}
void SetStyle(ScintillaEditView& v, int style, COLORREF fg)
{
    SetStyle(v, style, fg, sP.bg);
}

void ResetStyles(ScintillaEditView& v)
{
    // Note: deliberately *no* SCI_CLEARDOCUMENTSTYLE here. Wiping per-byte
    // style data on every tab activation forced a full SCI_COLOURISE pass and
    // showed unstyled text mid-switch. Style->color mappings live on the view
    // and are rewritten below; doc style bytes survive the activation and
    // render with the updated colors immediately.
    v.Call(SCI_STYLESETFONT, STYLE_DEFAULT,
        reinterpret_cast<sptr_t>("Cascadia Mono"));
    v.Call(SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
    SetStyle(v, STYLE_DEFAULT, sP.fg, sP.bg);
    v.Call(SCI_STYLECLEARALL);

    v.Call(SCI_SETCARETLINEBACK, static_cast<uptr_t>(sP.caretLine));
    v.Call(SCI_SETCARETLINEVISIBLE, 1);
    v.Call(SCI_SETCARETFORE, static_cast<uptr_t>(sP.fg));

    v.Call(SCI_STYLESETFORE, STYLE_LINENUMBER, static_cast<sptr_t>(sP.marginFg));
    v.Call(SCI_STYLESETBACK, STYLE_LINENUMBER, static_cast<sptr_t>(sP.margin));

    v.Call(SCI_SETCARETLINEFRAME, 0);
    v.Call(SCI_SETCARETWIDTH, 2);

    ThemeId tid = Parameters::Instance().Theme();
    const UiPalette& u = Ui(tid);
    v.Call(SCI_SETSELBACK, 1, static_cast<sptr_t>(u.selection));
    v.Call(SCI_SETSELFORE, 0, 0);

    // Bracket matching uses accent fore + underline/bold.
    v.Call(SCI_STYLESETFORE, STYLE_BRACELIGHT, static_cast<sptr_t>(u.accent));
    v.Call(SCI_STYLESETBACK, STYLE_BRACELIGHT, static_cast<sptr_t>(sP.bg));
    v.Call(SCI_STYLESETBOLD, STYLE_BRACELIGHT, 1);
    v.Call(SCI_STYLESETUNDERLINE, STYLE_BRACELIGHT, 1);
    v.Call(SCI_STYLESETFORE, STYLE_BRACEBAD, static_cast<sptr_t>(u.dirty));
    v.Call(SCI_STYLESETBOLD, STYLE_BRACEBAD, 1);

    v.Call(SCI_SETWHITESPACEFORE, 1, static_cast<sptr_t>(u.textMuted));

    v.Call(SCI_SETFOLDMARGINCOLOUR, 1, static_cast<sptr_t>(sP.bg));
    v.Call(SCI_SETFOLDMARGINHICOLOUR, 1, static_cast<sptr_t>(sP.bg));
}

void ApplyKeywords(ScintillaEditView& v, int keywordSet, const char* words)
{
    v.Call(SCI_SETKEYWORDS, static_cast<uptr_t>(keywordSet),
        reinterpret_cast<sptr_t>(words));
}

void StyleCpp(ScintillaEditView& v)
{
    SetStyle(v, SCE_C_COMMENT,       sP.comment);
    SetStyle(v, SCE_C_COMMENTLINE,   sP.comment);
    SetStyle(v, SCE_C_COMMENTDOC,    sP.comment);
    SetStyle(v, SCE_C_COMMENTLINEDOC,sP.comment);
    SetStyle(v, SCE_C_NUMBER,        sP.number);
    SetStyle(v, SCE_C_WORD,          sP.keyword);
    SetStyle(v, SCE_C_WORD2,         sP.keyword2);
    SetStyle(v, SCE_C_STRING,        sP.string);
    SetStyle(v, SCE_C_CHARACTER,     sP.character);
    SetStyle(v, SCE_C_OPERATOR,      sP.op);
    SetStyle(v, SCE_C_IDENTIFIER,    sP.identifier);
    SetStyle(v, SCE_C_PREPROCESSOR,  sP.preproc);

    // VSCode-style split: storage/types/modifiers in set 0 (blue),
    // control-flow keywords in set 1 (pink/magenta).
    ApplyKeywords(v, 0,
        "alignas alignof and and_eq asm auto bitand bitor bool char char8_t "
        "char16_t char32_t class compl concept const consteval constexpr "
        "constinit const_cast decltype delete double dynamic_cast enum "
        "explicit export extern false float friend inline int long mutable "
        "namespace new noexcept not not_eq nullptr operator or or_eq private "
        "protected public register reinterpret_cast requires short signed "
        "sizeof static static_assert static_cast struct template this "
        "thread_local true typedef typeid typename union unsigned using "
        "virtual void volatile wchar_t xor xor_eq");
    ApplyKeywords(v, 1,
        "break case catch continue co_await co_return co_yield default do "
        "else for goto if return switch throw try while");
}

void StylePython(ScintillaEditView& v)
{
    SetStyle(v, SCE_P_COMMENTLINE,  sP.comment);
    SetStyle(v, SCE_P_COMMENTBLOCK, sP.comment);
    SetStyle(v, SCE_P_NUMBER,       sP.number);
    SetStyle(v, SCE_P_WORD,         sP.keyword);
    SetStyle(v, SCE_P_STRING,       sP.string);
    SetStyle(v, SCE_P_CHARACTER,    sP.character);
    SetStyle(v, SCE_P_OPERATOR,     sP.op);
    SetStyle(v, SCE_P_IDENTIFIER,   sP.identifier);
    SetStyle(v, SCE_P_TRIPLE,       sP.string);
    SetStyle(v, SCE_P_TRIPLEDOUBLE, sP.string);
    SetStyle(v, SCE_P_DEFNAME,      sP.keyword2);
    SetStyle(v, SCE_P_CLASSNAME,    sP.keyword2);

    ApplyKeywords(v, 0,
        "False None True and as assert async await break class continue def del "
        "elif else except finally for from global if import in is lambda "
        "nonlocal not or pass raise return try while with yield match case");
}

void StyleHtml(ScintillaEditView& v)
{
    SetStyle(v, SCE_H_TAG,          sP.tagName);
    SetStyle(v, SCE_H_TAGUNKNOWN,   sP.tagName);
    SetStyle(v, SCE_H_ATTRIBUTE,    sP.attrName);
    SetStyle(v, SCE_H_DOUBLESTRING, sP.string);
    SetStyle(v, SCE_H_SINGLESTRING, sP.string);
    SetStyle(v, SCE_H_COMMENT,      sP.comment);
    SetStyle(v, SCE_H_NUMBER,       sP.number);
    SetStyle(v, SCE_H_ENTITY,       sP.keyword2);
    SetStyle(v, SCE_H_TAGEND,       sP.op);
    SetStyle(v, SCE_H_XMLSTART,     sP.tagName);
    SetStyle(v, SCE_H_XMLEND,       sP.tagName);
}

void StyleCss(ScintillaEditView& v)
{
    SetStyle(v, SCE_CSS_COMMENT,          sP.comment);
    SetStyle(v, SCE_CSS_TAG,              sP.tagName);
    SetStyle(v, SCE_CSS_CLASS,            sP.keyword2);
    SetStyle(v, SCE_CSS_PSEUDOCLASS,      sP.keyword2);
    SetStyle(v, SCE_CSS_IDENTIFIER,       sP.keyword);
    SetStyle(v, SCE_CSS_DOUBLESTRING,     sP.string);
    SetStyle(v, SCE_CSS_SINGLESTRING,     sP.string);
    SetStyle(v, SCE_CSS_OPERATOR,         sP.op);
    SetStyle(v, SCE_CSS_VALUE,            sP.identifier);
    SetStyle(v, SCE_CSS_IMPORTANT,        sP.keyword);
}

void StyleJson(ScintillaEditView& v)
{
    SetStyle(v, SCE_JSON_NUMBER,          sP.number);
    SetStyle(v, SCE_JSON_STRING,          sP.string);
    SetStyle(v, SCE_JSON_PROPERTYNAME,    sP.keyword2);
    SetStyle(v, SCE_JSON_KEYWORD,         sP.keyword);
    SetStyle(v, SCE_JSON_LINECOMMENT,     sP.comment);
    SetStyle(v, SCE_JSON_BLOCKCOMMENT,    sP.comment);
    SetStyle(v, SCE_JSON_OPERATOR,        sP.op);
}

void StyleXml(ScintillaEditView& v)
{
    StyleHtml(v);
}

void StyleBash(ScintillaEditView& v)
{
    SetStyle(v, SCE_SH_COMMENTLINE, sP.comment);
    SetStyle(v, SCE_SH_NUMBER,      sP.number);
    SetStyle(v, SCE_SH_WORD,        sP.keyword);
    SetStyle(v, SCE_SH_STRING,      sP.string);
    SetStyle(v, SCE_SH_CHARACTER,   sP.character);
    SetStyle(v, SCE_SH_OPERATOR,    sP.op);
    SetStyle(v, SCE_SH_IDENTIFIER,  sP.identifier);
    SetStyle(v, SCE_SH_SCALAR,      sP.keyword2);

    ApplyKeywords(v, 0,
        "if then else elif fi case esac for while until do done in function "
        "return break continue exit local readonly export declare typeset "
        "select time eval exec set unset shift trap source");
}

void StyleMarkdown(ScintillaEditView& v)
{
    ThemeId tid = Parameters::Instance().Theme();
    bool isDark = IsDarkFamily(tid);

    constexpr int hStyles[6] = {
        SCE_MARKDOWN_HEADER1, SCE_MARKDOWN_HEADER2, SCE_MARKDOWN_HEADER3,
        SCE_MARKDOWN_HEADER4, SCE_MARKDOWN_HEADER5, SCE_MARKDOWN_HEADER6,
    };
    // Per-theme header color ramps. Headers always stay saturated so
    // structure reads even against muted backgrounds.
    COLORREF hColors[6];
    switch (tid) {
    case ThemeId::DarkPro:
        // VS Code Dark+ token-color ramp.
        hColors[0] = RGB(0x56,0x9C,0xD6); hColors[1] = RGB(0x4E,0xC9,0xB0);
        hColors[2] = RGB(0xDC,0xDC,0xAA); hColors[3] = RGB(0xCE,0x91,0x78);
        hColors[4] = RGB(0xC5,0x86,0xC0); hColors[5] = RGB(0x9C,0xDC,0xFE);
        break;
    case ThemeId::DeepBlue:
        hColors[0] = RGB(0x4E,0xC9,0xB0); hColors[1] = RGB(0x56,0x9C,0xD6);
        hColors[2] = RGB(0x9C,0xDC,0xFE); hColors[3] = RGB(0xCE,0x91,0x78);
        hColors[4] = RGB(0xC5,0x86,0xC0); hColors[5] = RGB(0xDC,0xDC,0xAA);
        break;
    case ThemeId::Mint:
        hColors[0] = RGB(0x1E,0x5E,0x3A); hColors[1] = RGB(0x35,0x72,0x48);
        hColors[2] = RGB(0x4E,0x87,0x56); hColors[3] = RGB(0x66,0x9B,0x62);
        hColors[4] = RGB(0x7B,0xAF,0x6F); hColors[5] = RGB(0x8F,0xBF,0x7C);
        break;
    case ThemeId::Nordic:
        hColors[0] = RGB(0xBF,0x61,0x6A); hColors[1] = RGB(0xD0,0x87,0x70);
        hColors[2] = RGB(0xEB,0xCB,0x8B); hColors[3] = RGB(0xA3,0xBE,0x8C);
        hColors[4] = RGB(0x5E,0x81,0xAC); hColors[5] = RGB(0xB4,0x8E,0xAD);
        break;
    case ThemeId::HighContrast:
        hColors[0] = RGB(0xA0,0x00,0x00); hColors[1] = RGB(0x80,0x2A,0x00);
        hColors[2] = RGB(0x60,0x55,0x00); hColors[3] = RGB(0x00,0x60,0x20);
        hColors[4] = RGB(0x00,0x40,0x90); hColors[5] = RGB(0x5A,0x00,0x7A);
        break;
    case ThemeId::ModernLight:
    default:
        hColors[0] = RGB(0xC9,0x4A,0x1A); hColors[1] = RGB(0xC9,0x6B,0x2C);
        hColors[2] = RGB(0xB5,0x6E,0x2E); hColors[3] = RGB(0x8E,0x5E,0x2E);
        hColors[4] = RGB(0x6E,0x4E,0x2E); hColors[5] = RGB(0x55,0x40,0x28);
        break;
    }
    for (int i = 0; i < 6; ++i) {
        SetStyle(v, hStyles[i], hColors[i]);
        v.Call(SCI_STYLESETBOLD, hStyles[i], 1);
        v.Call(SCI_STYLESETSIZE, hStyles[i], 14 - i);
    }

    COLORREF emClr = isDark ? RGB(0xBB,0xC2,0xCF) : sP.fg;
    SetStyle(v, SCE_MARKDOWN_EM1,     emClr);
    v.Call(SCI_STYLESETITALIC, SCE_MARKDOWN_EM1, 1);
    SetStyle(v, SCE_MARKDOWN_EM2,     emClr);
    v.Call(SCI_STYLESETITALIC, SCE_MARKDOWN_EM2, 1);
    SetStyle(v, SCE_MARKDOWN_STRONG1, emClr);
    v.Call(SCI_STYLESETBOLD,   SCE_MARKDOWN_STRONG1, 1);
    SetStyle(v, SCE_MARKDOWN_STRONG2, emClr);
    v.Call(SCI_STYLESETBOLD,   SCE_MARKDOWN_STRONG2, 1);

    // Code block: tinted from the theme's caret line (already on-theme).
    COLORREF codeFg = sP.preproc;
    COLORREF codeBg = sP.caretLine;
    SetStyle(v, SCE_MARKDOWN_CODE,    codeFg, codeBg);
    SetStyle(v, SCE_MARKDOWN_CODE2,   codeFg, codeBg);
    SetStyle(v, SCE_MARKDOWN_CODEBK,  codeFg, codeBg);

    SetStyle(v, SCE_MARKDOWN_PRECHAR,     sP.keyword2);
    SetStyle(v, SCE_MARKDOWN_ULIST_ITEM,  sP.keyword);
    SetStyle(v, SCE_MARKDOWN_OLIST_ITEM,  sP.keyword);
    SetStyle(v, SCE_MARKDOWN_BLOCKQUOTE,  sP.comment);
    v.Call(SCI_STYLESETITALIC, SCE_MARKDOWN_BLOCKQUOTE, 1);
    SetStyle(v, SCE_MARKDOWN_STRIKEOUT,   sP.comment);
    SetStyle(v, SCE_MARKDOWN_HRULE,       sP.keyword2);

    const UiPalette& u = Ui(tid);
    SetStyle(v, SCE_MARKDOWN_LINK, u.accent);
    v.Call(SCI_STYLESETUNDERLINE, SCE_MARKDOWN_LINK, 1);
}

void StyleGeneric(ScintillaEditView& /*v*/) { /* no-op */ }

// ---- Embedded code styling for Markdown fences ----------------------------
// Style indices 200-207 are reserved for tokens inside fenced code blocks.
// They are independent of any lexer's style range so the markdown lexer's
// own SCE_MARKDOWN_CODEBK styling is freely overwritten per-token here.
enum EmbStyle {
    EMB_DEFAULT = 200,
    EMB_COMMENT = 201,
    EMB_NUMBER  = 202,
    EMB_STRING  = 203,
    EMB_KEYWORD = 204,
    EMB_KEYWORD2= 205,  // control flow / secondary
    EMB_PREPROC = 206,
    EMB_TYPE    = 207,
    EMB_FUNC    = 208,  // identifier immediately followed by '('
};

enum class LangFamily { Cpp, Python, Json, Bash, Generic };

struct LangRule {
    LangFamily family;
    std::set<std::string> keywords;
    std::set<std::string> keywords2;
    bool hashAsPreproc;   // C/C++: # at line start = preproc
};

static std::set<std::string> Words(std::string_view src)
{
    std::set<std::string> out;
    size_t i = 0;
    while (i < src.size()) {
        while (i < src.size() && (src[i]==' '||src[i]=='\t'||src[i]=='\n')) ++i;
        size_t j = i;
        while (j < src.size() && src[j]!=' '&&src[j]!='\t'&&src[j]!='\n') ++j;
        if (j > i) out.emplace(src.substr(i, j - i));
        i = j;
    }
    return out;
}

static const LangRule& LookupLang(std::string_view name)
{
    // Lowercase + alias collapse.
    std::string n;
    n.reserve(name.size());
    for (char c : name) n.push_back((c>='A'&&c<='Z') ? char(c+32) : c);
    if (n=="c++"||n=="cxx"||n=="cc"||n=="hpp"||n=="hxx"||n=="h"||n=="cpp") n="cpp";
    else if (n=="cs") n="csharp";
    else if (n=="js"||n=="jsx"||n=="mjs") n="javascript";
    else if (n=="ts"||n=="tsx") n="typescript";
    else if (n=="py"||n=="py3") n="python";
    else if (n=="sh"||n=="zsh"||n=="shell") n="bash";
    else if (n=="rs") n="rust";
    else if (n=="golang") n="go";

    static std::map<std::string, LangRule> table = []() {
        std::map<std::string, LangRule> t;
        // C / C++
        auto cppKw = Words(
            "alignas alignof and and_eq asm auto bitand bitor bool char "
            "char8_t char16_t char32_t class compl concept const consteval "
            "constexpr constinit const_cast decltype delete double dynamic_cast "
            "enum explicit export extern false float friend inline int long "
            "mutable namespace new noexcept not not_eq nullptr operator or "
            "or_eq private protected public register reinterpret_cast requires "
            "short signed sizeof static static_assert static_cast struct "
            "template this thread_local true typedef typeid typename union "
            "unsigned using virtual void volatile wchar_t xor xor_eq");
        auto cppCtl = Words(
            "break case catch continue co_await co_return co_yield default "
            "do else for goto if return switch throw try while");
        t["cpp"]    = {LangFamily::Cpp, cppKw, cppCtl, true};
        t["c"]      = t["cpp"];
        t["csharp"] = {LangFamily::Cpp, Words(
            "abstract as base bool byte char class const decimal delegate "
            "double enum event explicit extern false fixed float implicit int "
            "interface internal long namespace new null object operator out "
            "override params private protected public readonly ref sbyte "
            "sealed short stackalloc static string struct this true uint "
            "ulong unsafe ushort using var virtual void volatile"),
            Words("break case catch continue default do else finally for foreach "
                  "goto if in is lock return switch throw try when while yield"),
            false};
        t["java"]   = {LangFamily::Cpp, Words(
            "abstract assert boolean byte char class const default double "
            "enum extends false final finally float implements import "
            "instanceof int interface long native new null package private "
            "protected public short static strictfp super synchronized this "
            "throws transient true var void volatile"),
            Words("break case catch continue do else for goto if return switch "
                  "throw try while yield"), false};
        t["javascript"] = {LangFamily::Cpp, Words(
            "async await class const debugger delete export extends false "
            "from function get import in instanceof let new null of "
            "operator set static super this true typeof undefined var void"),
            Words("break case catch continue default do else finally for if "
                  "return switch throw try while yield"), false};
        t["typescript"] = {LangFamily::Cpp, Words(
            "abstract any as async await boolean class const constructor "
            "debugger declare delete enum export extends false from function "
            "get implements import in instanceof interface keyof let module "
            "namespace never new null number object of private protected "
            "public readonly require set static string super this true type "
            "typeof undefined var void"),
            Words("break case catch continue default do else finally for if "
                  "return switch throw try while yield"), false};
        t["rust"] = {LangFamily::Cpp, Words(
            "as async await const crate dyn enum extern false fn impl let "
            "mod move mut pub ref self Self static struct super trait true "
            "type unsafe use where"),
            Words("break continue do else for if in loop match return while yield"),
            false};
        t["go"] = {LangFamily::Cpp, Words(
            "chan const false func import interface map nil package range "
            "struct true type var"),
            Words("break case continue default defer do else fallthrough for "
                  "go goto if return select switch"), false};
        // Python
        t["python"] = {LangFamily::Python, Words(
            "False None True and as assert async await class def del elif "
            "else except finally for from global if import in is lambda "
            "nonlocal not or pass raise return try while with yield match case"),
            {}, false};
        // JSON
        t["json"] = {LangFamily::Json, Words("true false null"), {}, false};
        // Bash
        t["bash"] = {LangFamily::Bash, Words(
            "if then else elif fi case esac for while until do done in "
            "function return break continue exit local readonly export "
            "declare typeset select time eval exec set unset shift trap source"),
            {}, false};
        return t;
    }();
    auto it = table.find(n);
    if (it != table.end()) return it->second;
    static const LangRule generic{LangFamily::Generic, {}, {}, false};
    return generic;
}

static void TokenizeAndStyle(ScintillaEditView& v, sptr_t startPos,
                             const std::string& text, const LangRule& lang)
{
    v.Call(SCI_STARTSTYLING, static_cast<uptr_t>(startPos));

    // Coalesce runs of unstyled chars (whitespace, punctuation) into a single
    // SCI_SETSTYLING call — per-byte messaging is what made big files feel
    // sluggish on theme switch.
    size_t defStart = 0;
    bool atLineStart = true;
    auto flushDefault = [&](size_t upto) {
        if (upto > defStart)
            v.Call(SCI_SETSTYLING, upto - defStart, EMB_DEFAULT);
    };
    auto emitToken = [&](size_t pos, size_t len, int style) {
        flushDefault(pos);
        v.Call(SCI_SETSTYLING, len, style);
        defStart = pos + len;
    };

    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);

        // Block comment /* ... */ (Cpp-family).
        if (lang.family == LangFamily::Cpp &&
            c == '/' && i+1 < text.size() && text[i+1] == '*') {
            size_t j = i + 2;
            while (j+1 < text.size() && !(text[j]=='*' && text[j+1]=='/')) ++j;
            j = (j+1 < text.size()) ? j+2 : text.size();
            emitToken(i, j - i, EMB_COMMENT);
            i = j; atLineStart = false; continue;
        }
        // Line comments.
        if (lang.family == LangFamily::Cpp &&
            c == '/' && i+1 < text.size() && text[i+1] == '/') {
            size_t j = i;
            while (j < text.size() && text[j] != '\n') ++j;
            emitToken(i, j - i, EMB_COMMENT);
            i = j; continue;
        }
        if ((lang.family == LangFamily::Python || lang.family == LangFamily::Bash) &&
            c == '#') {
            size_t j = i;
            while (j < text.size() && text[j] != '\n') ++j;
            emitToken(i, j - i, EMB_COMMENT);
            i = j; continue;
        }
        // C/C++ preprocessor: # at line start through end of line.
        if (lang.hashAsPreproc && atLineStart && c == '#') {
            size_t j = i;
            while (j < text.size() && text[j] != '\n') ++j;
            emitToken(i, j - i, EMB_PREPROC);
            i = j; continue;
        }
        // Strings "..." or '...' (single-line, with backslash escapes).
        if (c == '"' || c == '\'') {
            char q = static_cast<char>(c);
            size_t j = i + 1;
            while (j < text.size() && text[j] != q && text[j] != '\n') {
                if (text[j] == '\\' && j+1 < text.size()) j += 2;
                else ++j;
            }
            if (j < text.size() && text[j] == q) ++j;
            emitToken(i, j - i, EMB_STRING);
            i = j; atLineStart = false; continue;
        }
        // Numbers (incl. 0x.. and floats).
        if (c >= '0' && c <= '9') {
            size_t j = i;
            while (j < text.size()) {
                unsigned char d = static_cast<unsigned char>(text[j]);
                if ((d>='0'&&d<='9')||(d>='a'&&d<='f')||(d>='A'&&d<='F')||
                    d=='x'||d=='X'||d=='.'||d=='_') ++j;
                else break;
            }
            while (j < text.size() && (text[j]=='u'||text[j]=='U'||
                   text[j]=='l'||text[j]=='L'||text[j]=='f'||text[j]=='F')) ++j;
            emitToken(i, j - i, EMB_NUMBER);
            i = j; atLineStart = false; continue;
        }
        // Identifier / keyword.
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_') {
            size_t j = i;
            while (j < text.size()) {
                unsigned char d = static_cast<unsigned char>(text[j]);
                if ((d>='a'&&d<='z')||(d>='A'&&d<='Z')||
                    (d>='0'&&d<='9')||d=='_') ++j;
                else break;
            }
            std::string word(text.data() + i, j - i);
            int style = EMB_DEFAULT;
            if (lang.keywords.count(word))       style = EMB_KEYWORD;
            else if (lang.keywords2.count(word)) style = EMB_KEYWORD2;
            else {
                size_t k = j;
                while (k < text.size() && (text[k]==' '||text[k]=='\t')) ++k;
                if (k < text.size() && text[k]=='(') style = EMB_FUNC;
            }
            // Plain identifier (style==EMB_DEFAULT) just rolls into the run.
            if (style != EMB_DEFAULT) emitToken(i, j - i, style);
            i = j; atLineStart = false; continue;
        }
        // Whitespace / punctuation — extend the pending default run.
        if (c == '\n') atLineStart = true;
        else if (c != ' ' && c != '\t' && c != '\r') atLineStart = false;
        ++i;
    }
    flushDefault(i);
}

void SetLexerByName(ScintillaEditView& v, const char* lexerName)
{
    if (!lexerName || !*lexerName) {
        v.Call(SCI_SETILEXER, 0, 0);
        return;
    }
    void* lexer = reinterpret_cast<void*>(CreateLexer(lexerName));
    v.Call(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(lexer));
}

// Configure embedded-code style slots (200-208) on the view. Cheap — pure
// SCI_STYLESETFORE/BACK calls, no doc traversal. Run on every ApplyLanguage
// so theme changes update fenced-block / function-name colors even when we
// skip the heavy lex/scan passes.
static void SetupEmbeddedStyles(ScintillaEditView& v)
{
    auto setEmb = [&](int s, COLORREF fg, COLORREF bg) {
        v.Call(SCI_STYLESETFORE, static_cast<uptr_t>(s), static_cast<sptr_t>(fg));
        v.Call(SCI_STYLESETBACK, static_cast<uptr_t>(s), static_cast<sptr_t>(bg));
        v.Call(SCI_STYLESETFONT, static_cast<uptr_t>(s),
            reinterpret_cast<sptr_t>("Cascadia Mono"));
        v.Call(SCI_STYLESETSIZE, static_cast<uptr_t>(s), 11);
    };
    setEmb(EMB_DEFAULT,  sP.fg,       sP.caretLine);
    setEmb(EMB_COMMENT,  sP.comment,  sP.caretLine);
    setEmb(EMB_NUMBER,   sP.number,   sP.caretLine);
    setEmb(EMB_STRING,   sP.string,   sP.caretLine);
    setEmb(EMB_KEYWORD,  sP.keyword,  sP.caretLine);
    setEmb(EMB_KEYWORD2, sP.keyword2, sP.caretLine);
    setEmb(EMB_PREPROC,  sP.preproc,  sP.caretLine);
    setEmb(EMB_TYPE,     sP.keyword2, sP.caretLine);
    // Function-name slot uses the editor bg (not the code-block tint) so
    // top-level cpp/python identifiers paint flush with the editor surface.
    setEmb(EMB_FUNC,     sP.func,     sP.bg);
}

// Per-document state for ApplyLanguage gating: track the last lang we
// post-styled this doc with. Theme changes don't invalidate doc style bytes
// — the view's color mappings get rewritten and existing bytes render fine.
struct AppliedState { LangType lang; };
static std::map<sptr_t, AppliedState> g_applied;

// Whole-document post-lex passes (function-name scan, markdown fences) fetch
// the full text — HighlightFunctionNames even at 2 bytes/char. Cap them so a
// pathological file doesn't turn every debounce flush into a quarter-GB
// allocation; the lexer's own colourise still runs at any size.
constexpr sptr_t kMaxWholeDocPassBytes = 32 * 1024 * 1024;

} // namespace

void ForgetDocStyle(sptr_t docHandle)
{
    g_applied.erase(docHandle);
}

void ApplyLanguage(ScintillaEditView& v, LangType lang)
{
    sptr_t docHandle = v.Call(SCI_GETDOCPOINTER);
    auto it = g_applied.find(docHandle);
    bool firstSeen   = (it == g_applied.end());
    bool langChanged = !firstSeen && it->second.lang != lang;

    SelectPalette(Parameters::Instance().Theme());
    ResetStyles(v);
    SetupEmbeddedStyles(v);
    // Only (re)attach a lexer when the language for this doc changes. Calling
    // SCI_SETILEXER resets Scintilla's endStyled counter and forces a full
    // re-colourise on every tab switch — which on 80k+ files shows as a
    // multi-second stall plus repeated repaints.
    if (firstSeen || langChanged)
        SetLexerByName(v, LangLexerName(lang));

    switch (lang) {
    case LangType::C:
    case LangType::Cpp:
    case LangType::CSharp:
    case LangType::Java:
    case LangType::ObjectiveC:
    case LangType::JavaScript:
    case LangType::TypeScript:
    case LangType::Go:
    case LangType::Swift:
    case LangType::Rust:
    case LangType::Kotlin:
    case LangType::Dart:
    case LangType::Scala:
        StyleCpp(v); break;
    case LangType::Python:
        StylePython(v); break;
    case LangType::Html:
    case LangType::PhP:
        StyleHtml(v); break;
    case LangType::Xml:
        StyleXml(v); break;
    case LangType::Css:
    case LangType::Scss:
    case LangType::Less:
        StyleCss(v); break;
    case LangType::Json:
        StyleJson(v); break;
    case LangType::Shell:
        StyleBash(v); break;
    case LangType::Markdown:
        StyleMarkdown(v); break;
    default:
        StyleGeneric(v); break;
    }

    // Heavy work — gate on (lang change for this doc) || (doc not fully lex'd).
    // Tab-switch back to a doc with same lang, fully styled: skip entirely.
    sptr_t totalLen  = v.Call(SCI_GETLENGTH);
    sptr_t styledTo  = v.Call(SCI_GETENDSTYLED);
    bool unstyled    = (totalLen > 0 && styledTo < totalLen);
    if (firstSeen || langChanged || unstyled) {
        // Paint the attached document immediately with default styling — then
        // run colourise + function-name pass. Otherwise the user waits through
        // the whole heavy path before seeing any text (very visible on large
        // files opened via drag-drop, where the window is already foreground).
        if (v.Hwnd()) ::UpdateWindow(v.Hwnd());

        // Coalesce the thousands of SCI_SETSTYLING calls (and the colourise
        // invalidation) into a single final repaint — without this the paint
        // queue churns for seconds on big files.
        // Turn wrap off while we colourise. With wrap on, Scintilla recomputes
        // wrap positions incrementally on idle/paint — interleaved with our
        // styling passes this shows up as 3-4s of flicker on big files.
        sptr_t prevWrap = v.Call(SCI_GETWRAPMODE);
        if (prevWrap != SC_WRAP_NONE) v.Call(SCI_SETWRAPMODE, SC_WRAP_NONE);

        if (v.Hwnd()) ::SendMessageW(v.Hwnd(), WM_SETREDRAW, FALSE, 0);
        v.Call(SCI_COLOURISE, 0, -1);
        if (lang == LangType::Markdown)
            StyleMarkdownFences(v);
        else
            HighlightFunctionNames(v, lang);
        if (v.Hwnd()) {
            ::SendMessageW(v.Hwnd(), WM_SETREDRAW, TRUE, 0);
            ::InvalidateRect(v.Hwnd(), nullptr, FALSE);
        }

        if (prevWrap != SC_WRAP_NONE) v.Call(SCI_SETWRAPMODE, prevWrap);
        g_applied[docHandle] = AppliedState{lang};
    }
}

void HighlightFunctionNames(ScintillaEditView& v, LangType lang)
{
    // Identifier style ID per lexer family (Scintilla SCE_*_IDENTIFIER values).
    int idStyle = -1;
    switch (lang) {
    case LangType::C:        case LangType::Cpp:
    case LangType::CSharp:   case LangType::Java:
    case LangType::ObjectiveC:
    case LangType::JavaScript:case LangType::TypeScript:
    case LangType::Go:       case LangType::Swift:
    case LangType::Rust:     case LangType::Kotlin:
    case LangType::Dart:     case LangType::Scala:
        idStyle = 11; break;          // SCE_C_IDENTIFIER
    case LangType::Python:
        idStyle = 11; break;          // SCE_P_IDENTIFIER
    default:
        return;
    }

    SelectPalette(Parameters::Instance().Theme());
    v.Call(SCI_STYLESETFORE, EMB_FUNC, static_cast<sptr_t>(sP.func));
    v.Call(SCI_STYLESETBACK, EMB_FUNC, static_cast<sptr_t>(sP.bg));
    v.Call(SCI_STYLESETFONT, EMB_FUNC,
        reinterpret_cast<sptr_t>("Cascadia Mono"));
    v.Call(SCI_STYLESETSIZE, EMB_FUNC, 11);

    sptr_t len = v.Call(SCI_GETLENGTH);
    if (len <= 0 || len > kMaxWholeDocPassBytes) return;

    // One bulk fetch of interleaved (char, style) bytes — much faster than
    // SCI_GETSTYLEAT per identifier on big files.
    std::string styled(2 * static_cast<size_t>(len) + 2, '\0');
    // SCI_GETSTYLEDTEXTFULL pairs with Sci_TextRangeFull (64-bit positions).
    // The non-FULL variant takes Sci_TextRange (32-bit positions) — feeding it
    // the Full struct misaligns lpstrText and crashes when Scintilla writes the
    // trailing NUL to a bogus address.
    Sci_TextRangeFull tr{{0, static_cast<Sci_Position>(len)}, styled.data()};
    v.Call(SCI_GETSTYLEDTEXTFULL, 0, reinterpret_cast<sptr_t>(&tr));

    auto ch    = [&](size_t k) { return static_cast<unsigned char>(styled[2*k]); };
    auto stAt  = [&](size_t k) { return static_cast<unsigned char>(styled[2*k+1]); };
    const size_t n = static_cast<size_t>(len);

    size_t i = 0;
    while (i < n) {
        unsigned char c = ch(i);
        bool idStart = (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';
        if (!idStart) { ++i; continue; }
        size_t j = i;
        while (j < n) {
            unsigned char d = ch(j);
            if ((d>='a'&&d<='z')||(d>='A'&&d<='Z')||
                (d>='0'&&d<='9')||d=='_') ++j;
            else break;
        }
        // Only re-style if the lexer thinks this run is an identifier — leaves
        // keywords, strings, comments, preprocessor untouched.
        if (stAt(i) == static_cast<unsigned char>(idStyle)) {
            size_t k = j;
            while (k < n && (ch(k)==' '||ch(k)=='\t')) ++k;
            if (k < n && ch(k)=='(') {
                v.Call(SCI_STARTSTYLING, static_cast<uptr_t>(i));
                v.Call(SCI_SETSTYLING, j - i, EMB_FUNC);
            }
        }
        i = j;
    }
}

void StyleMarkdownFences(ScintillaEditView& v)
{
    SelectPalette(Parameters::Instance().Theme());

    // Configure embedded-code styles. Bg is the same tinted block bg used
    // by the markdown lexer for code regions, so the strip stays cohesive.
    auto setEmb = [&](int s, COLORREF fg) {
        v.Call(SCI_STYLESETFORE, static_cast<uptr_t>(s), static_cast<sptr_t>(fg));
        v.Call(SCI_STYLESETBACK, static_cast<uptr_t>(s), static_cast<sptr_t>(sP.caretLine));
        v.Call(SCI_STYLESETFONT, static_cast<uptr_t>(s),
            reinterpret_cast<sptr_t>("Cascadia Mono"));
        v.Call(SCI_STYLESETSIZE, static_cast<uptr_t>(s), 11);
    };
    setEmb(EMB_DEFAULT,  sP.fg);
    setEmb(EMB_COMMENT,  sP.comment);
    setEmb(EMB_NUMBER,   sP.number);
    setEmb(EMB_STRING,   sP.string);
    setEmb(EMB_KEYWORD,  sP.keyword);
    setEmb(EMB_KEYWORD2, sP.keyword2);
    setEmb(EMB_PREPROC,  sP.preproc);
    setEmb(EMB_TYPE,     sP.keyword2);
    setEmb(EMB_FUNC,     sP.func);

    sptr_t len = v.Call(SCI_GETLENGTH);
    if (len <= 0 || len > kMaxWholeDocPassBytes) return;
    std::string buf(static_cast<size_t>(len), '\0');
    v.Call(SCI_GETTEXT, static_cast<uptr_t>(len + 1),
        reinterpret_cast<sptr_t>(buf.data()));

    auto isFence = [&](size_t pos) {
        if (pos + 3 > buf.size()) return false;
        bool atLineStart = (pos == 0) || buf[pos - 1] == '\n';
        return atLineStart && buf[pos]=='`' && buf[pos+1]=='`' && buf[pos+2]=='`';
    };

    size_t pos = 0;
    while (pos < buf.size()) {
        size_t fence = pos;
        while (fence < buf.size() && !isFence(fence)) ++fence;
        if (fence >= buf.size()) break;

        size_t langStart = fence + 3;
        size_t langEnd = langStart;
        while (langEnd < buf.size() && buf[langEnd] != '\n' && buf[langEnd] != '\r')
            ++langEnd;
        std::string langName(buf.data() + langStart, langEnd - langStart);
        while (!langName.empty() && (langName.back()==' '||langName.back()=='\t'))
            langName.pop_back();
        size_t bodyStart = langEnd;
        if (bodyStart < buf.size() && buf[bodyStart] == '\r') ++bodyStart;
        if (bodyStart < buf.size() && buf[bodyStart] == '\n') ++bodyStart;

        size_t closing = bodyStart;
        while (closing < buf.size() && !isFence(closing)) ++closing;

        size_t bodyEnd = (closing < buf.size()) ? closing : buf.size();
        if (bodyEnd > bodyStart) {
            std::string body(buf.data() + bodyStart, bodyEnd - bodyStart);
            const LangRule& rule = LookupLang(langName);
            TokenizeAndStyle(v, static_cast<sptr_t>(bodyStart), body, rule);
        }
        if (closing >= buf.size()) break;
        pos = closing + 3;
    }
}

} // namespace npp
