#define GPUI_INCLUDE_PRIVATE_API 1
#include "autocorrect.h"

#include <stdio.h>
#include <string.h>

#line 1 "src/autocorrect/code.cpp"

namespace autocorrect {

static const bool kContextCodeblockEnabled = true;

static void CursorAdvance(Results* res, Str part) {
    int i = 0;
    while (i < len(part)) {
        char c = part.s[i];
        if (c == '\r' && i + 1 < len(part) && part.s[i + 1] == '\n') {
            res->line++;
            res->col = 1;
            i += 2;
            continue;
        }
        if (c == '\n') {
            res->line++;
            res->col = 1;
            i++;
            continue;
        }
        res->col++;
        i += Utf8Len(part, i);
    }
}

void EmitIgnore(Results* res, Str part) {
    if (!res->lint) {
        res->out.Append(part);
    }
    CursorAdvance(res, part);
}

static Str TrimStart(Str s, int* leadingBytes) {
    int i = 0;
    while (i < len(s) && (s.s[i] == ' ' || s.s[i] == '\t' || s.s[i] == '\r' ||
                          s.s[i] == '\n' || s.s[i] == '\f')) {
        i++;
    }
    *leadingBytes = i;
    return Str(s.s + i, len(s) - i);
}

static Str TrimEnd(Str s) {
    int end = len(s);
    while (end > 0 && (s.s[end - 1] == ' ' || s.s[end - 1] == '\t' ||
                       s.s[end - 1] == '\r' || s.s[end - 1] == '\n' ||
                       s.s[end - 1] == '\f')) {
        end--;
    }
    return Str(s.s, end);
}

static Str Trim(Str s) {
    int leading = 0;
    return TrimEnd(TrimStart(s, &leading));
}

void EmitText(Results* res, Str rule, Str part) {
    int line0 = res->line;
    int col0 = res->col;

    if (StrEq(rule, StrL("comment")) || StrEq(rule, StrL("COMMENT"))) {
        Toggle t = ToggleParse(part);
        if (t.kind != ToggleKind::None) {
            res->toggle = t;
        }
    }
    uint16_t mask = ToggleDisableRules(&res->toggle);
    bool enabled = ToggleIsEnabled(&res->toggle);
    if (res->lint) {
        if (!enabled) {
            CursorAdvance(res, part);
            return;
        }
        int subLine = 0;
        int lineStart = 0;
        for (int i = 0; i <= len(part); i++) {
            if (i < len(part) && part.s[i] != '\n') {
                continue;
            }
            Str lineStr(part.s + lineStart, i - lineStart);
            RuleResult lr = FormatOrLintText(res->a, lineStr, true, mask);
            if (lr.severity != Severity::Pass) {
                int leadingBytes = 0;
                Str trimmed = TrimEnd(TrimStart(lineStr, &leadingBytes));
                LineResult out;
                out.line = line0 + subLine;

                out.col = subLine > 0 ? leadingBytes + 1 : col0;
                out.old = base::StrDup(res->a, trimmed);
                out.neu = base::StrDup(res->a, Trim(lr.out));
                out.severity = lr.severity;
                res->lines.Append(res->a, out);
            }
            subLine++;
            lineStart = i + 1;
        }
        CursorAdvance(res, part);
        return;
    }
    if (!enabled) {
        res->out.Append(part);
        CursorAdvance(res, part);
        return;
    }

    int lineStart = 0;
    bool first = true;
    for (int i = 0; i <= len(part); i++) {
        if (i < len(part) && part.s[i] != '\n') {
            continue;
        }
        if (!first) {
            res->out.AppendChar('\n');
        }
        first = false;
        Str lineStr(part.s + lineStart, i - lineStart);
        RuleResult fr = FormatOrLintText(res->a, lineStr, false, mask);
        res->out.Append(fr.out);
        lineStart = i + 1;
    }
    CursorAdvance(res, part);
}

void EmitError(Results* res, Str raw, Str message) {
    (void)raw;
    res->error = base::StrDup(res->a, message);
}

static void EmitSub(Results* res, Str part, Str lang, Str code,
                    bool replaceInPart) {
    int baseLine = res->line;
    if (res->lint) {
        if (!ToggleIsEnabled(&res->toggle) || !kContextCodeblockEnabled) {
            CursorAdvance(res, part);
            return;
        }
        LintResult sub = LintFor(res->a, code, lang);
        if (sub.HasError()) {
            res->error = sub.error;
        }
        for (int i = 0; i < sub.nLines; i++) {
            LineResult line = sub.lines[i];

            line.line += baseLine - 1;
            res->lines.Append(res->a, line);
        }
        CursorAdvance(res, part);
        return;
    }
    if (!ToggleIsEnabled(&res->toggle) || !kContextCodeblockEnabled) {
        res->out.Append(part);
        CursorAdvance(res, part);
        return;
    }
    FormatResult sub = FormatFor(res->a, code, lang);
    if (sub.HasError()) {
        res->error = sub.error;
    }
    if (!replaceInPart) {
        res->out.Append(sub.out);
    } else if (len(code) == 0) {

        res->out.Append(part);
    } else {

        int at = 0;
        while (at + len(code) <= len(part)) {
            if (StrEq(Str(part.s + at, len(code)), code)) {
                res->out.Append(sub.out);
                at += len(code);
                continue;
            }
            res->out.AppendChar(part.s[at]);
            at++;
        }
        res->out.Append(Str(part.s + at, len(part) - at));
    }
    CursorAdvance(res, part);
}

void EmitCodeblock(Results* res, Str part, Str lang, Str code) {
    EmitSub(res, part, lang, code, true);
}

void EmitInlineScript(Results* res, Str rule, Str part) {
    Str lang = StrEq(rule, StrL("inline_style")) ? StrL("css") : StrL("js");
    EmitSub(res, part, lang, part, false);
}

Toggle ResultsPushCodeblockToggle(Results* res) {
    Toggle saved = res->toggle;
    Toggle disable;
    disable.kind = ToggleKind::Disable;
    disable.mask = (uint16_t)(1u << kRuleHalfwidthPunctuation);
    ToggleMerge(&res->toggle, disable);
    return saved;
}

LintResult LintTake(Results* res, Str raw) {
    (void)raw;
    LintResult out;
    out.lines = res->lines.Flatten(res->a);
    out.nLines = res->lines.len;
    out.error = res->error;
    return out;
}

FormatResult FormatTake(Results* res, Str raw) {
    FormatResult out;
    out.error = res->error;

    out.out = len(res->error) > 0
                  ? base::StrDup(res->a, raw)
                  : base::StrDup(res->a, Str(res->out.els, res->out.len));
    return out;
}

static bool ScanForType(Str type, Results* res, Str raw) {
    struct Entry {
        const char* type;
        void (*scan)(Results*, Str);
    };
    static const Entry kTable[] = {
        {"html", ScanHtml},     {"yaml", ScanYaml},
        {"sql", ScanSql},       {"rust", ScanRust},
        {"ruby", ScanRuby},     {"elixir", ScanElixir},
        {"go", ScanGo},         {"javascript", ScanJavascript},
        {"css", ScanCss},       {"json", ScanJson},
        {"python", ScanPython}, {"objective_c", ScanObjectiveC},
        {"csharp", ScanCsharp}, {"swift", ScanSwift},
        {"java", ScanJava},     {"scala", ScanScala},
        {"kotlin", ScanKotlin}, {"php", ScanPhp},
        {"dart", ScanDart},     {"markdown", ScanMarkdown},
        {"conf", ScanConf},     {"c", ScanC},
        {"zig", ScanRust},      {"text", ScanMarkdown},
    };
    for (const Entry& e : kTable) {
        if (base::StrEq(type, Str(e.type))) {
            e.scan(res, raw);
            return true;
        }
    }
    return false;
}

LintResult LintFor(Arena* a, Str raw, Str filenameOrExt) {
    Str type = MatchFilename(a, filenameOrExt);
    Results res;
    res.a = a;
    res.lint = true;
    LintResult out;
    if (ScanForType(type, &res, raw)) {
        out = LintTake(&res, raw);
    }
    out.filepath = base::StrDup(a, filenameOrExt);
    return out;
}

FormatResult FormatFor(Arena* a, Str raw, Str filenameOrExt) {
    Str type = MatchFilename(a, filenameOrExt);
    Results res;
    res.a = a;
    res.lint = false;
    if (ScanForType(type, &res, raw)) {
        return FormatTake(&res, raw);
    }
    FormatResult out;
    out.out = base::StrDup(a, raw);
    return out;
}

}

#line 1 "src/autocorrect/config.cpp"

namespace autocorrect {

static const char* const kRuleNames[kNRules] = {
    "space-word",         "space-punctuation",        "space-bracket",
    "space-dash",         "space-backticks",          "space-dollar",
    "fullwidth",          "halfwidth-word",           "halfwidth-punctuation",
    "no-space-fullwidth", "no-space-fullwidth-quote", "spellcheck",
};

SeverityMode RuleSeverity(int rule) {

    if (rule == kRuleSpaceDollar || rule == kRuleSpellcheck) {
        return SeverityMode::Off;
    }
    if (rule >= 0 && rule < kNRules) {
        return SeverityMode::Error;
    }
    return SeverityMode::Off;
}

int RuleIdByName(Str name) {
    for (int i = 0; i < kNRules; i++) {
        Str candidate(kRuleNames[i]);
        if (len(name) == len(candidate) && base::StrEqI(name, kRuleNames[i])) {
            return i;
        }
    }
    return -1;
}

struct FileType {
    const char* ext;
    const char* type;
};

static const FileType kFileTypes[] = {

    {"html", "html"},
    {"htm", "html"},
    {"vue", "html"},
    {"ejs", "html"},
    {"html.erb", "html"},
    {"svelte", "html"},

    {"yaml", "yaml"},
    {"yml", "yaml"},

    {"rust", "rust"},
    {"rs", "rust"},

    {"sql", "sql"},

    {"ruby", "ruby"},
    {"rb", "ruby"},
    {"Gemfile", "ruby"},
    {"Rakefile", "ruby"},
    {"Profile", "ruby"},
    {"gemspec", "ruby"},

    {"crystal", "ruby"},
    {"cr", "ruby"},

    {"elixir", "elixir"},
    {"ex", "elixir"},
    {"exs", "elixir"},

    {"js", "javascript"},
    {"jsx", "javascript"},
    {"javascript", "javascript"},
    {"ts", "javascript"},
    {"tsx", "javascript"},
    {"typescript", "javascript"},
    {"js.erb", "javascript"},

    {"css", "css"},
    {"scss", "css"},
    {"sass", "css"},
    {"less", "css"},

    {"json", "json"},
    {"json5", "json"},

    {"go", "go"},

    {"python", "python"},
    {"py", "python"},

    {"objective_c", "objective_c"},
    {"objective-c", "objective_c"},
    {"m", "objective_c"},
    {"h", "objective_c"},

    {"strings", "strings"},

    {"csharp", "csharp"},
    {"cs", "csharp"},

    {"java", "java"},
    {"proto", "java"},

    {"scala", "scala"},

    {"swift", "swift"},

    {"kotlin", "kotlin"},
    {"kt", "kotlin"},
    {"gradle", "kotlin"},

    {"php", "php"},

    {"dart", "dart"},

    {"markdown", "markdown"},
    {"md", "markdown"},
    {"mdx", "markdown"},

    {"latex", "latex"},
    {"tex", "latex"},

    {"asciidoc", "asciidoc"},
    {"adoc", "asciidoc"},
    {"asc", "asciidoc"},

    {"po", "gettext"},
    {"pot", "gettext"},

    {"properties", "conf"},
    {"conf", "conf"},
    {"ini", "conf"},
    {"cfg", "conf"},
    {"toml", "conf"},

    {"cc", "c"},
    {"cpp", "c"},
    {"c", "c"},

    {"xml", "xml"},

    {"jupyter", "jupyter"},
    {"ipynb", "jupyter"},

    {"sh", "ruby"},
    {"shell", "ruby"},

    {"text", "text"},
    {"plain", "text"},
    {"txt", "text"},
};

static const int kNFileTypes =
    (int)(sizeof(kFileTypes) / sizeof(kFileTypes[0]));

static Str FileTypeFor(Str ext) {
    for (int i = 0; i < kNFileTypes; i++) {
        if (base::StrEq(ext, Str(kFileTypes[i].ext))) {
            return Str(kFileTypes[i].type);
        }
    }
    return {};
}

bool IsSupportType(Str filenameOrExt) {
    return len(FileTypeFor(filenameOrExt)) > 0;
}

Str GetFileExtension(Arena* a, Str filename) {
    Str name = base::StrTrimAscii(filename);
    if (IsSupportType(name)) {
        return base::StrDup(a, name);
    }

    for (int i = len(name) - 1; i >= 0; i--) {
        if (name.s[i] == '/') {
            name = Str(name.s + i + 1, len(name) - i - 1);
            break;
        }
    }

    int lastDot = -1;
    int secondLastDot = -1;
    int nDots = 0;
    for (int i = 0; i < len(name); i++) {
        if (name.s[i] == '.') {
            secondLastDot = lastDot;
            lastDot = i;
            nDots++;
        }
    }
    if (nDots == 0) {
        return base::StrDup(a, name);
    }
    Str ext(name.s + lastDot + 1, len(name) - lastDot - 1);
    if (nDots >= 2) {
        Str doubleExt(name.s + secondLastDot + 1,
                      len(name) - secondLastDot - 1);
        if (IsSupportType(doubleExt)) {
            ext = doubleExt;
        }
    }
    return base::StrDup(a, ext);
}

Str MatchFilename(Arena* a, Str filenameOrExt) {
    Str ext = GetFileExtension(a, filenameOrExt);
    Str type = FileTypeFor(ext);
    if (len(type) > 0) {
        return type;
    }
    return base::StrDup(a, filenameOrExt);
}

}

#line 1 "src/autocorrect/fullwidth.cpp"

