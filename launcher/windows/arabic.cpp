// SPDX-License-Identifier: GPL-2.0-or-later
// Arabic in the launcher (arabic.h): Dear ImGui draws glyphs left to right one code point at a
// time, so the text is prepared for it through GImFontTextShape (gpu/patches/imgui):
//  - shaping: each letter becomes its isolated, initial, medial or final form (Arabic
//    Presentation Forms-B), lam + alef its ligature; the fonts map those code points
//    (tools/arabic_presentation_forms.py);
//  - order: one line at a time, runs of Arabic are reversed and laid out right to left, while
//    runs of Latin letters and digits (DLSS, 1440p, 60 FPS) keep their own order, as the
//    Unicode bidirectional algorithm does for such simple text.
#include "arabic.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace arabic {
namespace {

struct Forms { char32_t isolated, final, initial, medial; };  // 0: the letter has no such form

const Forms* FormsOf(char32_t c) {
    static const struct { char32_t letter; Forms forms; } kTable[] = {
        {0x0621, {0xFE80, 0, 0, 0}}, {0x0622, {0xFE81, 0xFE82, 0, 0}}, {0x0623, {0xFE83, 0xFE84, 0, 0}},
        {0x0624, {0xFE85, 0xFE86, 0, 0}}, {0x0625, {0xFE87, 0xFE88, 0, 0}}, {0x0626, {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}},
        {0x0627, {0xFE8D, 0xFE8E, 0, 0}}, {0x0628, {0xFE8F, 0xFE90, 0xFE91, 0xFE92}}, {0x0629, {0xFE93, 0xFE94, 0, 0}},
        {0x062A, {0xFE95, 0xFE96, 0xFE97, 0xFE98}}, {0x062B, {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}},
        {0x062C, {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}}, {0x062D, {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}},
        {0x062E, {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}}, {0x062F, {0xFEA9, 0xFEAA, 0, 0}}, {0x0630, {0xFEAB, 0xFEAC, 0, 0}},
        {0x0631, {0xFEAD, 0xFEAE, 0, 0}}, {0x0632, {0xFEAF, 0xFEB0, 0, 0}}, {0x0633, {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}},
        {0x0634, {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}}, {0x0635, {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}},
        {0x0636, {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}}, {0x0637, {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}},
        {0x0638, {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}}, {0x0639, {0xFEC9, 0xFECA, 0xFECB, 0xFECC}},
        {0x063A, {0xFECD, 0xFECE, 0xFECF, 0xFED0}}, {0x0640, {0x0640, 0x0640, 0x0640, 0x0640}},
        {0x0641, {0xFED1, 0xFED2, 0xFED3, 0xFED4}}, {0x0642, {0xFED5, 0xFED6, 0xFED7, 0xFED8}},
        {0x0643, {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}}, {0x0644, {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}},
        {0x0645, {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}}, {0x0646, {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}},
        {0x0647, {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}}, {0x0648, {0xFEED, 0xFEEE, 0, 0}}, {0x0649, {0xFEEF, 0xFEF0, 0, 0}},
        {0x064A, {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}},
    };
    for (const auto& e : kTable)
        if (e.letter == c) return &e.forms;
    return nullptr;
}

bool Transparent(char32_t c) { return (c >= 0x064B && c <= 0x065F) || c == 0x0670; }  // harakat
bool JoinsForward(char32_t c) {
    const Forms* f = FormsOf(c);
    return f && f->initial;
}
bool ArabicLetter(char32_t c) { return c >= 0x0600 && c <= 0x06FF; }
bool RightToLeft(char32_t c) { return ArabicLetter(c) || (c >= 0xFB50 && c <= 0xFEFF) || c == 0x200F; }
bool LeftToRight(char32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x0590) ||
           c == 0x200E;
}
bool Mark(char32_t c) { return c == 0x200E || c == 0x200F; }  // LRM / RLM: direction only, not drawn
char32_t Mirror(char32_t c) {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '<': return '>';
    case '>': return '<';
    case '{': return '}';
    case '}': return '{';
    }
    return c;
}

std::u32string Decode(const char* s, const char* end) {
    std::u32string out;
    while (s < end) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, s, end);
        s += n > 0 ? n : 1;
        out.push_back(char32_t(c));
    }
    return out;
}

std::string Encode(const std::u32string& text) {
    std::string out;
    for (char32_t c : text) {
        char buf[5];
        const int n = ImTextCharToUtf8(buf, unsigned(c));
        out.append(buf, size_t(n));
    }
    return out;
}