namespace autocorrect {

static bool IsCjClassCp(uint32_t cp) {
    return cp == '|' || IsCj(cp);
}

static bool IsCjOrWordClassCp(uint32_t cp) {
    return IsCjClassCp(cp) || IsWordCp(cp);
}

static bool IsNormalPunct(char c) {
    return c == ',' || c == '?';
}

static bool IsSpecialPunct(char c) {
    return c == '.' || c == ':' || c == '!';
}

static int MatchClassRun(Str s, int i, bool (*pred)(uint32_t)) {
    int at = i;
    while (at < len(s)) {
        int next = at;
        if (!pred(Utf8Next(s, &next))) {
            break;
        }
        at = next;
    }
    return at - i;
}

static Str FullwidthFor(char c) {
    switch (c) {
        case ',':
            return StrL("，");
        case '.':
            return StrL("。");
        case ':':
            return StrL("：");
        case '!':
            return StrL("！");
        case '?':
            return StrL("？");
        default:
            return {};
    }
}

static void AppendMappedSpan(StrBuilder* out, Str span) {
    int i = 0;
    while (i < len(span)) {
        char c = span.s[i];
        if (IsNormalPunct(c) || IsSpecialPunct(c)) {
            out->Append(FullwidthFor(c));
            i++;
            while (i < len(span) && span.s[i] == ' ') {
                i++;
            }
            continue;
        }
        int step = Utf8Len(span, i);
        out->Append(Str(span.s + i, step));
        i += step;
    }
}

static int MatchLeft(Str s, int i) {
    int a = MatchClassRun(s, i, IsCjOrWordClassCp);
    if (a <= 0) {
        return -1;
    }
    int at = i + a;
    if (at >= len(s) || !IsNormalPunct(s.s[at])) {
        return -1;
    }
    at++;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    int b = MatchClassRun(s, at, IsCjClassCp);
    if (b <= 0) {
        return -1;
    }
    return at + b - i;
}

static int MatchRight(Str s, int i) {
    int a = MatchClassRun(s, i, IsCjClassCp);
    if (a <= 0) {
        return -1;
    }
    int at = i + a;
    if (at >= len(s) || !IsNormalPunct(s.s[at])) {
        return -1;
    }
    at++;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    return at - i;
}

static int MatchSpecial(Str s, int i) {
    int a = MatchClassRun(s, i, IsCjClassCp);
    if (a <= 0) {
        return -1;
    }
    int at = i + a;
    if (at >= len(s) || !IsSpecialPunct(s.s[at])) {
        return -1;
    }
    at++;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    int b = MatchClassRun(s, at, IsCjClassCp);
    if (b <= 0) {
        return -1;
    }
    return at + b - i;
}

static int MatchSpecialLast(Str s, int i) {
    int a = MatchClassRun(s, i, IsCjClassCp);
    if (a <= 0) {
        return -1;
    }
    int at = i + a;
    if (at >= len(s) || !IsSpecialPunct(s.s[at])) {
        return -1;
    }
    at++;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    if (at < len(s) && (s.s[at] == '"' || s.s[at] == '\'')) {
        at++;
    }
    return at == len(s) ? at - i : -1;
}

using MatchFn = int (*)(Str, int);

static bool PassReplace(Str in, MatchFn match, StrBuilder* out) {
    bool changed = false;
    int i = 0;
    while (i < len(in)) {
        int n = match(in, i);
        if (n > 0) {
            AppendMappedSpan(out, Str(in.s + i, n));
            i += n;
            changed = true;
            continue;
        }
        int step = Utf8Len(in, i);
        out->Append(Str(in.s + i, step));
        i += step;
    }
    return changed;
}

bool FormatFullwidth(Arena* a, Str in, Str* out) {
    static const MatchFn kPatterns[] = {MatchLeft, MatchRight, MatchSpecial,
                                        MatchSpecialLast};
    Str cur = in;
    bool changed = false;
    StrBuilder b;
    for (MatchFn pattern : kPatterns) {
        b.Reset();
        if (PassReplace(cur, pattern, &b)) {
            cur = base::StrDup(a, Str(b.els, b.len));
            changed = true;
        }
    }
    if (changed) {
        *out = cur;
    }
    return changed;
}

}

#line 1 "src/autocorrect/halfwidth.cpp"

namespace autocorrect {

static bool IsAsciiDigitCp(uint32_t cp) {
    return cp >= '0' && cp <= '9';
}

static bool IsAlnumCharCp(uint32_t cp) {
    return cp != '_' && IsWordCp(cp);
}

static bool IsWhitespaceCp(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' ||
        cp == 0x0B || cp == 0x85 || cp == 0xA0) {
        return true;
    }
    return cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
           cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

static void AppendCp(StrBuilder* out, uint32_t cp) {
    char buf[4];
    int n;
    if (cp < 0x80) {
        buf[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    out->Append(Str(buf, n));
}

bool FormatHalfwidthWord(Arena* a, Str in, Str* out) {
    StrBuilder b;
    bool changed = false;
    for (int i = 0; i < len(in);) {
        uint32_t cp = Utf8Next(in, &i);

        if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) ||
            (cp >= 0xFF41 && cp <= 0xFF5A)) {
            AppendCp(&b, cp - 0xFEE0);
            changed = true;
            continue;
        }
        if (cp == 0x3000) {
            b.AppendChar(' ');
            changed = true;
            continue;
        }
        AppendCp(&b, cp);
    }

    Str cur = changed ? Str(b.els, b.len) : in;
    StrBuilder t;
    bool timeHit = false;
    for (int i = 0; i < len(cur);) {
        int save = i;
        uint32_t cp = Utf8Next(cur, &i);
        if (IsAsciiDigitCp(cp) && i < len(cur)) {
            int j = i;
            uint32_t c2 = Utf8Next(cur, &j);
            if (c2 == 0xFF1A && j < len(cur) &&
                IsAsciiDigitCp(Utf8At(cur, j))) {
                AppendCp(&t, cp);
                t.AppendChar(':');
                uint32_t c3 = Utf8Next(cur, &j);
                AppendCp(&t, c3);
                i = j;
                timeHit = true;
                continue;
            }
        }
        t.Append(Str(cur.s + save, i - save));
    }
    if (!changed && !timeHit) {
        return false;
    }
    *out = base::StrDup(a, timeHit ? Str(t.els, t.len) : cur);
    return true;
}

enum class ReplaceMode : uint8_t {
    Replace,
    PrefixSpace,
    SuffixSpace
};
enum class CharType : uint8_t {
    LeftQuote,
    RightQuote,
    Other
};

struct ReplaceRule {
    uint32_t from;
    uint32_t to;
    ReplaceMode mode;
    CharType type;
};

static const ReplaceRule kPunctuationMap[] = {
    {0xFF0C, ',', ReplaceMode::SuffixSpace, CharType::Other},
    {0x3001, ',', ReplaceMode::SuffixSpace, CharType::Other},
    {0x3002, '.', ReplaceMode::SuffixSpace, CharType::Other},
    {0xFF1A, ':', ReplaceMode::SuffixSpace, CharType::Other},
    {0xFF1B, '.', ReplaceMode::SuffixSpace, CharType::Other},
    {0xFF01, '!', ReplaceMode::SuffixSpace, CharType::Other},
    {0xFF1F, '?', ReplaceMode::SuffixSpace, CharType::Other},
    {0xFF08, '(', ReplaceMode::PrefixSpace, CharType::LeftQuote},
    {0x3010, '[', ReplaceMode::PrefixSpace, CharType::LeftQuote},
    {0x300C, '[', ReplaceMode::PrefixSpace, CharType::LeftQuote},
    {0x300A, 0x201C, ReplaceMode::PrefixSpace, CharType::LeftQuote},
    {0xFF09, ')', ReplaceMode::SuffixSpace, CharType::RightQuote},
    {0x3011, ']', ReplaceMode::SuffixSpace, CharType::RightQuote},
    {0x300D, ']', ReplaceMode::SuffixSpace, CharType::RightQuote},
    {0x300B, 0x201D, ReplaceMode::SuffixSpace, CharType::RightQuote},
};

static const ReplaceRule* RuleFor(uint32_t cp) {
    for (const ReplaceRule& r : kPunctuationMap) {
        if (r.from == cp) {
            return &r;
        }
    }
    return nullptr;
}

static bool IsEnglishSep(uint32_t cp) {
    return cp == ' ' || cp == ',' || cp == '.' || cp == '\'' || cp == '?' ||
           cp == '!' || cp == '&' || cp == ':';
}

static bool HasEnglishShape(Str s) {
    int i = 0;
    while (i < len(s)) {
        if (!IsWordCp(Utf8At(s, i))) {
            Utf8Next(s, &i);
            continue;
        }

        while (i < len(s) && IsWordCp(Utf8At(s, i))) {
            Utf8Next(s, &i);
        }

        int seps = 0;
        while (i < len(s) && IsEnglishSep(Utf8At(s, i))) {
            Utf8Next(s, &i);
            seps++;
        }

        if (seps > 0 && i < len(s) && IsWordCp(Utf8At(s, i))) {
            return true;
        }
    }
    return false;
}

static bool StartsWithWord(Str s) {
    int i = 0;
    while (i < len(s) && IsWhitespaceCp(Utf8At(s, i))) {
        Utf8Next(s, &i);
    }
    return i < len(s) && IsWordCp(Utf8At(s, i));
}

static bool IsQuoteCh(uint32_t cp) {
    return cp == '"' || cp == '\'' || cp == '`';
}

static bool IsQuoted(Str s) {
    int first = 0;
    while (first < len(s) && IsWhitespaceCp(Utf8At(s, first))) {
        Utf8Next(s, &first);
    }
    if (first >= len(s) || !IsQuoteCh(Utf8At(s, first))) {
        return false;
    }
    int last = -1;
    for (int i = first; i < len(s);) {
        int at = i;
        uint32_t cp = Utf8Next(s, &i);
        if (IsQuoteCh(cp)) {

            int j = i;
            bool tail = true;
            while (j < len(s)) {
                if (!IsWhitespaceCp(Utf8At(s, j))) {
                    tail = false;
                    break;
                }
                Utf8Next(s, &j);
            }
            if (tail) {
                last = at;
            }
        }
    }
    if (last <= first) {
        return false;
    }

    if (last == first + 1) {
        return false;
    }
    for (int i = first + 1; i < last; i++) {
        if (s.s[i] == '\n') {
            return false;
        }
    }
    return true;
}

static bool HasTwoLetters(Str s) {
    int run = 0;
    for (int i = 0; i < len(s); i++) {
        char c = s.s[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        run = letter ? run + 1 : 0;
        if (run >= 2) {
            return true;
        }
    }
    return false;
}

static bool LooksLikeCodeString(Str s) {
    for (int i = 0; i < len(s); i++) {
        char c = s.s[i];
        if ((c == '#' || c == '%' || c == '$') && i + 2 < len(s) &&
            s.s[i + 1] == '{') {
            for (int j = i + 3; j < len(s) && s.s[j] != '\n'; j++) {
                if (s.s[j] == '}') {
                    return true;
                }
            }
        }
    }
    int i = 0;
    while (i < len(s)) {
        if (!IsWordCp(Utf8At(s, i))) {
            Utf8Next(s, &i);
            continue;
        }
        while (i < len(s) && IsWordCp(Utf8At(s, i))) {
            Utf8Next(s, &i);
        }
        if (i + 1 < len(s) && s.s[i] == '.' && IsWordCp(Utf8At(s, i + 1))) {
            int j = i + 1;
            while (j < len(s) && IsWordCp(Utf8At(s, j))) {
                Utf8Next(s, &j);
            }
            if (j < len(s) && s.s[j] == '(') {
                return true;
            }
        }
    }
    return false;
}

static bool IsMayOnlyEnglish(Str text) {
    if (HasCjk(text)) {
        return false;
    }
    if (HasEnglishShape(text) && StartsWithWord(text)) {
        return true;
    }
    if (IsQuoted(text) && HasTwoLetters(text)) {

        if (LooksLikeCodeString(text)) {
            return false;
        }
        return true;
    }
    return false;
}

static void EscapeQuote(StrBuilder* out, uint32_t wrapQuote, uint32_t quote,
                        uint32_t* lastCp) {
    if ((quote == '"' || quote == '\'') && wrapQuote == quote) {
        out->AppendChar('\\');
    }
    AppendCp(out, quote);
    *lastCp = quote;
}

static bool FormatLine(Str line, uint32_t wrapQuote, StrBuilder* out) {
    if (!IsMayOnlyEnglish(line)) {
        out->Append(line);
        return false;
    }
    bool changed = false;
    uint32_t lastCp = 0;
    bool hasLast = false;
    for (int i = 0; i < len(line);) {
        uint32_t cp = Utf8Next(line, &i);
        const ReplaceRule* rule = RuleFor(cp);
        if (!rule) {
            AppendCp(out, cp);
            lastCp = cp;
            hasLast = true;
            continue;
        }
        bool hasNext = i < len(line);
        uint32_t next = hasNext ? Utf8At(line, i) : 0;

        if (!hasNext && rule->type == CharType::LeftQuote) {
            AppendCp(out, cp);
            lastCp = cp;
            hasLast = true;
            continue;
        }
        switch (rule->mode) {
            case ReplaceMode::SuffixSpace:
                EscapeQuote(out, wrapQuote, rule->to, &lastCp);
                hasLast = true;
                if (hasNext && IsAlnumCharCp(next)) {
                    out->AppendChar(' ');
                    lastCp = ' ';
                }
                break;
            case ReplaceMode::PrefixSpace:
                if (hasLast && IsAlnumCharCp(lastCp)) {
                    out->AppendChar(' ');
                }
                EscapeQuote(out, wrapQuote, rule->to, &lastCp);
                hasLast = true;
                break;
            case ReplaceMode::Replace:
            default:
                EscapeQuote(out, wrapQuote, rule->to, &lastCp);
                hasLast = true;
                break;
        }
        changed = true;
    }
    return changed;
}

bool FormatHalfwidthPunctuation(Arena* a, Str in, Str* out) {

    uint32_t wrapQuote = ' ';
    for (int i = 0; i < len(in);) {
        uint32_t cp = Utf8Next(in, &i);
        if (!IsWhitespaceCp(cp)) {
            wrapQuote = cp;
            break;
        }
    }
    StrBuilder b;
    bool changed = false;
    int lineStart = 0;
    for (int i = 0; i <= len(in); i++) {
        bool eol = i == len(in) || in.s[i] == '\n';
        if (!eol) {
            continue;
        }

        int end = i == len(in) ? i : i + 1;
        if (end > lineStart) {
            changed |= FormatLine(Str(in.s + lineStart, end - lineStart),
                                  wrapQuote, &b);
        }
        lineStart = end;
    }
    if (!changed) {
        return false;
    }
    *out = base::StrDup(a, Str(b.els, b.len));
    return true;
}

}

#line 1 "src/autocorrect/html.cpp"

namespace autocorrect {

static bool HtmlLitI(Str s, int i, const char* lit) {
    for (int k = 0; lit[k]; k++) {
        if (i + k >= len(s)) {
            return false;
        }
        char c = s.s[i + k];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != lit[k]) {
            return false;
        }
    }
    return true;
}

static int HtmlLitLen(const char* lit) {
    int n = 0;
    while (lit[n]) {
        n++;
    }
    return n;
}

static int MatchTag(Str s, int i) {
    if (i >= len(s) || s.s[i] != '<') {
        return -1;
    }
    char quote = 0;
    for (int at = i + 1; at < len(s); at++) {
        char c = s.s[at];
        if (quote) {
            if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '>') {
            return at + 1 - i;
        }
    }
    return -1;
}

static int MatchCloseTag(Str s, int i, const char* name) {
    if (!HtmlLitI(s, i, "</")) {
        return -1;
    }
    int at = i + 2;
    if (!HtmlLitI(s, at, name)) {
        return -1;
    }
    at += HtmlLitLen(name);
    while (at < len(s) && (s.s[at] == ' ' || s.s[at] == '\t' ||
                           s.s[at] == '\n' || s.s[at] == '\r')) {
        at++;
    }
    if (at >= len(s) || s.s[at] != '>') {
        return -1;
    }
    return at + 1 - i;
}

static int FindCloseTag(Str s, int from, const char* name, int* len) {
    for (int at = from; at < s.len; at++) {
        int n = MatchCloseTag(s, at, name);
        if (n > 0) {
            *len = n;
            return at;
        }
    }
    return -1;
}

static bool AtOpenTag(Str s, int i, const char* name) {
    if (i >= len(s) || s.s[i] != '<' || !HtmlLitI(s, i + 1, name)) {
        return false;
    }
    int after = i + 1 + HtmlLitLen(name);
    if (after >= len(s)) {
        return false;
    }
    char c = s.s[after];
    return c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '/';
}

void ScanHtml(Results* res, Str raw) {
    int ignoreStart = 0;
    int i = 0;
    auto flush = [&](int upTo) {
        if (upTo > ignoreStart) {
            EmitIgnore(res, Str(raw.s + ignoreStart, upTo - ignoreStart));
        }
    };
    while (i < len(raw)) {
        if (raw.s[i] != '<') {

            int start = i;
            while (i < len(raw) && raw.s[i] != '<') {
                i++;
            }
            flush(start);
            EmitText(res, StrL("text"), Str(raw.s + start, i - start));
            ignoreStart = i;
            continue;
        }

        if (HtmlLitI(raw, i, "<!--")) {
            int end = -1;
            for (int at = i + 4; at + 3 <= len(raw); at++) {
                if (raw.s[at] == '-' && raw.s[at + 1] == '-' &&
                    raw.s[at + 2] == '>') {
                    end = at + 3;
                    break;
                }
            }
            if (end > 0) {
                flush(i);
                EmitText(res, StrL("comment"), Str(raw.s + i, end - i));
                ignoreStart = end;
                i = end;
                continue;
            }
            i++;
            continue;
        }

        if (HtmlLitI(raw, i, "<%")) {
            int end = -1;
            for (int at = i + 2; at + 2 <= len(raw); at++) {
                if (raw.s[at] == '%' && raw.s[at + 1] == '>') {
                    end = at + 2;
                    break;
                }
            }
            if (end > 0) {
                i = end;
                continue;
            }
            i++;
            continue;
        }

        if (HtmlLitI(raw, i, "<code>")) {
            int closeLen = 0;
            int close = FindCloseTag(raw, i + 6, "code", &closeLen);
            if (close > 0) {
                i = close + closeLen;
                continue;
            }
        }

        struct RawEl {
            const char* name;
            const char* rule;
        };
        static const RawEl kRawEls[] = {
            {"script", "inline_javascript"},
            {"style", "inline_style"},
            {"title", nullptr},
            {"textarea", nullptr},
        };
        bool handled = false;
        for (const RawEl& el : kRawEls) {
            if (!AtOpenTag(raw, i, el.name)) {
                continue;
            }
            int tag = MatchTag(raw, i);
            if (tag < 0) {
                break;
            }
            int closeLen = 0;
            int close = FindCloseTag(raw, i + tag, el.name, &closeLen);
            if (close < 0) {
                break;
            }
            if (el.rule) {
                flush(i + tag);
                EmitInlineScript(res, Str(el.rule),
                                 Str(raw.s + i + tag, close - (i + tag)));
                ignoreStart = close;
            }
            i = close + closeLen;
            handled = true;
            break;
        }
        if (handled) {
            continue;
        }

        int tag = MatchTag(raw, i);
        if (tag > 0) {
            i += tag;
            continue;
        }

        i++;
    }
    flush(len(raw));
}

}

#line 1 "src/autocorrect/ignorer.cpp"

namespace autocorrect {

struct IgnorePattern {
    Str glob = {};
    bool negated = false;
    bool dirOnly = false;
    bool anchored = false;
};

static bool GlobMatch(const char* p, const char* pe, const char* t,
                      const char* te) {
    while (p < pe) {
        char c = *p;
        if (c == '*') {
            bool doubleStar = p + 1 < pe && p[1] == '*';
            if (doubleStar) {
                const char* rest = p + 2;

                if (rest < pe && *rest == '/') {
                    if (GlobMatch(rest + 1, pe, t, te)) {
                        return true;
                    }
                }
                for (const char* at = t; at <= te; at++) {
                    if (GlobMatch(rest, pe, at, te)) {
                        return true;
                    }
                }
                return false;
            }
            const char* rest = p + 1;
            for (const char* at = t; at <= te; at++) {
                if (GlobMatch(rest, pe, at, te)) {
                    return true;
                }
                if (at < te && *at == '/') {
                    break;
                }
            }
            return false;
        }
        if (t >= te) {
            return false;
        }
        if (c == '?') {
            if (*t == '/') {
                return false;
            }
            p++;
            t++;
            continue;
        }
        if (c == '[') {
            const char* cls = p + 1;
            bool negate = cls < pe && (*cls == '!' || *cls == '^');
            if (negate) {
                cls++;
            }
            bool hit = false;
            const char* k = cls;
            while (k < pe && *k != ']') {
                if (k + 2 < pe && k[1] == '-' && k[2] != ']') {
                    if (*t >= *k && *t <= k[2]) {
                        hit = true;
                    }
                    k += 3;
                } else {
                    if (*t == *k) {
                        hit = true;
                    }
                    k++;
                }
            }
            if (k >= pe) {

                if (*t != '[') {
                    return false;
                }
                p++;
                t++;
                continue;
            }
            if (hit == negate || *t == '/') {
                return false;
            }
            p = k + 1;
            t++;
            continue;
        }
        if (c != *t) {
            return false;
        }
        p++;
        t++;
    }
    return t == te;
}

static bool PatternMatches(const IgnorePattern& pat, Str path, bool isDir) {
    if (pat.dirOnly && !isDir) {
        return false;
    }
    const char* pe = pat.glob.s + pat.glob.len;
    const char* te = path.s + len(path);
    if (pat.anchored) {
        return GlobMatch(pat.glob.s, pe, path.s, te);
    }

    if (GlobMatch(pat.glob.s, pe, path.s, te)) {
        return true;
    }
    for (const char* at = path.s; at < te; at++) {
        if (*at == '/' && GlobMatch(pat.glob.s, pe, at + 1, te)) {
            return true;
        }
    }
    return false;
}

static int Matched(const Ignorer* ig, Str path, bool isDir) {
    for (int i = ig->nPatterns - 1; i >= 0; i--) {
        const IgnorePattern& pat = ig->patterns[i];
        if (PatternMatches(pat, path, isDir)) {
            return pat.negated ? -1 : 1;
        }
    }
    return 0;
}

static int MatchedOrParents(const Ignorer* ig, Str path, bool isDir) {
    int m = Matched(ig, path, isDir);
    if (m != 0) {
        return m;
    }
    int end = len(path);
    for (;;) {
        while (end > 0 && path.s[end - 1] != '/') {
            end--;
        }
        if (end == 0) {
            return 0;
        }
        end--;
        m = Matched(ig, Str(path.s, end), true);
        if (m != 0) {
            return m;
        }
    }
}

static void AddPatternsFromFile(base::Vec<IgnorePattern>& out, Str workDir,
                                Str name) {
    int dirLen = len(workDir);
    while (dirLen > 0 &&
           (workDir.s[dirLen - 1] == '/' || workDir.s[dirLen - 1] == '\\')) {
        dirLen--;
    }
    base::TempStr path = base::fmt("%s/%s", Str(workDir.s, dirLen), name);
    if (len(path) >= 1024) {
        return;
    }
    FILE* f = fopen(path.s, "rb");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    const long kMax = 1024 * 1024;
    if (size <= 0 || size > kMax) {
        fclose(f);
        return;
    }
    char* buf = (char*)base::Alloc(nullptr, (int)size);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    Str text(buf, (int)got);
    int lineStart = 0;
    for (int i = 0; i <= len(text); i++) {
        if (i < len(text) && text.s[i] != '\n') {
            continue;
        }
        int end = i;
        while (end > lineStart &&
               (text.s[end - 1] == '\r' || text.s[end - 1] == ' ' ||
                text.s[end - 1] == '\t')) {
            end--;
        }
        Str line(text.s + lineStart, end - lineStart);
        lineStart = i + 1;
        if (len(line) == 0 || line.s[0] == '#') {
            continue;
        }
        IgnorePattern pat;
        if (line.s[0] == '!') {
            pat.negated = true;
            line = Str(line.s + 1, len(line) - 1);
        }
        if (len(line) > 0 && line.s[len(line) - 1] == '/') {
            pat.dirOnly = true;
            line = Str(line.s, len(line) - 1);
        }

        if (len(line) > 0 && line.s[0] == '/') {
            pat.anchored = true;
            line = Str(line.s + 1, len(line) - 1);
        }
        if (len(line) == 0) {
            continue;
        }
        for (int k = 0; !pat.anchored && k < len(line); k++) {
            if (line.s[k] == '/') {
                pat.anchored = true;
            }
        }

        pat.glob = base::StrDup(line);
        base::VecAppend(out, pat);
    }
    base::Free(nullptr, buf);
}

void IgnorerInit(Ignorer* ig, Str workDir) {
    ig->patterns = nullptr;
    ig->nPatterns = 0;
    base::Vec<IgnorePattern> patterns;

    AddPatternsFromFile(patterns, workDir, StrL(".autocorrectignore"));
    AddPatternsFromFile(patterns, workDir, StrL(".gitignore"));
    if (len(patterns) == 0) {
        return;
    }
    ig->patterns = (IgnorePattern*)base::Alloc(
        nullptr, len(patterns) * (int)sizeof(IgnorePattern));
    if (!ig->patterns) {
        for (int i = 0; i < len(patterns); i++) {
            base::StrFree(patterns[i].glob);
        }
        return;
    }
    memcpy(ig->patterns, patterns.els,
           (size_t)len(patterns) * sizeof(IgnorePattern));
    ig->nPatterns = len(patterns);
}

bool IgnorerIsIgnored(const Ignorer* ig, Str relativePath) {
    if (!ig || ig->nPatterns == 0 || len(relativePath) == 0) {
        return false;
    }

    base::TempStr buf = base::AllocStrTemp(std::min(len(relativePath), 1024));
    int n = 0;
    int start = 0;
    if (len(relativePath) >= 2 && relativePath.s[0] == '.' &&
        (relativePath.s[1] == '/' || relativePath.s[1] == '\\')) {
        start = 2;
    }
    for (int i = start; i < len(relativePath) && n < len(buf); i++) {
        char c = relativePath.s[i];
        buf.s[n++] = c == '\\' ? '/' : c;
    }
    Str path(buf.s, n);

    return MatchedOrParents(ig, path, false) == 1 ||
           MatchedOrParents(ig, path, true) == 1;
}

void IgnorerFree(Ignorer* ig) {
    if (!ig) {
        return;
    }
    for (int i = 0; i < ig->nPatterns; i++) {
        base::StrFree(ig->patterns[i].glob);
    }
    base::Free(nullptr, ig->patterns);
    ig->patterns = nullptr;
    ig->nPatterns = 0;
}

}

#line 1 "src/autocorrect/markdown.cpp"

namespace autocorrect {

namespace {

enum class MdRule : uint8_t {
    Container,
    Block,
    Text,
    String,
    LinkString,
    MarkString,
    InnerText,
    Comment,
    Codeblock,
};

struct MdNode {
    MdRule rule = MdRule::Container;
    int start = 0;
    int end = 0;
    MdNode* firstChild = nullptr;
    MdNode* lastChild = nullptr;
    MdNode* next = nullptr;