std::u32string Shape(const std::u32string& in) {
    std::u32string out;
    out.reserve(in.size());
    auto neighbour = [&](size_t i, int step) -> char32_t {  // the next letter, skipping harakat
        for (long j = long(i) + step; j >= 0 && j < long(in.size()); j += step)
            if (!Transparent(in[size_t(j)])) return in[size_t(j)];
        return 0;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        const char32_t c = in[i];
        const Forms* f = FormsOf(c);
        if (!f) {
            out.push_back(c);
            continue;
        }
        const bool before = JoinsForward(neighbour(i, -1));
        // lam + alef: one ligature, joined to the letter before or not.
        if (c == 0x0644 && i + 1 < in.size()) {
            static const struct { char32_t alef, isolated, final; } kLamAlef[] = {
                {0x0622, 0xFEF5, 0xFEF6}, {0x0623, 0xFEF7, 0xFEF8}, {0x0625, 0xFEF9, 0xFEFA}, {0x0627, 0xFEFB, 0xFEFC}};
            bool ligature = false;
            for (const auto& la : kLamAlef)
                if (in[i + 1] == la.alef) {
                    out.push_back(before ? la.final : la.isolated);
                    ++i;
                    ligature = true;
                    break;
                }
            if (ligature) continue;
        }
        const char32_t next = neighbour(i, 1);
        const bool after = f->initial && FormsOf(next);
        out.push_back(before && after ? f->medial : before ? f->final : after ? f->initial : f->isolated);
    }
    return out;
}

/// One line in display order. The launcher's Arabic is the paragraph language, so a line with
/// any Arabic reads right to left ("MSAA (2x) اللعبة" starts with Latin but is Arabic).
std::u32string Visual(const std::u32string& line) {
    enum Dir : char { L, R, N };
    std::vector<Dir> dir(line.size());
    Dir base = L;
    for (size_t i = 0; i < line.size(); ++i) {
        dir[i] = RightToLeft(line[i]) ? R : LeftToRight(line[i]) ? L : N;
        if (dir[i] == R) base = R;
    }
    // Bracket pairs take one direction (rule N0 of the bidirectional algorithm): the base one
    // when they hold it, else the opposite one they hold if the text before them has it too.
    std::vector<size_t> open;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '(' || line[i] == '[') open.push_back(i);
        else if ((line[i] == ')' || line[i] == ']') && !open.empty()) {
            const size_t o = open.back();
            open.pop_back();
            bool has_base = false, has_other = false;
            for (size_t k = o + 1; k < i; ++k) {
                has_base |= dir[k] == base;
                has_other |= dir[k] != N && dir[k] != base;
            }
            Dir before = base;
            for (size_t k = o; k-- > 0;)
                if (dir[k] != N) { before = dir[k]; break; }
            const Dir d = has_base ? base : has_other && before != base ? before : base;
            dir[o] = dir[i] = d;
        }
    }
    // Neutrals (spaces, punctuation) take the direction around them, else the base one.
    for (size_t i = 0; i < line.size(); ++i) {
        if (dir[i] != N) continue;
        size_t j = i;
        while (j < line.size() && dir[j] == N) ++j;
        Dir prev = base, next = base;
        for (size_t k = i; k-- > 0;)
            if (dir[k] != N) { prev = dir[k]; break; }
        if (j < line.size()) next = dir[j];
        const Dir d = prev == next ? prev : base;
        for (size_t k = i; k < j; ++k) dir[k] = d;
        i = j - 1;
    }
    struct Run { Dir dir; std::u32string text; };
    std::vector<Run> runs;
    for (size_t i = 0; i < line.size(); ++i) {
        if (runs.empty() || runs.back().dir != dir[i]) runs.push_back({dir[i], {}});
        runs.back().text.push_back(line[i]);
    }
    std::u32string out;
    auto put = [&](const Run& r) {
        if (r.dir == R) {
            for (size_t k = r.text.size(); k-- > 0;)
                if (!Mark(r.text[k])) out.push_back(Mirror(r.text[k]));
        } else {
            for (char32_t c : r.text)
                if (!Mark(c)) out.push_back(c);
        }
    };
    if (base == R)
        for (size_t k = runs.size(); k-- > 0;) put(runs[k]);
    else
        for (const Run& r : runs) put(r);
    return out;
}

const char* Transform(const char* text, const char* text_end, int visual, const char** out_end) {
    // Quick reject: Arabic is two-byte UTF-8 0xD8-0xDB, presentation forms 0xEF 0xAD-0xBB.
    bool any = false;
    for (const char* p = text; p < text_end && !any; ++p) {
        const unsigned char b = (unsigned char)*p;
        any = visual ? (b >= 0xD8 && b <= 0xDB) || b == 0xEF : (b >= 0xD8 && b <= 0xDB);
    }
    if (!any) return nullptr;
    static std::unordered_map<std::string, std::string> cache[2];
    std::string key(text, size_t(text_end - text));
    auto& slot = cache[visual ? 1 : 0];
    auto it = slot.find(key);
    if (it == slot.end()) {
        if (slot.size() > 20000) slot.clear();  // bounded: the launcher's texts and its log
        const std::u32string in = Decode(text, text_end);
        bool needed = false;
        for (char32_t c : in) needed |= visual ? RightToLeft(c) : ArabicLetter(c);
        it = slot.emplace(std::move(key), needed ? Encode(visual ? Visual(in) : Shape(in)) : std::string("\x01")).first;
    }
    if (it->second == "\x01") return nullptr;
    *out_end = it->second.data() + it->second.size();
    return it->second.data();
}

} // namespace

void Install(bool on) { GImFontTextShape = on ? Transform : nullptr; }

} // namespace arabic