    int langStart = 0;
    int langEnd = 0;
    int codeStart = 0;
    int codeEnd = 0;
};

struct MdParser {
    Str s = {};
    base::Arena* a = nullptr;
    int depth = 0;
};

const int kMaxDepth = 200;

MdNode* NewNode(MdParser* p, MdRule rule, int start) {
    MdNode* n = base::ArenaNew<MdNode>(p->a);
    n->rule = rule;
    n->start = start;
    return n;
}

void AddChild(MdNode* parent, MdNode* child) {
    if (!parent->firstChild) {
        parent->firstChild = child;
    } else {
        parent->lastChild->next = child;
    }
    parent->lastChild = child;
}

bool AtLit(Str s, int i, const char* lit) {
    for (int k = 0; lit[k]; k++) {
        if (i + k >= len(s) || s.s[i + k] != lit[k]) {
            return false;
        }
    }
    return true;
}

int MatchNewline(Str s, int i) {
    if (i < len(s) && s.s[i] == '\n') {
        return 1;
    }
    if (i + 1 < len(s) && s.s[i] == '\r' && s.s[i + 1] == '\n') {
        return 2;
    }
    return -1;
}

bool IsIdentifierCh(char c) {
    return c == '_' || c == '-' || c == '.' || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool MdIsAsciiAlnumCh(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

MdNode* ParseInline(MdParser* p, int* pos);

bool InlineStartsAt(MdParser* p, int i) {
    char c = i < len(p->s) ? p->s.s[i] : 0;
    if (c != '[' && c != '!' && c != '`' && c != '*' && c != '~' && c != '"') {
        return false;
    }
    int at = i;
    return ParseInline(p, &at) != nullptr;
}

int ScanString(MdParser* p, int i) {
    Str s = p->s;
    int at = i;
    while (at < len(s)) {
        char c = s.s[at];
        if (c == '\n' ||
            (c == '\r' && at + 1 < len(s) && s.s[at + 1] == '\n')) {
            break;
        }
        if ((c == '[' || c == '!' || c == '`' || c == '*' || c == '~' ||
             c == '"') &&
            InlineStartsAt(p, at)) {
            break;
        }
        at++;
    }
    return at;
}

MdNode* ParseWikilinks(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    if (!AtLit(s, i, "[[")) {
        return nullptr;
    }
    for (int at = i + 2; at + 1 < len(s); at++) {
        if (s.s[at] == ']' && s.s[at + 1] == ']') {
            MdNode* n = NewNode(p, MdRule::Container, i);
            n->end = at + 2;
            *pos = n->end;
            return n;
        }
    }
    return nullptr;
}

int MatchParen(Str s, int i) {
    if (i >= len(s) || s.s[i] != '(') {
        return -1;
    }
    int at = i + 1;
    if (at < len(s) && s.s[at] == ')') {
        return at + 1 - i;
    }
    auto inner = [&s](int from) {
        int j = from;
        while (j < len(s) && s.s[j] != '\n' && s.s[j] != '(' && s.s[j] != ')' &&
               !(s.s[j] == '\r' && j + 1 < len(s) && s.s[j + 1] == '\n')) {
            j++;
        }
        return j;
    };
    int j = inner(at);
    if (j == at) {
        return -1;
    }
    at = j;
    for (;;) {
        int sub = MatchParen(s, at);
        if (sub < 0) {
            break;
        }
        at += sub;
    }
    at = inner(at);
    if (at >= len(s) || s.s[at] != ')') {
        return -1;
    }
    return at + 1 - i;
}

MdNode* ParseMark(MdParser* p, int* pos);

MdNode* ParseLink(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    if (i >= len(s) || s.s[i] != '[') {
        return nullptr;
    }
    MdNode* n = NewNode(p, MdRule::Container, i);
    int at = i + 1;
    for (;;) {
        int save = at;
        MdNode* mark = ParseMark(p, &at);
        if (!mark) {
            at = save;
            break;
        }
        AddChild(n, mark);
    }
    int stringStart = at;
    while (at < len(s) && s.s[at] != ']') {
        at++;
    }
    if (at >= len(s)) {
        return nullptr;
    }
    MdNode* ls = NewNode(p, MdRule::LinkString, stringStart);
    ls->end = at;
    AddChild(n, ls);
    at++;
    int href = MatchParen(s, at);
    if (href > 0) {
        at += href;
    }
    n->end = at;
    *pos = at;
    return n;
}

MdNode* ParseCodeInline(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    if (i >= len(s) || s.s[i] != '`') {
        return nullptr;
    }
    int at = i + 1;
    while (at < len(s) && s.s[at] != '`' && s.s[at] != '\n' &&
           !(s.s[at] == '\r' && at + 1 < len(s) && s.s[at + 1] == '\n')) {
        at++;
    }
    if (at >= len(s) || s.s[at] != '`') {
        return nullptr;
    }
    MdNode* n = NewNode(p, MdRule::Container, i);
    n->end = at + 1;
    *pos = n->end;
    return n;
}

MdNode* ParseMark(MdParser* p, int* pos) {
    if (p->depth >= kMaxDepth) {
        return nullptr;
    }
    Str s = p->s;
    int i = *pos;
    MdNode* code = ParseCodeInline(p, pos);
    if (code) {
        return code;
    }
    static const char* const kMarks[] = {"***", "**", "*", "~~", "\""};
    for (const char* open : kMarks) {
        if (!AtLit(s, i, open)) {
            continue;
        }
        int openLen = 0;
        while (open[openLen]) {
            openLen++;
        }
        int at = i + openLen;
        MdNode* n = NewNode(p, MdRule::Container, i);
        p->depth++;
        int save = at;
        MdNode* nested = ParseMark(p, &at);
        p->depth--;
        if (nested) {
            AddChild(n, nested);
        } else {
            at = save;

            int stringStart = at;
            while (at < len(s) && !AtLit(s, at, open)) {
                char c = s.s[at];
                if ((c == '[' || c == '!' || c == '`' || c == '*' || c == '~' ||
                     c == '"')) {
                    p->depth++;
                    bool isInline = InlineStartsAt(p, at);
                    p->depth--;
                    if (isInline) {
                        break;
                    }
                }
                at++;
            }
            MdNode* ms = NewNode(p, MdRule::MarkString, stringStart);
            ms->end = at;
            AddChild(n, ms);
        }
        if (!AtLit(s, at, open)) {

            return nullptr;
        }
        n->end = at + openLen;
        *pos = n->end;
        return n;
    }
    return nullptr;
}

MdNode* ParseImg(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    if (i >= len(s) || s.s[i] != '!') {
        return nullptr;
    }
    int at = i + 1;
    MdNode* link = ParseLink(p, &at);
    if (!link) {
        return nullptr;
    }
    MdNode* n = NewNode(p, MdRule::Container, i);
    n->end = at;
    AddChild(n, link);
    *pos = at;
    return n;
}

MdNode* ParseInline(MdParser* p, int* pos) {
    if (p->depth >= kMaxDepth) {
        return nullptr;
    }
    p->depth++;
    MdNode* n = ParseWikilinks(p, pos);
    if (!n) {
        n = ParseImg(p, pos);
    }
    if (!n) {
        n = ParseLink(p, pos);
    }
    if (!n) {
        n = ParseCodeInline(p, pos);
    }
    if (!n) {
        n = ParseMark(p, pos);
    }
    p->depth--;
    return n;
}

MdNode* ParseComment(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    if (!AtLit(s, i, "<!--")) {
        return nullptr;
    }
    for (int at = i + 4; at + 3 <= len(s); at++) {
        if (AtLit(s, at, "-->")) {
            MdNode* n = NewNode(p, MdRule::Comment, i);
            n->end = at + 3;
            *pos = n->end;
            return n;
        }
    }
    return nullptr;
}

int MatchTagSelf(Str s, int i) {
    if (i >= len(s) || s.s[i] != '<') {
        return -1;
    }
    int at = i + 1;
    while (at < len(s)) {
        if (s.s[at] == '>') {
            return -1;
        }
        if (s.s[at] == '/' && at + 1 < len(s) && s.s[at + 1] == '>') {
            return at + 2 - i;
        }
        at++;
    }
    return -1;
}

int MatchTagStart(Str s, int i) {
    if (i >= len(s) || s.s[i] != '<') {
        return -1;
    }
    int at = i + 1;
    while (at < len(s)) {
        if (s.s[at] == '/') {
            return -1;
        }
        if (s.s[at] == '>') {
            return at + 1 - i;
        }
        at++;
    }
    return -1;
}

int MatchTagEnd(Str s, int i) {
    if (!AtLit(s, i, "</")) {
        return -1;
    }
    for (int at = i + 2; at < len(s); at++) {
        if (s.s[at] == '>') {
            return at + 1 - i;
        }
    }
    return -1;
}

MdNode* ParseHtml(MdParser* p, int* pos) {
    if (p->depth >= kMaxDepth) {
        return nullptr;
    }
    Str s = p->s;
    int i = *pos;
    int n = MatchTagSelf(s, i);
    if (n > 0) {
        MdNode* node = NewNode(p, MdRule::Container, i);
        node->end = i + n;
        *pos = node->end;
        return node;
    }
    n = MatchTagStart(s, i);
    if (n < 0) {
        return nullptr;
    }
    MdNode* node = NewNode(p, MdRule::Container, i);
    int at = i + n;

    while (at < len(s) && (s.s[at] == ' ' || MatchNewline(s, at) > 0)) {
        at += s.s[at] == ' ' ? 1 : MatchNewline(s, at);
    }

    for (;;) {
        p->depth++;
        int save = at;
        MdNode* sub = ParseHtml(p, &at);
        p->depth--;
        if (sub) {
            AddChild(node, sub);
            continue;
        }
        at = save;

        int textStart = at;
        while (at < len(s) && s.s[at] != '<' && s.s[at] != '>') {
            at++;
        }
        if (at == textStart) {
            break;
        }
        MdNode* text = NewNode(p, MdRule::InnerText, textStart);
        text->end = at;
        AddChild(node, text);
    }
    int end = MatchTagEnd(s, at);
    if (end < 0) {
        return nullptr;
    }
    node->end = at + end;
    *pos = node->end;
    return node;
}

int MatchMetaWrap(Str s, int i) {
    int at = i;
    while (at < len(s) && s.s[at] == '-') {
        at++;
    }
    return at - i >= 3 ? at - i : -1;
}

int MatchMetaKey(Str s, int i) {
    int at = i;
    while (at < len(s) && s.s[at] != ':' && IsIdentifierCh(s.s[at])) {
        at++;
    }
    if (at >= len(s) || s.s[at] != ':') {
        return -1;
    }
    at++;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    return at - i;
}

MdNode* ParseMetaInfo(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    int wrap = MatchMetaWrap(s, i);
    if (wrap < 0) {
        return nullptr;
    }
    int at = i + wrap;
    int nl = MatchNewline(s, at);
    if (nl < 0) {
        return nullptr;
    }
    at += nl;
    MdNode* node = NewNode(p, MdRule::Container, i);
    for (;;) {
        int save = at;
        int key = MatchMetaKey(s, at);
        if (key < 0) {
            break;
        }
        int valueStart = at + key;
        int valueEnd = ScanString(p, valueStart);
        if (valueEnd == valueStart) {
            at = save;
            break;
        }
        int pairNl = MatchNewline(s, valueEnd);
        if (pairNl < 0) {
            at = save;
            break;
        }
        MdNode* value = NewNode(p, MdRule::String, valueStart);
        value->end = valueEnd;
        AddChild(node, value);
        at = valueEnd + pairNl;
    }
    wrap = MatchMetaWrap(s, at);
    if (wrap < 0) {
        return nullptr;
    }
    at += wrap;
    for (;;) {
        int n = MatchNewline(s, at);
        if (n < 0) {
            break;
        }
        at += n;
    }
    node->end = at;
    *pos = at;
    return node;
}

bool IsMetaTagsItemCh(Str s, int* i) {
    int at = *i;
    if (at >= len(s)) {
        return false;
    }
    char c = s.s[at];
    if (c == ',' || c == '\n' || (c == '\r' && MatchNewline(s, at) > 0)) {
        return false;
    }
    if (c == ' ' || MdIsAsciiAlnumCh(c)) {
        *i = at + 1;
        return true;
    }
    int next = at;
    uint32_t cp = Utf8Next(s, &next);
    if (IsCjk(cp)) {
        *i = next;
        return true;
    }
    return false;
}

int MatchMetaTags(Str s, int i) {
    int key = MatchMetaKey(s, i);
    if (key < 0) {
        return -1;
    }
    int at = i + key;
    int commas = 0;
    for (;;) {
        while (IsMetaTagsItemCh(s, &at)) {
        }
        int save = at;
        while (at < len(s) && s.s[at] == ' ') {
            at++;
        }
        if (at < len(s) && s.s[at] == ',') {
            at++;
            while (at < len(s) && s.s[at] == ' ') {
                at++;
            }
            commas++;
            continue;
        }
        at = save;
        break;
    }
    if (commas == 0) {
        return -1;
    }
    int nl = MatchNewline(s, at);
    if (nl < 0) {
        return -1;
    }
    return at + nl - i;
}

int MatchHr(Str s, int i) {
    int at = i;
    while (at < len(s) && s.s[at] == '-') {
        at++;
    }
    return at - i >= 3 ? at - i : -1;
}

int MatchIndent(Str s, int i) {
    if (i < len(s) && s.s[i] == '\t') {
        return 1;
    }
    int at = i;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    return at - i >= 4 ? at - i : -1;
}

MdNode* ParseCodeblock(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    if (AtLit(s, i, "```")) {
        int at = i + 3;
        int langStart = at;
        while (at < len(s) && IsIdentifierCh(s.s[at])) {
            at++;
        }
        int langEnd = at;
        int codeStart = at;
        while (at < len(s) && !AtLit(s, at, "```")) {
            at++;
        }
        if (at >= len(s)) {
            return nullptr;
        }
        MdNode* n = NewNode(p, MdRule::Codeblock, i);
        n->langStart = langStart;
        n->langEnd = langEnd;
        n->codeStart = codeStart;
        n->codeEnd = at;
        n->end = at + 3;
        *pos = n->end;
        return n;
    }
    int at = i;
    int lines = 0;
    for (;;) {
        int save = at;
        int indent = MatchIndent(s, at);
        if (indent < 0) {
            break;
        }
        int j = at + indent;
        while (j < len(s) && s.s[j] != '\n' &&
               !(s.s[j] == '\r' && j + 1 < len(s) && s.s[j + 1] == '\n')) {
            j++;
        }
        int nl = MatchNewline(s, j);
        if (nl < 0) {
            at = save;
            break;
        }
        at = j + nl;
        lines++;
    }
    if (lines == 0) {
        return nullptr;
    }
    MdNode* n = NewNode(p, MdRule::Codeblock, i);
    n->end = at;

    n->langStart = n->langEnd = n->codeStart = n->codeEnd = i;
    *pos = at;
    return n;
}

int ParseInlineOrStringSeq(MdParser* p, int* pos, MdNode* parent) {
    int matched = 0;
    for (;;) {
        int save = *pos;
        MdNode* inl = ParseInline(p, pos);
        if (inl) {
            AddChild(parent, inl);
            matched++;
            continue;
        }
        *pos = save;
        int end = ScanString(p, *pos);
        if (end == *pos) {
            break;
        }
        MdNode* str = NewNode(p, MdRule::String, *pos);
        str->end = end;
        AddChild(parent, str);
        *pos = end;
        matched++;
    }
    return matched;
}

int MatchListPrefix(Str s, int i) {
    int at = i;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    if (at >= len(s)) {
        return -1;
    }
    char c = s.s[at];
    if (c == '*' || c == '-') {
        at++;
    } else if (c >= '0' && c <= '9' && at + 1 < len(s) && s.s[at + 1] == '.') {
        at += 2;
    } else if (c == '[' && at + 2 < len(s) &&
               (s.s[at + 1] == ' ' || s.s[at + 1] == 'x' ||
                s.s[at + 1] == 'X') &&
               s.s[at + 2] == ']') {
        at += 3;
    } else {
        return -1;
    }
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    return at - i;
}

int MatchNewlinePlus(Str s, int i) {
    int at = i;
    int count = 0;
    for (;;) {
        int n = MatchNewline(s, at);
        if (n < 0) {
            break;
        }
        at += n;
        count++;
    }
    return count > 0 ? at - i : -1;
}

MdNode* ParseListItem(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    int prefix = MatchListPrefix(s, i);
    if (prefix < 0) {
        return nullptr;
    }
    MdNode* n = NewNode(p, MdRule::Container, i);
    int at = i + prefix;
    if (ParseInlineOrStringSeq(p, &at, n) == 0) {
        return nullptr;
    }
    int nl = MatchNewlinePlus(s, at);
    if (nl < 0) {
        return nullptr;
    }
    n->end = at + nl;
    *pos = n->end;
    return n;
}

MdNode* ParseListParagraph(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    int indent = MatchIndent(s, i);
    if (indent < 0) {
        return nullptr;
    }
    MdNode* n = NewNode(p, MdRule::Container, i);
    int at = i + indent;
    ParseInlineOrStringSeq(p, &at, n);
    int nl = MatchNewlinePlus(s, at);
    if (nl < 0) {
        return nullptr;
    }
    n->end = at + nl;
    *pos = n->end;
    return n;
}

MdNode* ParseList(MdParser* p, int* pos) {
    int i = *pos;
    int at = i;
    MdNode* first = ParseListItem(p, &at);
    if (!first) {
        return nullptr;
    }
    MdNode* n = NewNode(p, MdRule::Container, i);
    AddChild(n, first);
    for (;;) {
        int save = at;
        MdNode* item = ParseListItem(p, &at);
        if (!item) {
            at = save;
            item = ParseListParagraph(p, &at);
        }
        if (!item) {
            at = save;
            break;
        }
        AddChild(n, item);
    }
    n->end = at;
    *pos = at;
    return n;
}

MdNode* ParseBlockItem(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    int at = i;
    if (at < len(s) && s.s[at] == '>') {
        at++;
    } else {
        int hashes = 0;
        while (at < len(s) && s.s[at] == '#' && hashes < 6) {
            at++;
            hashes++;
        }
        if (hashes == 0) {
            return nullptr;
        }
    }
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    MdNode* n = NewNode(p, MdRule::Container, i);
    if (ParseInlineOrStringSeq(p, &at, n) == 0) {
        return nullptr;
    }
    n->end = at;
    *pos = at;
    return n;
}

int MatchText(MdParser* p, int i) {
    int at = ScanString(p, i);
    if (at == i) {
        return -1;
    }
    for (;;) {
        int nl = MatchNewline(p->s, at);
        if (nl < 0) {
            break;
        }
        int next = ScanString(p, at + nl);
        if (next == at + nl) {
            break;
        }
        at = next;
    }
    return at - i;
}

MdNode* ParseParagraph(MdParser* p, int* pos) {
    int i = *pos;
    MdNode* n = NewNode(p, MdRule::Container, i);
    int matched = 0;
    for (;;) {
        int save = *pos;
        MdNode* inl = ParseInline(p, pos);
        if (inl) {
            AddChild(n, inl);
            matched++;
            continue;
        }
        *pos = save;
        int text = MatchText(p, *pos);
        if (text < 0) {
            break;
        }
        MdNode* t = NewNode(p, MdRule::Text, *pos);
        t->end = *pos + text;
        AddChild(n, t);
        *pos = t->end;
        matched++;
    }
    if (matched == 0) {
        return nullptr;
    }
    n->end = *pos;
    return n;
}

MdNode* ParseBlock(MdParser* p, int* pos) {
    Str s = p->s;
    int i = *pos;
    MdNode* block = NewNode(p, MdRule::Block, i);
    int tags = MatchMetaTags(s, i);
    if (tags > 0) {
        block->end = i + tags;
        *pos = block->end;
        return block;
    }
    int hr = MatchHr(s, i);
    if (hr > 0) {
        block->end = i + hr;
        *pos = block->end;
        return block;
    }
    int at = i;
    MdNode* sub = ParseList(p, &at);
    if (!sub) {
        at = i;
        sub = ParseCodeblock(p, &at);
    }
    if (!sub) {
        at = i;
        sub = ParseBlockItem(p, &at);
    }
    if (!sub) {
        at = i;
        sub = ParseParagraph(p, &at);
    }
    if (!sub) {
        return nullptr;
    }
    AddChild(block, sub);
    block->end = at;
    *pos = at;
    return block;
}

int MatchTdTag(Str s, int i) {
    int at = i;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    if (at >= len(s) || s.s[at] != '|') {
        return -1;
    }
    at++;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    return at - i;
}

void WalkNode(Results* res, Str raw, const MdNode* n);

void WalkChildren(Results* res, Str raw, const MdNode* n) {
    int at = n->start;
    for (const MdNode* child = n->firstChild; child; child = child->next) {
        if (child->start > at) {
            EmitIgnore(res, Str(raw.s + at, child->start - at));
        }
        WalkNode(res, raw, child);
        at = child->end;
    }
    if (n->end > at) {
        EmitIgnore(res, Str(raw.s + at, n->end - at));
    }
}

void WalkNode(Results* res, Str raw, const MdNode* n) {
    Str span(raw.s + n->start, n->end - n->start);
    switch (n->rule) {
        case MdRule::Text:
            EmitText(res, StrL("text"), span);
            return;
        case MdRule::String:
            EmitText(res, StrL("string"), span);
            return;
        case MdRule::LinkString:
            EmitText(res, StrL("link_string"), span);
            return;
        case MdRule::MarkString:
            EmitText(res, StrL("mark_string"), span);
            return;
        case MdRule::InnerText:
            EmitText(res, StrL("inner_text"), span);
            return;
        case MdRule::Comment:
            EmitText(res, StrL("comment"), span);
            return;
        case MdRule::Codeblock: {
            Str lang(raw.s + n->langStart, n->langEnd - n->langStart);
            Str code(raw.s + n->codeStart, n->codeEnd - n->codeStart);
            EmitCodeblock(res, span, lang, code);
            return;
        }
        case MdRule::Block: {

            bool cjk = HasCjk(span);
            Toggle saved = {};
            if (cjk) {
                saved = ResultsPushCodeblockToggle(res);
            }
            WalkChildren(res, raw, n);
            if (cjk && saved.kind != ToggleKind::None) {
                res->toggle = saved;
            }
            return;
        }
        case MdRule::Container:
        default:
            if (!n->firstChild) {
                EmitIgnore(res, span);
                return;
            }
            WalkChildren(res, raw, n);
            return;
    }
}

}

void ScanMarkdown(Results* res, Str raw) {
    MdParser p;
    p.s = raw;
    p.a = res->a;
    MdNode* root = NewNode(&p, MdRule::Container, 0);
    root->end = len(raw);
    int pos = 0;
    while (pos < len(raw)) {

        int save = pos;
        MdNode* n = ParseComment(&p, &pos);
        if (!n) {
            pos = save;
            n = ParseHtml(&p, &pos);
        }
        if (!n) {
            pos = save;
            n = ParseMetaInfo(&p, &pos);
        }
        if (!n) {
            pos = save;
            n = ParseBlock(&p, &pos);
        }
        if (!n) {
            pos = save;
            n = ParseInline(&p, &pos);
        }
        if (!n) {
            pos = save;
            int td = MatchTdTag(raw, pos);
            if (td > 0) {
                pos += td;
                continue;
            }
            int nl = MatchNewline(raw, pos);
            if (nl > 0) {
                pos += nl;
                continue;
            }

            pos++;
            continue;
        }
        AddChild(root, n);
    }
    WalkNode(res, raw, root);
}

}

#line 1 "src/autocorrect/rule.cpp"

namespace autocorrect {

static bool IsAsciiAlnumCh(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static bool IsPathCh(char c) {
    return IsAsciiAlnumCh(c) || c == '-' || c == '_' || c == '.';
}

static bool IsMatchPath(Str s) {

    int i = 0;
    while (i < len(s) && IsAsciiAlnumCh(s.s[i])) {
        i++;
    }
    if (i > 0 && i + 2 < len(s) && s.s[i] == ':' && s.s[i + 1] == '/' &&
        s.s[i + 2] == '/') {
        return true;
    }

    i = 0;
    if (i < len(s) && s.s[i] == '/') {
        i++;
    }
    int start = i;
    while (i < len(s) && IsPathCh(s.s[i])) {
        i++;
    }
    return i - start >= 2 && i < len(s) && s.s[i] == '/';
}

static bool IsWordDashDotCp(uint32_t cp) {
    return cp == '-' || cp == '_' || cp == '.' || IsWordCp(cp);
}

static bool IsMatchPathHash(Str s) {
    s = base::StrTrimAscii(s);
    for (int i = 0; i < len(s); i++) {
        if (!IsPathCh(s.s[i])) {
            continue;
        }
        int j = i;
        while (j < len(s) && IsPathCh(s.s[j])) {
            j++;
        }
        if (j >= len(s) || s.s[j] != '#') {
            i = j;
            continue;
        }

        int k = j + 1;
        while (k < len(s)) {
            int at = k;
            uint32_t cp = Utf8Next(s, &at);
            if (!IsWordDashDotCp(cp)) {
                break;
            }
            if (IsHan(cp)) {
                return true;
            }
            k = at;
        }
        i = j;
    }
    return false;
}

using RuleFn = bool (*)(Arena*, Str, Str*);

struct RuleDef {
    int id;
    RuleFn fn;
};

static const RuleDef kRules[] = {
    {kRuleSpaceWord, FormatSpaceWord},
    {kRuleSpacePunctuation, FormatSpacePunctuation},
    {kRuleSpaceBracket, FormatSpaceBracket},
    {kRuleSpaceDash, FormatSpaceDash},
    {kRuleSpaceBackticks, FormatSpaceBackticks},
    {kRuleSpaceDollar, FormatSpaceDollar},
    {kRuleFullwidth, FormatFullwidth},
};

static const RuleDef kAfterRules[] = {
    {kRuleHalfwidthWord, FormatHalfwidthWord},
    {kRuleHalfwidthPunctuation, FormatHalfwidthPunctuation},
    {kRuleNoSpaceFullwidth, FormatNoSpaceFullwidth},
    {kRuleNoSpaceFullwidthQuote, FormatNoSpaceFullwidthQuote},
};

static void ApplyRule(Arena* a, const RuleDef& rule, bool lint,
                      uint16_t disableMask, RuleResult* result) {
    if (disableMask & (uint16_t)(1u << rule.id)) {
        return;
    }
    SeverityMode mode = RuleSeverity(rule.id);
    if (lint) {
        if (mode == SeverityMode::Off) {
            return;
        }
        Str neu;
        if (rule.fn(a, result->out, &neu)) {
            if (result->severity == Severity::Pass) {
                result->severity = mode == SeverityMode::Warning
                                       ? Severity::Warning
                                       : Severity::Error;
            }
            result->out = neu;
        }
        return;
    }
    if (mode != SeverityMode::Error) {
        return;
    }
    Str neu;
    if (rule.fn(a, result->out, &neu)) {
        result->severity = Severity::Error;
        result->out = neu;
    }
}

static void FormatPart(Arena* a, bool lint, uint16_t disableMask,
                       RuleResult* result) {
    if (IsMatchPath(result->out) || IsMatchPathHash(result->out)) {
        return;
    }
    for (const RuleDef& rule : kRules) {
        ApplyRule(a, rule, lint, disableMask, result);
    }
}

RuleResult FormatOrLintText(Arena* a, Str text, bool lint,
                            uint16_t disableMask) {
    RuleResult result;
    if (HasCjk(text)) {

        StrBuilder joined;
        Severity severity = Severity::Pass;
        int start = 0;
        for (int i = 0; i < len(text); i++) {
            char c = text.s[i];
            if (c != ' ' && c != '\n' && c != '\r') {
                continue;
            }
            RuleResult sub;
            sub.out = Str(text.s + start, i + 1 - start);
            sub.severity = severity;
            FormatPart(a, lint, disableMask, &sub);
            joined.Append(sub.out);
            severity = sub.severity;
            start = i + 1;
        }
        if (start < len(text)) {
            RuleResult sub;
            sub.out = Str(text.s + start, len(text) - start);
            sub.severity = severity;
            FormatPart(a, lint, disableMask, &sub);
            joined.Append(sub.out);
            severity = sub.severity;
        }
        result.out = base::StrDup(a, Str(joined.els, joined.len));
        result.severity = severity;
    } else {
        result.out = text;
    }
    for (const RuleDef& rule : kAfterRules) {
        ApplyRule(a, rule, lint, disableMask, &result);
    }
    return result;
}

Str Format(Arena* a, Str text) {
    RuleResult r = FormatOrLintText(a, text, false, 0);

    return base::StrDup(a, r.out);
}

}

#line 1 "src/autocorrect/source.cpp"

namespace autocorrect {

static int LitLen(Str s, int i, const char* lit) {
    Str literal = Str(lit);
    if (i + len(literal) > len(s) ||
        !StrEq(Str(s.s + i, len(literal)), literal)) {
        return -1;
    }
    return len(literal);
}

static int MatchLineComment(Str s, int i, const char* prefix) {
    int n = LitLen(s, i, prefix);
    if (n < 0) {
        return -1;
    }
    int at = i + n;
    while (at < len(s) && s.s[at] != '\n') {
        at++;
    }
    return at - i;
}

static int MatchBlock(Str s, int i, const char* open, const char* close) {
    int n = LitLen(s, i, open);
    if (n < 0) {
        return -1;
    }
    Str closing = Str(close);
    int closeLen = len(closing);
    for (int at = i + n; at + closeLen <= len(s); at++) {
        if (StrEq(Str(s.s + at, closeLen), closing)) {
            return at + closeLen - i;
        }
    }
    return -1;
}

static int MatchSingleLine(Str s, int i, char q) {
    if (i >= len(s) || s.s[i] != q) {
        return -1;
    }
    for (int at = i + 1; at < len(s) && s.s[at] != '\n'; at++) {
        if (s.s[at] == q) {
            return at + 1 - i;
        }
    }
    return -1;
}

static int MatchMultiLine(Str s, int i, char q) {
    if (i >= len(s) || s.s[i] != q) {
        return -1;
    }
    for (int at = i + 1; at < len(s); at++) {
        if (s.s[at] == q) {
            return at + 1 - i;
        }
    }
    return -1;
}

using StringFn = int (*)(Str, int);

static int MatchCallWithString(Str s, int i, const char* fn,
                               StringFn matchString) {
    int n = LitLen(s, i, fn);
    if (n < 0) {
        return -1;
    }
    int at = i + n;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    int sn = matchString(s, at);
    if (sn < 0) {
        return -1;
    }
    at += sn;
    while (at < len(s) && s.s[at] != ')') {
        at++;
    }
    if (at >= len(s)) {
        return -1;
    }
    return at + 1 - i;
}

struct Alt {
    int (*match)(Str, int);
    const char* rule;
};

static void ScanAlts(Results* res, Str raw, const Alt* alts, int nAlts) {
    int ignoreStart = 0;
    int i = 0;
    while (i < len(raw)) {
        int matched = -1;
        const Alt* hit = nullptr;
        for (int k = 0; k < nAlts; k++) {
            matched = alts[k].match(raw, i);
            if (matched > 0) {
                hit = &alts[k];
                break;
            }
        }
        if (!hit) {
            i++;
            continue;
        }
        if (hit->rule) {
            if (i > ignoreStart) {
                EmitIgnore(res, Str(raw.s + ignoreStart, i - ignoreStart));
            }
            EmitText(res, Str(hit->rule), Str(raw.s + i, matched));
            ignoreStart = i + matched;
        }

        i += matched;
    }
    if (len(raw) > ignoreStart) {
        EmitIgnore(res, Str(raw.s + ignoreStart, len(raw) - ignoreStart));
    }
}

static int CppLineComment(Str s, int i) {
    return MatchLineComment(s, i, "//");
}
static int CppBlockComment(Str s, int i) {
    return MatchBlock(s, i, "/*", "*/");
}
static int HashLineComment(Str s, int i) {
    return MatchLineComment(s, i, "#");
}
static int DoubleQuoteSingleLine(Str s, int i) {
    return MatchSingleLine(s, i, '"');
}
static int SingleQuoteSingleLine(Str s, int i) {
    return MatchSingleLine(s, i, '\'');
}

static int RustRegexp(Str s, int i) {
    if (LitLen(s, i, "r\"") < 0) {
        return -1;
    }
    int n = MatchSingleLine(s, i + 1, '"');
    return n > 0 ? 1 + n : -1;
}

static int RustString(Str s, int i) {
    int n = MatchMultiLine(s, i, '"');
    if (n > 0) {
        return n;
    }

    if (i >= len(s) || s.s[i] != 'r') {
        return -1;
    }
    int hashes = 0;
    int at = i + 1;
    while (at < len(s) && s.s[at] == '#') {
        at++;
        hashes++;
    }
    if (hashes == 0 || at >= len(s) || s.s[at] != '"') {
        return -1;
    }
    at++;

    for (; at < len(s); at++) {
        bool atHashes = at + hashes <= len(s);
        for (int h = 0; atHashes && h < hashes; h++) {
            atHashes = s.s[at + h] == '#';
        }
        if (atHashes) {
            break;
        }
    }
    if (at >= len(s) || s.s[at] != '"') {
        return -1;
    }
    at++;
    for (int h = 0; h < hashes; h++) {
        if (at >= len(s) || s.s[at] != '#') {
            return -1;
        }
        at++;
    }
    return at - i;
}

void ScanRust(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
        {RustRegexp, nullptr},
        {RustString, "string"},
    };
    ScanAlts(res, raw, kAlts, 4);
}

static int CInclude(Str s, int i) {
    int n = LitLen(s, i, "#include");
    if (n < 0) {
        return -1;
    }
    int at = i + n;
    int spaces = 0;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
        spaces++;
    }
    if (spaces == 0) {
        return -1;
    }
    int sn = MatchSingleLine(s, at, '"');
    return sn > 0 ? at + sn - i : -1;
}

void ScanC(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
        {CInclude, nullptr},
        {DoubleQuoteSingleLine, "string"},
    };
    ScanAlts(res, raw, kAlts, 4);
}

static int ObjcString(Str s, int i) {
    if (LitLen(s, i, "@\"") < 0) {
        return -1;
    }
    int n = MatchSingleLine(s, i + 1, '"');
    return n > 0 ? 1 + n : -1;
}

static int ObjcSkipBlank(Str s, int i) {
    while (i < len(s) && ((uint8_t)s.s[i] == ' ' || s.s[i] == '\t' ||
                          s.s[i] == '\n' || s.s[i] == '\r')) {
        i++;
    }
    return i;
}

static int ObjcIgnoreString(Str s, int i) {
    static const char* const kMethods[] = {"NSRegularExpression",
                                           "NSLocalizedString", "Match"};
    for (const char* m : kMethods) {
        int n = LitLen(s, i, m);
        if (n < 0) {
            continue;
        }
        int at = i + n;
        if (at >= len(s) || s.s[at] != '(') {
            continue;
        }
        at = ObjcSkipBlank(s, at + 1);
        int sn = ObjcString(s, at);
        if (sn > 0) {
            return at + sn - i;
        }
    }
    static const char* const kArgs[] = {"WithPattern", "WithKey"};
    for (const char* arg : kArgs) {
        int n = LitLen(s, i, arg);
        if (n < 0) {
            continue;
        }
        int at = i + n;
        if (at >= len(s) || s.s[at] != ':') {
            continue;
        }
        at = ObjcSkipBlank(s, at + 1);
        int sn = ObjcString(s, at);
        if (sn > 0) {
            return at + sn - i;
        }
    }
    return -1;
}

void ScanObjectiveC(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"},
        {ObjcIgnoreString, nullptr},
        {ObjcString, "string"},
    };
    ScanAlts(res, raw, kAlts, 3);
}

static int PyTripleQuoteComment(Str s, int i) {
    return MatchBlock(s, i, "'''", "'''");
}
static int PyString(Str s, int i) {
    int n = MatchSingleLine(s, i, '\'');
    if (n > 0) {
        return n;
    }
    n = MatchBlock(s, i, "\"\"\"", "\"\"\"");
    if (n > 0) {

        int at = i + n;
        while (at < len(s) && s.s[at] == '"') {
            at++;
        }
        return at - i;
    }
    return MatchSingleLine(s, i, '"');
}
static int PyRegexp(Str s, int i) {
    if (i < len(s) && s.s[i] == 'r') {
        int n = PyString(s, i + 1);
        if (n > 0) {
            return 1 + n;
        }
    }
    return MatchCallWithString(s, i, "compile(", PyString);
}

void ScanPython(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {HashLineComment, "COMMENT"},
        {PyTripleQuoteComment, "COMMENT"},
        {PyRegexp, nullptr},
        {PyString, "string"},
    };
    ScanAlts(res, raw, kAlts, 4);
}

static int RubyString(Str s, int i) {
    int n = MatchSingleLine(s, i, '\'');
    return n > 0 ? n : MatchSingleLine(s, i, '"');
}
static int RubyRegexp(Str s, int i) {
    int n = MatchSingleLine(s, i, '/');
    if (n > 0) {
        return n;
    }
    n = LitLen(s, i, "%r{");
    if (n > 0) {
        for (int at = i + n; at < len(s) && s.s[at] != '\n'; at++) {
            if (s.s[at] == '}') {
                return at + 1 - i;
            }
        }
        return -1;
    }
    return MatchCallWithString(s, i, "Regexp.new(", RubyString);
}

void ScanRuby(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {HashLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
        {RubyString, "string"},
        {RubyRegexp, nullptr},
    };
    ScanAlts(res, raw, kAlts, 4);
}

static int GoString(Str s, int i) {
    char q = i < len(s) ? s.s[i] : 0;
    if (q != '"' && q != '`') {
        return -1;
    }
    for (int at = i + 1; at < len(s); at++) {
        char c = s.s[at];
        if (c == q) {
            return at + 1 - i;
        }
        if (q == '"' && c == '\n') {
            return -1;
        }
        if (c == '%' && at + 1 < len(s) &&
            (s.s[at + 1] == 's' || s.s[at + 1] == 'q' || s.s[at + 1] == 'v')) {
            return -1;
        }
    }
    return -1;
}

static int GoCall(Str s, int i, const char* pkg) {
    int n = LitLen(s, i, pkg);
    if (n < 0) {
        return -1;
    }
    int at = i + n;
    int letters = 0;
    while (at < len(s) && ((s.s[at] >= 'a' && s.s[at] <= 'z') ||
                           (s.s[at] >= 'A' && s.s[at] <= 'Z'))) {
        at++;
        letters++;
    }
    if (letters == 0 || at >= len(s) || s.s[at] != '(') {
        return -1;
    }
    at++;
    while (at < len(s) && s.s[at] == ' ') {
        at++;
    }
    int sn = GoString(s, at);
    if (sn < 0) {
        return -1;
    }
    at += sn;
    while (at < len(s) && s.s[at] != ')') {
        at++;
    }
    return at < len(s) ? at + 1 - i : -1;
}

static int GoRegexp(Str s, int i) {
    return GoCall(s, i, "regexp.");
}
static int GoTimeParse(Str s, int i) {
    return GoCall(s, i, "time.");
}

void ScanGo(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"}, {CppBlockComment, "COMMENT"},
        {GoRegexp, nullptr},         {GoTimeParse, nullptr},
        {GoString, "string"},
    };
    ScanAlts(res, raw, kAlts, 5);
}

static int SqlLineComment(Str s, int i) {
    return MatchLineComment(s, i, "--");
}

void ScanSql(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {SqlLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
        {SingleQuoteSingleLine, "string"},
    };
    ScanAlts(res, raw, kAlts, 3);
}

void ScanCss(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
    };
    ScanAlts(res, raw, kAlts, 2);
}

void ScanConf(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {HashLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
    };
    ScanAlts(res, raw, kAlts, 2);
}

static int JavaString(Str s, int i) {
    int n = MatchBlock(s, i, "\"\"\"", "\"\"\"");
    return n > 0 ? n : MatchSingleLine(s, i, '"');
}
static int JavaRegexp(Str s, int i) {
    return GoCall(s, i, "Pattern.");
}

void ScanJava(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
        {JavaRegexp, nullptr},
        {JavaString, "string"},
    };
    ScanAlts(res, raw, kAlts, 4);
}

static int CsString(Str s, int i) {
    if (LitLen(s, i, "@\"") >= 0) {
        int n = MatchMultiLine(s, i + 1, '"');
        return n > 0 ? 1 + n : -1;
    }
    if (LitLen(s, i, "$\"") >= 0) {
        int n = MatchSingleLine(s, i + 1, '"');
        return n > 0 ? 1 + n : -1;
    }
    return MatchSingleLine(s, i, '"');
}
static int CsRegexp(Str s, int i) {
    return MatchCallWithString(s, i, "Regex(", CsString);
}

void ScanCsharp(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
        {CsRegexp, nullptr},
        {CsString, "string"},
    };
    ScanAlts(res, raw, kAlts, 4);
}

static int SwiftString(Str s, int i) {
    int n = MatchBlock(s, i, "\"\"\"", "\"\"\"");
    return n > 0 ? n : MatchSingleLine(s, i, '"');
}
static int SwiftIgnoreString(Str s, int i) {
    static const char* const kMethods[] = {"NSRegularExpression",
                                           "NSLocalizedString", "Match"};
    for (const char* m : kMethods) {
        int n = LitLen(s, i, m);
        if (n < 0) {
            continue;
        }
        int at = i + n;
        if (at >= len(s) || s.s[at] != '(') {
            continue;
        }
        at = ObjcSkipBlank(s, at + 1);
        int sn = SwiftString(s, at);
        if (sn > 0) {
            return at + sn - i;
        }
    }
    static const char* const kArgs[] = {"pattern", "key"};
    for (const char* arg : kArgs) {
        int n = LitLen(s, i, arg);
        if (n < 0) {
            continue;
        }
        int at = i + n;
        if (at >= len(s) || s.s[at] != ':') {
            continue;
        }
        at = ObjcSkipBlank(s, at + 1);
        int sn = SwiftString(s, at);
        if (sn > 0) {
            return at + sn - i;
        }
    }
    return -1;
}

void ScanSwift(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
        {SwiftIgnoreString, nullptr},
        {SwiftString, "string"},
    };
    ScanAlts(res, raw, kAlts, 4);
}

static int KotlinString(Str s, int i) {
    int n = MatchBlock(s, i, "\"\"\"", "\"\"\"");
    return n > 0 ? n : MatchSingleLine(s, i, '"');
}
static int KotlinRegexp(Str s, int i) {
    int n = MatchCallWithString(s, i, "Regex(", KotlinString);
    if (n > 0) {
        return n;
    }
    n = KotlinString(s, i);
    if (n > 0) {
        int suffix = LitLen(s, i + n, ".toRegex()");
        if (suffix > 0) {
            return n + suffix;
        }
    }
    return -1;
}

void ScanKotlin(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
        {KotlinRegexp, nullptr},
        {KotlinString, "string"},
    };
    ScanAlts(res, raw, kAlts, 4);
}

static int ScalaString(Str s, int i) {
    int n = MatchBlock(s, i, "\"\"\"", "\"\"\"");
    return n > 0 ? n : MatchSingleLine(s, i, '"');
}
static int ScalaStringLiteral(Str s, int i) {
    if (i >= len(s) || s.s[i] != 's') {
        return -1;
    }
    int n = ScalaString(s, i + 1);
    return n > 0 ? 1 + n : -1;
}
static int ScalaRegexp(Str s, int i) {
    int n = MatchCallWithString(s, i, "Regex(", ScalaString);
    if (n > 0) {
        return n;
    }
    n = ScalaString(s, i);
    if (n > 0 && LitLen(s, i + n, ".r") > 0) {
        return n + 2;
    }
    return -1;
}

void ScanScala(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"}, {CppBlockComment, "COMMENT"},
        {ScalaRegexp, nullptr},      {ScalaStringLiteral, nullptr},
        {ScalaString, "string"},
    };
    ScanAlts(res, raw, kAlts, 5);
}

static int DartString(Str s, int i) {
    int n = MatchBlock(s, i, "'''", "'''");
    if (n > 0) {
        return n;
    }
    n = MatchSingleLine(s, i, '\'');
    if (n > 0) {
        return n;
    }
    n = MatchBlock(s, i, "\"\"\"", "\"\"\"");
    return n > 0 ? n : MatchSingleLine(s, i, '"');
}
static int DartRegexp(Str s, int i) {
    if (i >= len(s) || s.s[i] != 'r') {
        return -1;
    }
    int n = DartString(s, i + 1);
    return n > 0 ? 1 + n : -1;
}

void ScanDart(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"},
        {DartRegexp, nullptr},
        {DartString, "string"},
    };
    ScanAlts(res, raw, kAlts, 4);
}

static int ElixirDocComment(Str s, int i) {
    return MatchBlock(s, i, "\"\"\"", "\"\"\"");
}
static int ElixirString(Str s, int i) {
    int n = MatchSingleLine(s, i, '\'');
    if (n > 0) {
        return n;
    }
    n = MatchSingleLine(s, i, '"');
    if (n > 0) {
        return n;
    }
    if ((LitLen(s, i, "~s(") > 0 || LitLen(s, i, "~c(") > 0)) {
        for (int at = i + 3; at < len(s) && s.s[at] != '\n'; at++) {
            if (s.s[at] == ')') {
                return at + 1 - i;
            }
        }
    }
    return -1;
}
static int ElixirRegexp(Str s, int i) {
    if (LitLen(s, i, "~r/") > 0) {
        for (int at = i + 3; at < len(s) && s.s[at] != '\n'; at++) {
            if (s.s[at] == '/') {
                return at + 1 - i;
            }
        }
        return -1;
    }
    return MatchCallWithString(s, i, "Regex.compile(", ElixirString);
}

void ScanElixir(Results* res, Str raw) {
    static const Alt kAlts[] = {
        {HashLineComment, "COMMENT"},
        {ElixirDocComment, "COMMENT"},
        {ElixirRegexp, nullptr},
        {ElixirString, "string"},
    };
    ScanAlts(res, raw, kAlts, 4);
}

static int JsString(Str s, int i) {
    char q = i < len(s) ? s.s[i] : 0;
    if (q == '\'') {
        return MatchMultiLine(s, i, '\'');
    }
    if (q == '"') {
        return MatchSingleLine(s, i, '"');
    }
    if (q == '`') {
        int n = MatchMultiLine(s, i, '`');
        if (n < 0) {
            return -1;
        }

        int at = i + n;
        while (at < len(s) && s.s[at] == '`') {
            at++;
        }
        return at - i;
    }
    return -1;
}

static int JsRegexp(Str s, int i) {
    int n = MatchSingleLine(s, i, '/');
    if (n > 0) {
        return n;
    }
    n = LitLen(s, i, "RegExp(");
    if (n > 0) {
        int at = i + n;
        while (at < len(s) && s.s[at] == ' ') {
            at++;
        }
        int sn = JsString(s, at);
        if (sn > 0) {
            at += sn;
            while (at < len(s) && s.s[at] != ')') {
                at++;
            }
            if (at < len(s)) {
                return at + 1 - i;
            }
        }
    }
    return -1;
}

static int JsOpenHtml(Str s, int i) {
    if (i >= len(s) || s.s[i] != '<') {
        return -1;
    }
    for (int at = i + 1; at < len(s); at++) {
        if (s.s[at] == '>') {
            return at + 1 - i;
        }
    }
    return -1;
}

static int JsCloseHtml(Str s, int i) {
    if (LitLen(s, i, "</") < 0) {
        return -1;
    }
    for (int at = i + 2; at < len(s); at++) {
        if (s.s[at] == '>') {
            return at + 1 - i;
        }
    }
    return -1;
}

static int JsHtmlNode(Str s, int i) {
    int n = JsOpenHtml(s, i);
    if (n < 0) {
        return -1;
    }
    int at = i + n;
    int children = 0;
    while (at < len(s)) {
        int c = JsCloseHtml(s, at);
        if (c > 0) {
            return children > 0 ? at + c - i : -1;
        }
        if (s.s[at] == '<') {
            int sub = JsHtmlNode(s, at);
            if (sub < 0) {
                return -1;
            }
            at += sub;
        } else {
            while (at < len(s) && s.s[at] != '<') {
                at++;
            }
        }
        children++;
    }
    return -1;
}

void ScanJavascript(Results* res, Str raw) {
    int ignoreStart = 0;
    int i = 0;
    auto flush = [&](int upTo) {
        if (upTo > ignoreStart) {
            EmitIgnore(res, Str(raw.s + ignoreStart, upTo - ignoreStart));
        }
    };
    while (i < len(raw)) {
        int n = CppLineComment(raw, i);
        if (n < 0) {
            n = CppBlockComment(raw, i);
        }
        if (n > 0) {
            flush(i);
            EmitText(res, StrL("COMMENT"), Str(raw.s + i, n));
            ignoreStart = i + n;
            i += n;
            continue;
        }

        n = JsString(raw, i);
        if (n > 0) {
            int at = i + n;
            while (at < len(raw) && raw.s[at] == ' ') {
                at++;
            }
            if (at < len(raw) && raw.s[at] == ':') {
                at++;
                while (at < len(raw) && raw.s[at] == ' ') {
                    at++;
                }
                int vn = JsString(raw, at);
                if (vn > 0) {
                    flush(at);
                    EmitText(res, StrL("string"), Str(raw.s + at, vn));
                    ignoreStart = at + vn;
                    i = at + vn;
                    continue;
                }
            }
            flush(i);
            EmitText(res, StrL("string"), Str(raw.s + i, n));
            ignoreStart = i + n;
            i += n;
            continue;
        }
        n = JsRegexp(raw, i);
        if (n > 0) {
            i += n;
            continue;
        }
        if (raw.s[i] == '<') {
            int node = JsHtmlNode(raw, i);
            if (node > 0) {

                int at = i;
                int end = i + node;
                while (at < end) {
                    if (raw.s[at] == '<') {
                        int t = JsOpenHtml(raw, at);
                        at += t > 0 ? t : 1;
                        continue;
                    }
                    int textEnd = at;
                    while (textEnd < end && raw.s[textEnd] != '<') {
                        textEnd++;
                    }
                    flush(at);
                    EmitText(res, StrL("text"), Str(raw.s + at, textEnd - at));
                    ignoreStart = textEnd;
                    at = textEnd;
                }
                i = end;
                continue;
            }
            int tag = JsOpenHtml(raw, i);
            if (tag > 0) {

                i += tag;
                continue;
            }
        }
        i++;
    }
    flush(len(raw));
}

static int PhpString(Str s, int i) {
    int n = MatchBlock(s, i, "\"\"\"", "\"\"\"");
    return n > 0 ? n : MatchBlock(s, i, "\"", "\"");
}
static int PhpRegexp(Str s, int i) {
    int n = MatchCallWithString(s, i, "preg_match_all(", PhpString);
    if (n > 0) {
        return n;
    }
    return MatchCallWithString(s, i, "preg_match(", PhpString);
}

void ScanPhp(Results* res, Str raw) {
    int ignoreStart = 0;
    int i = 0;
    bool inPhp = false;
    static const Alt kAlts[] = {
        {CppLineComment, "COMMENT"},  {HashLineComment, "COMMENT"},
        {CppBlockComment, "COMMENT"}, {PhpRegexp, nullptr},
        {PhpString, "string"},
    };
    while (i < len(raw)) {
        if (!inPhp) {

            int matched = -1;
            const Alt* hit = nullptr;
            for (int k = 0; k < 3; k++) {
                matched = kAlts[k].match(raw, i);
                if (matched > 0) {
                    hit = &kAlts[k];
                    break;
                }
            }
            if (hit) {
                if (i > ignoreStart) {
                    EmitIgnore(res, Str(raw.s + ignoreStart, i - ignoreStart));
                }
                EmitText(res, Str(hit->rule), Str(raw.s + i, matched));
                ignoreStart = i + matched;
                i += matched;
                continue;
            }
            if (LitLen(raw, i, "<?php") > 0) {
                inPhp = true;
                i += 5;
                continue;
            }
            i++;
            continue;
        }
        if (LitLen(raw, i, "?>") > 0) {
            inPhp = false;
            i += 2;
            continue;
        }
        int matched = -1;
        const Alt* hit = nullptr;
        for (const Alt& alt : kAlts) {
            matched = alt.match(raw, i);
            if (matched > 0) {
                hit = &alt;
                break;
            }
        }
        if (!hit) {
            i++;
            continue;
        }
        if (hit->rule) {
            if (i > ignoreStart) {
                EmitIgnore(res, Str(raw.s + ignoreStart, i - ignoreStart));
            }
            EmitText(res, Str(hit->rule), Str(raw.s + i, matched));
            ignoreStart = i + matched;
        }
        i += matched;
    }
    if (len(raw) > ignoreStart) {
        EmitIgnore(res, Str(raw.s + ignoreStart, len(raw) - ignoreStart));
    }
}

static int JsonString(Str s, int i) {

    if (i >= len(s) || s.s[i] != '"') {
        return -1;
    }
    for (int at = i + 1; at < len(s); at++) {
        if (s.s[at] == '\\') {
            at++;
            continue;
        }
        if (s.s[at] == '"') {
            return at + 1 - i;
        }
    }
    return -1;
}

void ScanJson(Results* res, Str raw) {
    int ignoreStart = 0;
    int i = 0;
    while (i < len(raw)) {
        int n = CppLineComment(raw, i);
        bool isComment = n > 0;
        if (!isComment) {
            n = CppBlockComment(raw, i);
            isComment = n > 0;
        }
        if (isComment) {
            if (i > ignoreStart) {
                EmitIgnore(res, Str(raw.s + ignoreStart, i - ignoreStart));
            }
            EmitText(res, StrL("COMMENT"), Str(raw.s + i, n));
            ignoreStart = i + n;
            i += n;
            continue;
        }
        n = JsonString(raw, i);
        if (n > 0) {

            int at = i + n;
            while (at < len(raw) && (raw.s[at] == ' ' || raw.s[at] == '\t')) {
                at++;
            }
            bool isKey = at < len(raw) && raw.s[at] == ':';
            if (!isKey) {
                if (i > ignoreStart) {
                    EmitIgnore(res, Str(raw.s + ignoreStart, i - ignoreStart));
                }
                EmitText(res, StrL("string"), Str(raw.s + i, n));
                ignoreStart = i + n;
            }
            i += n;
            continue;
        }
        i++;
    }
    if (len(raw) > ignoreStart) {
        EmitIgnore(res, Str(raw.s + ignoreStart, len(raw) - ignoreStart));
    }
}

void ScanYaml(Results* res, Str raw) {
    int ignoreStart = 0;
    int i = 0;
    while (i < len(raw)) {
        int at = i;
        while (at < len(raw) && raw.s[at] == ' ') {
            at++;
        }
        if (at < len(raw) && raw.s[at] == '#') {
            int end = at;
            while (end < len(raw) && raw.s[end] != '\n') {
                end++;
            }
            if (at > ignoreStart) {
                EmitIgnore(res, Str(raw.s + ignoreStart, at - ignoreStart));
            }
            EmitText(res, StrL("comment"), Str(raw.s + at, end - at));
            ignoreStart = end;
            i = end < len(raw) ? end + 1 : end;
            continue;
        }

        int keyEnd = at;
        if (at < len(raw) && raw.s[at] == '"') {
            int n = MatchSingleLine(raw, at, '"');
            keyEnd = n > 0 ? at + n : at;
        } else {
            while (keyEnd < len(raw) && raw.s[keyEnd] != ':' &&
                   raw.s[keyEnd] != '"' && raw.s[keyEnd] != '\'' &&
                   raw.s[keyEnd] != '\n') {
                keyEnd++;
            }
        }
        bool isPair = keyEnd > at && keyEnd < len(raw) && raw.s[keyEnd] == ':';
        if (!isPair) {

            while (i < len(raw) && raw.s[i] != '\n') {
                i++;
            }
            if (i < len(raw)) {
                i++;
            }
            continue;
        }
        int valueStart = keyEnd + 1;
        if (valueStart < len(raw) && raw.s[valueStart] == ' ') {
            valueStart++;
        }

        int valueEnd = valueStart;
        if (valueStart < len(raw) &&
            (raw.s[valueStart] == '"' || raw.s[valueStart] == '\'')) {
            int n = MatchSingleLine(raw, valueStart, raw.s[valueStart]);
            if (n > 0) {
                valueEnd = valueStart + n;
            }
        } else {
            while (valueEnd < len(raw) && raw.s[valueEnd] != '\n' &&
                   raw.s[valueEnd] != '"' && raw.s[valueEnd] != '\'') {
                valueEnd++;
            }
        }
        if (valueStart > ignoreStart) {
            EmitIgnore(res, Str(raw.s + ignoreStart, valueStart - ignoreStart));
        }
        EmitText(res, StrL("string"),
                 Str(raw.s + valueStart, valueEnd - valueStart));
        ignoreStart = valueEnd;
        i = valueEnd;
        while (i < len(raw) && raw.s[i] != '\n') {
            i++;
        }
        if (i < len(raw)) {
            i++;
        }
    }
    if (len(raw) > ignoreStart) {
        EmitIgnore(res, Str(raw.s + ignoreStart, len(raw) - ignoreStart));
    }
}

}

#line 1 "src/autocorrect/toggle.cpp"

namespace autocorrect {

static bool IsAsciiAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static int MatchRuleName(Str s, int i) {
    int at = i;
    while (at < len(s) && IsAsciiAlnum(s.s[at])) {
        at++;
        while (at < len(s) && (s.s[at] == '-' || s.s[at] == '_')) {
            at++;
        }
    }
    return at - i;
}

static void ToggleAddRule(Toggle* t, Str name) {

    int id = RuleIdByName(name);
    if (id >= 0) {
        t->mask = (uint16_t)(t->mask | (1u << id));
    } else {
        t->hasUnknown = true;
    }
}

Toggle ToggleParse(Str comment) {
    Str s = comment;
    static const Str kWord = StrL("autocorrect");
    for (int i = 0; i + len(kWord) <= len(s); i++) {
        if (!base::StrEq(Str(s.s + i, len(kWord)), kWord)) {
            continue;
        }
        int at = i + len(kWord);

        if (at < len(s) && s.s[at] == ':') {
            at++;
            while (at < len(s) && s.s[at] == ' ') {
                at++;
            }
        } else if (at < len(s) && s.s[at] == '-') {
            at++;
        } else {
            continue;
        }
        ToggleKind kind;
        static const Str kEnable = StrL("enable");
        static const Str kTrue = StrL("true");
        static const Str kDisable = StrL("disable");
        static const Str kFalse = StrL("false");
        if (at + len(kEnable) <= len(s) &&
            base::StrEq(Str(s.s + at, len(kEnable)), kEnable)) {
            kind = ToggleKind::Enable;
            at += len(kEnable);
        } else if (at + len(kTrue) <= len(s) &&
                   base::StrEq(Str(s.s + at, len(kTrue)), kTrue)) {
            kind = ToggleKind::Enable;
            at += len(kTrue);
        } else if (at + len(kDisable) <= len(s) &&
                   base::StrEq(Str(s.s + at, len(kDisable)), kDisable)) {
            kind = ToggleKind::Disable;
            at += len(kDisable);
        } else if (at + len(kFalse) <= len(s) &&
                   base::StrEq(Str(s.s + at, len(kFalse)), kFalse)) {
            kind = ToggleKind::Disable;
            at += len(kFalse);
        } else {
            continue;
        }
        Toggle t;
        t.kind = kind;

        while (at < len(s) && s.s[at] == ' ') {
            at++;
            for (;;) {
                int n = MatchRuleName(s, at);
                if (n <= 0) {
                    break;
                }
                ToggleAddRule(&t, Str(s.s + at, n));
                at += n;
                while (at < len(s) && (s.s[at] == ',' || s.s[at] == ' ')) {
                    at++;
                }
            }
        }
        return t;
    }
    Toggle none;
    none.kind = ToggleKind::None;
    return none;
}

void ToggleMerge(Toggle* t, Toggle neu) {
    if (neu.kind == ToggleKind::None) {
        *t = neu;
        return;
    }
    if (t->kind != neu.kind) {
        *t = neu;
        return;
    }

    if (neu.RulesEmpty()) {
        t->mask = 0;
        t->hasUnknown = false;
        return;
    }
    if (t->RulesEmpty()) {
        return;
    }
    t->mask = (uint16_t)(t->mask | neu.mask);
    t->hasUnknown = t->hasUnknown || neu.hasUnknown;
}

uint16_t ToggleDisableRules(const Toggle* t) {
    return t->kind == ToggleKind::Disable ? t->mask : 0;
}

bool ToggleIsEnabled(const Toggle* t) {

    switch (t->kind) {
        case ToggleKind::None:
            return true;
        case ToggleKind::Disable:
            return !t->RulesEmpty();
        case ToggleKind::Enable:
        default:
            return t->RulesEmpty();
    }
}

}

#line 1 "src/autocorrect/unicode.cpp"

namespace autocorrect {

uint32_t Utf8Next(Str s, int* i) {
    int at = *i;
    uint8_t b0 = (uint8_t)s.s[at];

    int size = 1;
    uint32_t cp = b0;
    if ((b0 & 0xE0) == 0xC0 && at + 1 < len(s)) {
        size = 2;
        cp = ((uint32_t)(b0 & 0x1F) << 6) | ((uint8_t)s.s[at + 1] & 0x3F);
    } else if ((b0 & 0xF0) == 0xE0 && at + 2 < len(s)) {
        size = 3;
        cp = ((uint32_t)(b0 & 0x0F) << 12) |
             (((uint32_t)(uint8_t)s.s[at + 1] & 0x3F) << 6) |
             ((uint8_t)s.s[at + 2] & 0x3F);
    } else if ((b0 & 0xF8) == 0xF0 && at + 3 < len(s)) {
        size = 4;
        cp = ((uint32_t)(b0 & 0x07) << 18) |
             (((uint32_t)(uint8_t)s.s[at + 1] & 0x3F) << 12) |
             (((uint32_t)(uint8_t)s.s[at + 2] & 0x3F) << 6) |
             ((uint8_t)s.s[at + 3] & 0x3F);
    }
    *i = at + size;
    return cp;
}

int Utf8Len(Str s, int i) {
    int at = i;
    Utf8Next(s, &at);
    return at - i;
}

uint32_t Utf8At(Str s, int i) {
    int at = i;
    return Utf8Next(s, &at);
}

int Utf8Count(Str s) {
    int n = 0;
    for (int i = 0; i < len(s);) {
        Utf8Next(s, &i);
        n++;
    }
    return n;
}

struct CpRange {
    uint32_t lo;
    uint32_t hi;
};

static bool InRanges(uint32_t cp, const CpRange* r, int n) {

    for (int i = 0; i < n; i++) {
        if (cp < r[i].lo) {
            return false;
        }
        if (cp <= r[i].hi) {
            return true;
        }
    }
    return false;
}

static const CpRange kHan[] = {
    {0x2E80, 0x2E99},   {0x2E9B, 0x2EF3},   {0x2F00, 0x2FD5},
    {0x3005, 0x3005},   {0x3007, 0x3007},   {0x3021, 0x3029},
    {0x3038, 0x303B},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},
    {0xF900, 0xFA6D},   {0xFA70, 0xFAD9},   {0x20000, 0x2A6DF},
    {0x2A700, 0x2B739}, {0x2B740, 0x2B81D}, {0x2B820, 0x2CEA1},
    {0x2CEB0, 0x2EBE0}, {0x2EBF0, 0x2EE5D}, {0x2F800, 0x2FA1D},
    {0x30000, 0x3134A}, {0x31350, 0x323AF},
};

static const CpRange kHangul[] = {
    {0x1100, 0x11FF}, {0x302E, 0x302F}, {0x3131, 0x318E}, {0x3200, 0x321E},
    {0x3260, 0x327E}, {0xA960, 0xA97C}, {0xAC00, 0xD7A3}, {0xD7B0, 0xD7C6},
    {0xD7CB, 0xD7FB}, {0xFFA0, 0xFFBE}, {0xFFC2, 0xFFC7}, {0xFFCA, 0xFFCF},
    {0xFFD2, 0xFFD7}, {0xFFDA, 0xFFDC},
};

static const CpRange kKatakana[] = {
    {0x30A1, 0x30FA},   {0x30FD, 0x30FF},   {0x31F0, 0x31FF},
    {0x32D0, 0x32FE},   {0x3300, 0x3357},   {0xFF66, 0xFF6F},
    {0xFF71, 0xFF9D},   {0x1AFF0, 0x1AFF3}, {0x1AFF5, 0x1AFFB},
    {0x1AFFD, 0x1AFFE}, {0x1B000, 0x1B000}, {0x1B120, 0x1B122},
    {0x1B155, 0x1B155}, {0x1B164, 0x1B167},
};

static const CpRange kHiragana[] = {
    {0x3041, 0x3096},   {0x309D, 0x309F},   {0x1B001, 0x1B11F},
    {0x1B132, 0x1B132}, {0x1B150, 0x1B152}, {0x1F200, 0x1F200},
};

static const CpRange kBopomofo[] = {
    {0x3105, 0x312F},
    {0x31A0, 0x31BF},
};

bool IsHan(uint32_t cp) {
    return cp >= 0x2E80 &&
           InRanges(cp, kHan, (int)(sizeof(kHan) / sizeof(kHan[0])));
}

bool IsHangul(uint32_t cp) {
    return cp >= 0x1100 &&
           InRanges(cp, kHangul, (int)(sizeof(kHangul) / sizeof(kHangul[0])));
}

bool IsKatakana(uint32_t cp) {
    return cp >= 0x30A1 &&
           InRanges(cp, kKatakana,
                    (int)(sizeof(kKatakana) / sizeof(kKatakana[0])));
}

bool IsHiragana(uint32_t cp) {
    return cp >= 0x3041 &&
           InRanges(cp, kHiragana,
                    (int)(sizeof(kHiragana) / sizeof(kHiragana[0])));
}

bool IsBopomofo(uint32_t cp) {
    return cp >= 0x3105 &&
           InRanges(cp, kBopomofo,
                    (int)(sizeof(kBopomofo) / sizeof(kBopomofo[0])));
}

bool IsCjk(uint32_t cp) {
    return IsHan(cp) || IsHangul(cp) || IsKatakana(cp) || IsHiragana(cp) ||
           IsBopomofo(cp);
}

bool IsCj(uint32_t cp) {
    return IsHan(cp) || IsKatakana(cp) || IsHiragana(cp) || IsBopomofo(cp);
}

bool IsWordCp(uint32_t cp) {
    if (cp < 0x80) {
        return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
               (cp >= '0' && cp <= '9') || cp == '_';
    }

    if (cp >= 0xC0 && cp <= 0x24F) {
        return cp != 0xD7 && cp != 0xF7;
    }
    if (cp >= 0x370 && cp <= 0x4FF) {
        return true;
    }
    if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) ||
        (cp >= 0xFF41 && cp <= 0xFF5A)) {
        return true;
    }
    return IsCjk(cp);
}

bool HasCjk(Str s) {
    for (int i = 0; i < len(s);) {
        if (IsCjk(Utf8Next(s, &i))) {
            return true;
        }
    }
    return false;
}

}

#line 1 "src/autocorrect/word.cpp"

namespace autocorrect {

using SideFn = int (*)(Str s, int i);

static bool IsAsciiAlnumCp(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= '0' && cp <= '9');
}

static bool IsSpaceCp(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' ||
        cp == 0x0B || cp == 0x85 || cp == 0xA0) {
        return true;
    }
    return cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
           cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

static bool IsCjkClassCp(uint32_t cp) {
    return cp == '|' || IsCjk(cp);
}

static int MatchCp(Str s, int i, bool (*pred)(uint32_t)) {
    if (i >= len(s)) {
        return -1;
    }
    int at = i;
    uint32_t cp = Utf8Next(s, &at);
    return pred(cp) ? at - i : -1;
}

static int SideCjkWordOne(Str s, int i) {
    if (i >= len(s)) {
        return -1;
    }
    int at = i;
    uint32_t cp = Utf8Next(s, &at);
    if (IsHan(cp) || IsHangul(cp) || IsKatakana(cp) || IsHiragana(cp)) {
        return at - i;
    }
    if (IsBopomofo(cp) && at < len(s)) {
        uint32_t c2 = Utf8Next(s, &at);
        if (c2 != '%' && c2 != '$' && c2 != '\\') {
            return at - i;
        }
    }
    return -1;
}

static int SideAlnum(Str s, int i) {
    return MatchCp(s, i, IsAsciiAlnumCp);
}

static int SideNotEscapeThenAlnum(Str s, int i) {
    if (i >= len(s)) {
        return -1;
    }
    int at = i;
    uint32_t cp = Utf8Next(s, &at);
    if (cp == '%' || cp == '$' || cp == '\\') {
        return -1;
    }
    if (at >= len(s) || !IsAsciiAlnumCp(Utf8At(s, at))) {
        return -1;
    }
    return at + 1 - i;
}

static int SideCjk(Str s, int i) {
    return MatchCp(s, i, IsCjk);
}

static int SideSignedNumber(Str s, int i) {
    if (i >= len(s) || (s.s[i] != '-' && s.s[i] != '+')) {
        return -1;
    }
    int at = i + 1;
    int digits = 0;
    while (at < len(s) && s.s[at] >= '0' && s.s[at] <= '9') {
        at++;
        digits++;
    }
    return digits > 0 ? at - i : -1;
}

static int SideStartAlnum(Str s, int i) {
    if (i != 0) {
        return -1;
    }
    return MatchCp(s, i, IsAsciiAlnumCp);
}

static int SideDigitPercent(Str s, int i) {
    if (i + 1 < len(s) && s.s[i] >= '0' && s.s[i] <= '9' && s.s[i + 1] == '%') {
        return 2;
    }
    return -1;
}

static int SideAlnumPlusHash(Str s, int i) {
    if (i >= len(s) || !IsAsciiAlnumCp((uint8_t)s.s[i])) {
        return -1;
    }
    int at = i + 1;
    int n = 0;
    while (at < len(s) && (s.s[at] == '+' || s.s[at] == '#')) {
        at++;
        n++;
    }
    return n > 0 ? at - i : -1;
}

static bool IsCjkOrRightQuoteCp(uint32_t cp) {
    return IsCjkClassCp(cp) || cp == 0x201D || cp == 0x2019;
}
static int SideCjkOrRightQuote(Str s, int i) {
    return MatchCp(s, i, IsCjkOrRightQuoteCp);
}

static bool IsCjkSpaceOrLeftQuoteCp(uint32_t cp) {
    if (IsCjkClassCp(cp) || IsSpaceCp(cp)) {
        return true;
    }
    return cp == 0xFF08 || cp == 0x3010 || cp == 0x300C || cp == 0x300A ||
           cp == 0x201C || cp == 0x2018;
}

static bool IsCjkSpaceOrCloseQuoteCp(uint32_t cp) {
    if (IsCjkClassCp(cp) || IsSpaceCp(cp)) {
        return true;
    }
    return cp == 0xFF09 || cp == 0x3011 || cp == 0x300D || cp == 0x201D ||
           cp == 0x2019 || cp == 0x300B;
}

static bool IsCjkOrLeftQuoteCp(uint32_t cp) {
    return IsCjkClassCp(cp) || cp == 0x201C || cp == 0x2018;
}
static int SideCjkOrLeftQuote(Str s, int i) {
    return MatchCp(s, i, IsCjkOrLeftQuoteCp);
}

static int SidePipeThenOpen(Str s, int i) {
    if (i >= len(s) || (s.s[i] != '|' && s.s[i] != '+')) {
        return -1;
    }
    int n = MatchCp(s, i + 1, IsCjkSpaceOrLeftQuoteCp);
    return n > 0 ? 1 + n : -1;
}
static int SideDashThenOpen(Str s, int i) {
    if (i >= len(s) || s.s[i] != '-') {
        return -1;
    }
    int n = MatchCp(s, i + 1, IsCjkSpaceOrLeftQuoteCp);
    return n > 0 ? 1 + n : -1;
}

static int SideCloseThenPipe(Str s, int i) {
    int n = MatchCp(s, i, IsCjkSpaceOrCloseQuoteCp);
    if (n <= 0 || i + n >= len(s) || (s.s[i + n] != '|' && s.s[i + n] != '+')) {
        return -1;
    }
    return n + 1;
}
static int SideCloseThenDash(Str s, int i) {
    int n = MatchCp(s, i, IsCjkSpaceOrCloseQuoteCp);
    if (n <= 0 || i + n >= len(s) || s.s[i + n] != '-') {
        return -1;
    }
    return n + 1;
}

static int SideBang(Str s, int i) {
    return i < len(s) && s.s[i] == '!' ? 1 : -1;
}

static int SideOpenBracket(Str s, int i) {
    return i < len(s) && (s.s[i] == '[' || s.s[i] == '(') ? 1 : -1;
}
static int SideCloseBracket(Str s, int i) {
    return i < len(s) && (s.s[i] == ']' || s.s[i] == ')') ? 1 : -1;
}

static int SideBacktickString(Str s, int i) {
    if (i >= len(s) || s.s[i] != '`') {
        return -1;
    }
    int last = -1;
    for (int j = i + 1; j < len(s) && s.s[j] != '\n'; j++) {
        if (s.s[j] == '`' && j > i + 1) {
            last = j;
        }
    }
    return last > 0 ? last + 1 - i : -1;
}

static int SideDollar(Str s, int i) {
    return i < len(s) && s.s[i] == '$' ? 1 : -1;
}

static bool IsWordCjkOrBacktickCp(uint32_t cp) {
    return cp == '`' || IsWordCp(cp) || IsCjk(cp);
}
static int SideWordCjkOrBacktick(Str s, int i) {
    return MatchCp(s, i, IsWordCjkOrBacktickCp);
}

static bool IsWordOrCjkCp(uint32_t cp) {
    return IsWordCp(cp) || IsCjk(cp);
}
static int SideWordOrCjk(Str s, int i) {
    return MatchCp(s, i, IsWordOrCjkCp);
}

static bool IsFullwidthPunctCp(uint32_t cp) {
    switch (cp) {
        case 0xFF0C:
        case 0x3002:
        case 0x3001:
        case 0xFF01:
        case 0xFF1F:
        case 0xFF1A:
        case 0xFF1B:
        case 0xFF08:
        case 0xFF09:
        case 0x300C:
        case 0x300D:
        case 0x300A:
        case 0x300B:
        case 0x3010:
        case 0x3011:
            return true;
        default:
            return false;
    }
}
static int SideFullwidthPunct(Str s, int i) {
    return MatchCp(s, i, IsFullwidthPunctCp);
}

static bool IsFullwidthQuoteCp(uint32_t cp) {
    return cp == 0x201C || cp == 0x201D || cp == 0x2018 || cp == 0x2019;
}
static int SideFullwidthQuote(Str s, int i) {
    return MatchCp(s, i, IsFullwidthQuoteCp);
}

static bool PassAdd(Str in, SideFn one, SideFn other, StrBuilder* out) {
    bool changed = false;
    int i = 0;
    while (i < len(in)) {
        int n1 = one(in, i);
        if (n1 > 0) {
            int n2 = other(in, i + n1);
            if (n2 > 0) {
                out->Append(Str(in.s + i, n1));
                out->AppendChar(' ');
                out->Append(Str(in.s + i + n1, n2));
                i += n1 + n2;
                changed = true;
                continue;
            }
        }
        int step = Utf8Len(in, i);
        out->Append(Str(in.s + i, step));
        i += step;
    }
    return changed;
}

static bool PassRemove(Str in, SideFn one, SideFn other, StrBuilder* out) {
    bool changed = false;
    int i = 0;
    while (i < len(in)) {
        int n1 = one(in, i);
        if (n1 > 0) {
            int sp = i + n1;
            while (sp < len(in) && in.s[sp] == ' ') {
                sp++;
            }
            if (sp > i + n1) {
                int n2 = other(in, sp);
                if (n2 > 0) {
                    out->Append(Str(in.s + i, n1));
                    out->Append(Str(in.s + sp, n2));
                    i = sp + n2;
                    changed = true;
                    continue;
                }
            }
        }
        int step = Utf8Len(in, i);
        out->Append(Str(in.s + i, step));
        i += step;
    }
    return changed;
}

struct Pass {
    SideFn one;
    SideFn other;
    bool remove;
};

static bool RunPasses(Arena* a, Str in, const Pass* passes, int nPasses,
                      Str* out) {
    Str cur = in;
    bool changed = false;
    StrBuilder b;
    for (int p = 0; p < nPasses; p++) {
        b.Reset();
        bool hit = passes[p].remove
                       ? PassRemove(cur, passes[p].one, passes[p].other, &b)
                       : PassAdd(cur, passes[p].one, passes[p].other, &b);
        if (hit) {
            cur = base::StrDup(a, Str(b.els, b.len));
            changed = true;
        }
    }
    if (changed) {
        *out = cur;
    }
    return changed;
}

bool FormatSpaceWord(Arena* a, Str in, Str* out) {
    static const Pass kPasses[] = {
        {SideCjkWordOne, SideAlnum, false},
        {SideNotEscapeThenAlnum, SideCjk, false},

        {SideCjk, SideSignedNumber, false},
        {SideSignedNumber, SideCjk, false},
        {SideStartAlnum, SideCjk, false},
        {SideDigitPercent, SideCjk, false},
        {SideAlnumPlusHash, SideCjk, false},
    };
    return RunPasses(a, in, kPasses, 7, out);
}

bool FormatSpacePunctuation(Arena* a, Str in, Str* out) {
    static const Pass kPasses[] = {
        {SideCjkOrRightQuote, SidePipeThenOpen, false},
        {SideCloseThenPipe, SideCjkOrLeftQuote, false},
        {SideBang, SideCjk, false},
    };
    return RunPasses(a, in, kPasses, 3, out);
}

bool FormatSpaceBracket(Arena* a, Str in, Str* out) {
    static const Pass kPasses[] = {
        {SideCjk, SideOpenBracket, false},
        {SideCloseBracket, SideCjk, false},
    };
    return RunPasses(a, in, kPasses, 2, out);
}

bool FormatSpaceDash(Arena* a, Str in, Str* out) {
    static const Pass kPasses[] = {
        {SideCjkOrRightQuote, SideDashThenOpen, false},
        {SideCloseThenDash, SideCjkOrLeftQuote, false},
    };
    return RunPasses(a, in, kPasses, 2, out);
}

static bool PassBacktickThenCjk(Str in, StrBuilder* out) {
    bool changed = false;
    int i = 0;
    while (i < len(in)) {
        if (in.s[i] == '`') {
            int end = -1;
            for (int j = i + 2; j < len(in) && in.s[j] != '\n'; j++) {
                if (in.s[j] == '`' && j + 1 < len(in) &&
                    SideCjk(in, j + 1) > 0) {
                    end = j + 1;
                }
            }
            if (end > 0) {
                int n2 = SideCjk(in, end);
                out->Append(Str(in.s + i, end - i));
                out->AppendChar(' ');
                out->Append(Str(in.s + end, n2));
                i = end + n2;
                changed = true;
                continue;
            }
        }
        int step = Utf8Len(in, i);
        out->Append(Str(in.s + i, step));
        i += step;
    }
    return changed;
}

bool FormatSpaceBackticks(Arena* a, Str in, Str* out) {
    Str cur = in;
    bool changed = false;
    StrBuilder b;
    if (PassAdd(cur, SideCjk, SideBacktickString, &b)) {
        cur = base::StrDup(a, Str(b.els, b.len));
        changed = true;
    }
    b.Reset();
    if (PassBacktickThenCjk(cur, &b)) {
        cur = base::StrDup(a, Str(b.els, b.len));
        changed = true;
    }
    if (changed) {
        *out = cur;
    }
    return changed;
}

bool FormatSpaceDollar(Arena* a, Str in, Str* out) {
    static const Pass kPasses[] = {
        {SideCjk, SideDollar, false},
        {SideDollar, SideCjk, false},
    };
    return RunPasses(a, in, kPasses, 2, out);
}

bool FormatNoSpaceFullwidth(Arena* a, Str in, Str* out) {
    if (!HasCjk(in)) {
        return false;
    }
    static const Pass kPasses[] = {
        {SideWordCjkOrBacktick, SideFullwidthPunct, true},
        {SideFullwidthPunct, SideWordCjkOrBacktick, true},
    };
    return RunPasses(a, in, kPasses, 2, out);
}

bool FormatNoSpaceFullwidthQuote(Arena* a, Str in, Str* out) {
    if (!HasCjk(in)) {
        return false;
    }
    static const Pass kPasses[] = {
        {SideWordOrCjk, SideFullwidthQuote, true},
        {SideFullwidthQuote, SideWordOrCjk, true},
    };
    return RunPasses(a, in, kPasses, 2, out);
}

}
