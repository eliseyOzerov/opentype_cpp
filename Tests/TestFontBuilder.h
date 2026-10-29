// A tiny valid TrueType font built in memory, for the font and text tests, so no font file is needed.
//
// Units per em 1000, ascender 800, descender -200, line gap 100. Glyphs:
//   0 .notdef  empty, advance 500
//   1 'A'      a square (0,0) (500,0) (500,700) (0,700), all on-curve, advance 600
//   2 'B'      (0,0) on, (250,500) off, (500,0) on: a quadratic arch closed by a line, advance 500
//   3 'C'      composite: glyph 1 at offset (100, 50) scaled by 0.5, then glyph 2, advance 700
//   4 ' '      empty, advance 300
//   5 'O'      four off-curve points (0,0) (500,0) (500,500) (0,500): every on-curve point implied, advance 600
// kern: pair (A, B) = -50.
//
// The second half builds OpenType fonts with CFF outlines (sfnt version "OTTO"): `TestCharstring` writes
// Type 2 charstrings, `cffTable` lays out a CFF table around them (name-keyed or CID-keyed), and
// `cffTestFont` wraps it in head, hhea, maxp (version 0.5), hmtx and cmap. `minimalCFFTestFont` is the
// standard one, units per em 1000, nominalWidthX = defaultWidthX = 500:
//   0 .notdef  `endchar`; width 500 (the default)
//   1 'A'      `100 0 0 rmoveto 500 700 -500 hlineto endchar`: the square of the TrueType font, width 600
//   2 'B'      rrcurveto, vhcurveto (two groups) and hvcurveto (five arguments): five nodes, width 500
//   3 'C'      width -100, 5 + 3 stems, an implicit stem and a two-byte hintmask, a local and a global
//              subroutine called with the arguments left on the caller's stack; width 400
//   4 ' '      `-200 endchar`: no outline, width 300
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

namespace editor::testing {

struct TestFontOptions {
    /// Map characters with a format 12 cmap (adds U+1F600 -> glyph 1) instead of format 4.
    bool format12 = false;
    /// Store `loca` as 32-bit offsets instead of halved 16-bit ones.
    bool longLoca = false;
};

class TestBytes {
public:
    std::vector<uint8_t> data;
    void u8(int value) { data.push_back(static_cast<uint8_t>(value & 0xFF)); }
    void u16(int value) {
        u8(value >> 8);
        u8(value);
    }
    void i16(int value) { u16(value & 0xFFFF); }
    void u32(uint32_t value) {
        u16(static_cast<int>(value >> 16));
        u16(static_cast<int>(value & 0xFFFF));
    }
    void append(const TestBytes& other) { data.insert(data.end(), other.data.begin(), other.data.end()); }
    void pad(int multiple) {
        while (data.size() % static_cast<size_t>(multiple) != 0) u8(0);
    }
};

struct TestPoint {
    int x;
    int y;
    bool on;
};

/// A simple glyph: header, end points, flags with short vectors and repeats, then x and y deltas.
inline TestBytes simpleGlyph(const std::vector<std::vector<TestPoint>>& contours) {
    std::vector<TestPoint> points;
    std::vector<int> ends;
    for (const auto& contour : contours) {
        points.insert(points.end(), contour.begin(), contour.end());
        ends.push_back(static_cast<int>(points.size()) - 1);
    }
    int minX = points[0].x, maxX = points[0].x, minY = points[0].y, maxY = points[0].y;
    for (const TestPoint& point : points) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    TestBytes glyph;
    glyph.i16(static_cast<int>(contours.size()));
    glyph.i16(minX);
    glyph.i16(minY);
    glyph.i16(maxX);
    glyph.i16(maxY);
    for (int end : ends) glyph.u16(end);
    glyph.u16(0);

    std::vector<int> flags;
    TestBytes xs, ys;
    int previousX = 0, previousY = 0;
    for (const TestPoint& point : points) {
        int flag = point.on ? 1 : 0;
        const int dx = point.x - previousX;
        const int dy = point.y - previousY;
        if (dx == 0) {
            flag |= 0x10;
        } else if (std::abs(dx) < 256) {
            flag |= 0x02 | (dx > 0 ? 0x10 : 0);
            xs.u8(std::abs(dx));
        } else {
            xs.i16(dx);
        }
        if (dy == 0) {
            flag |= 0x20;
        } else if (std::abs(dy) < 256) {
            flag |= 0x04 | (dy > 0 ? 0x20 : 0);
            ys.u8(std::abs(dy));
        } else {
            ys.i16(dy);
        }
        flags.push_back(flag);
        previousX = point.x;
        previousY = point.y;
    }
    for (size_t index = 0; index < flags.size();) {
        size_t run = 1;
        while (index + run < flags.size() && flags[index + run] == flags[index] && run < 256) run += 1;
        if (run > 1) {
            glyph.u8(flags[index] | 0x08);
            glyph.u8(static_cast<int>(run) - 1);
        } else {
            glyph.u8(flags[index]);
        }
        index += run;
    }
    glyph.append(xs);
    glyph.append(ys);
    glyph.pad(2);
    return glyph;
}

/// The file around `tables` (sorted by tag by the caller): the sfnt header, one directory record per
/// table (checksums left 0, which the reader does not check), then the tables padded to four bytes.
inline std::vector<uint8_t> assembleSfnt(uint32_t version, const std::vector<std::pair<std::string, TestBytes>>& tables) {
    TestBytes file;
    file.u32(version);
    file.u16(static_cast<int>(tables.size()));
    file.u16(128);
    file.u16(3);
    file.u16(0);
    size_t offset = 12 + 16 * tables.size();
    TestBytes body;
    for (const auto& [tag, table] : tables) {
        for (char character : tag) file.u8(character);
        file.u32(0);
        file.u32(static_cast<uint32_t>(offset + body.data.size()));
        file.u32(static_cast<uint32_t>(table.data.size()));
        TestBytes padded = table;
        padded.pad(4);
        body.append(padded);
    }
    file.append(body);
    return file.data;
}

inline std::vector<uint8_t> minimalTestFont(TestFontOptions options = {}) {
    // Glyph data.
    std::vector<TestBytes> glyphs(6);
    glyphs[1] = simpleGlyph({{{0, 0, true}, {500, 0, true}, {500, 700, true}, {0, 700, true}}});
    glyphs[2] = simpleGlyph({{{0, 0, true}, {250, 500, false}, {500, 0, true}}});
    // Composite: glyph 1 at (100, 50) scaled 0.5 (word arguments, xy values, a scale, more components), then glyph 2 (byte arguments).
    TestBytes composite;
    composite.i16(-1);
    for (int index = 0; index < 4; index += 1) composite.i16(0);
    composite.u16(0x0001 | 0x0002 | 0x0008 | 0x0020);
    composite.u16(1);
    composite.i16(100);
    composite.i16(50);
    composite.i16(0x2000);
    composite.u16(0x0002);
    composite.u16(2);
    composite.u8(0);
    composite.u8(0);
    composite.pad(2);
    glyphs[3] = composite;
    glyphs[5] = simpleGlyph({{{0, 0, false}, {500, 0, false}, {500, 500, false}, {0, 500, false}}});

    TestBytes glyf, loca;
    for (const TestBytes& glyph : glyphs) {
        if (options.longLoca) loca.u32(static_cast<uint32_t>(glyf.data.size())); else loca.u16(static_cast<int>(glyf.data.size() / 2));
        glyf.append(glyph);
    }
    if (options.longLoca) loca.u32(static_cast<uint32_t>(glyf.data.size())); else loca.u16(static_cast<int>(glyf.data.size() / 2));

    TestBytes head;
    head.u32(0x00010000);
    head.u32(0x00010000);
    head.u32(0);
    head.u32(0x5F0F3CF5);
    head.u16(0);
    head.u16(1000);
    for (int index = 0; index < 4; index += 1) head.u32(0);
    for (int index = 0; index < 4; index += 1) head.i16(0);
    head.u16(0);
    head.u16(8);
    head.i16(2);
    head.i16(options.longLoca ? 1 : 0);
    head.i16(0);

    TestBytes maxp;
    maxp.u32(0x00010000);
    maxp.u16(6);
    for (int index = 0; index < 13; index += 1) maxp.u16(0);

    const int advances[6] = {500, 600, 500, 700, 300, 600};
    TestBytes hhea;
    hhea.u32(0x00010000);
    hhea.i16(800);
    hhea.i16(-200);
    hhea.i16(100);
    hhea.u16(700);
    for (int index = 0; index < 11; index += 1) hhea.i16(0);
    hhea.u16(6);
    TestBytes hmtx;
    for (int advance : advances) {
        hmtx.u16(advance);
        hmtx.i16(0);
    }

    TestBytes cmap;
    cmap.u16(0);
    cmap.u16(1);
    cmap.u16(3);
    cmap.u16(options.format12 ? 10 : 1);
    cmap.u32(12);
    if (options.format12) {
        const uint32_t groups[4][3] = {{32, 32, 4}, {65, 67, 1}, {79, 79, 5}, {0x1F600, 0x1F600, 1}};
        cmap.u16(12);
        cmap.u16(0);
        cmap.u32(16 + 12 * 4);
        cmap.u32(0);
        cmap.u32(4);
        for (const auto& group : groups) {
            cmap.u32(group[0]);
            cmap.u32(group[1]);
            cmap.u32(group[2]);
        }
    } else {
        const int ends[4] = {32, 67, 79, 0xFFFF};
        const int starts[4] = {32, 65, 79, 0xFFFF};
        const int deltas[4] = {4 - 32, 1 - 65, 5 - 79, 1};
        cmap.u16(4);
        cmap.u16(16 + 8 * 4);
        cmap.u16(0);
        cmap.u16(8);
        cmap.u16(4);
        cmap.u16(1);
        cmap.u16(4);
        for (int end : ends) cmap.u16(end);
        cmap.u16(0);
        for (int start : starts) cmap.u16(start);
        for (int delta : deltas) cmap.i16(delta);
        for (int index = 0; index < 4; index += 1) cmap.u16(0);
    }

    TestBytes kern;
    kern.u16(0);
    kern.u16(1);
    kern.u16(0);
    kern.u16(14 + 6);
    kern.u16(0x0001);
    kern.u16(1);
    kern.u16(6);
    kern.u16(0);
    kern.u16(0);
    kern.u16(1);
    kern.u16(2);
    kern.i16(-50);

    return assembleSfnt(0x00010000, {{"cmap", cmap}, {"glyf", glyf}, {"head", head}, {"hhea", hhea}, {"hmtx", hmtx}, {"kern", kern}, {"loca", loca}, {"maxp", maxp}});
}

// ---- OpenType fonts with CFF outlines ------------------------------------------------------------

/// The Type 2 charstring operators the tests write.
namespace cs {
constexpr int hstem = 1, vstem = 3, vmoveto = 4, rlineto = 5, hlineto = 6, vlineto = 7, rrcurveto = 8, callsubr = 10,
              returnFromSubroutine = 11, endchar = 14, hstemhm = 18, hintmask = 19, cntrmask = 20, rmoveto = 21, hmoveto = 22,
              vstemhm = 23, rcurveline = 24, rlinecurve = 25, vvcurveto = 26, hhcurveto = 27, callgsubr = 29, vhcurveto = 30,
              hvcurveto = 31;
/// The escaped operators (12 n): the second byte.
constexpr int hflex = 34, flex = 35, hflex1 = 36, flex1 = 37;
} // namespace cs

/// A number as the compact one-to-three byte form CFF DICTs and charstrings share: 32 to 246 for
/// -107 to 107, 247 to 254 and a second byte out to ±1131, else 28 and a 16-bit integer.
inline void appendCompactNumber(TestBytes& bytes, int value) {
    if (value >= -107 && value <= 107) {
        bytes.u8(value + 139);
    } else if (value >= 108 && value <= 1131) {
        bytes.u8(247 + (value - 108) / 256);
        bytes.u8((value - 108) % 256);
    } else if (value >= -1131 && value <= -108) {
        bytes.u8(251 + (-value - 108) / 256);
        bytes.u8((-value - 108) % 256);
    } else {
        bytes.u8(28);
        bytes.i16(value);
    }
}

/// A Type 2 charstring written call by call: `TestCharstring().args({100, 0, 0}).op(cs::rmoveto)`.
class TestCharstring {
public:
    TestBytes bytes;
    TestCharstring& n(int value) {
        appendCompactNumber(bytes, value);
        return *this;
    }
    TestCharstring& args(std::initializer_list<int> values) {
        for (int value : values) n(value);
        return *this;
    }
    /// A 16.16 fixed-point operand: byte 255 and a 32-bit integer of value · 65536.
    TestCharstring& fixed(float value) {
        bytes.u8(255);
        bytes.u32(static_cast<uint32_t>(static_cast<int32_t>(value * 65536.0f)));
        return *this;
    }
    TestCharstring& op(int code) {
        bytes.u8(code);
        return *this;
    }
    TestCharstring& escaped(int code) {
        bytes.u8(12);
        bytes.u8(code);
        return *this;
    }
    /// A byte as it is, such as the mask after `hintmask`.
    TestCharstring& raw(int value) {
        bytes.u8(value);
        return *this;
    }
};

/// A CFF INDEX of `objects` with two-byte offsets.
inline TestBytes cffIndex(const std::vector<TestBytes>& objects) {
    TestBytes index;
    index.u16(static_cast<int>(objects.size()));
    if (objects.empty()) return index;
    index.u8(2);
    int offset = 1;
    index.u16(offset);
    for (const TestBytes& object : objects) {
        offset += static_cast<int>(object.data.size());
        index.u16(offset);
    }
    for (const TestBytes& object : objects) index.append(object);
    return index;
}

/// A DICT integer in the fixed five-byte form (29 and a 32-bit integer), so an offset's size does
/// not depend on its value.
inline void appendDictOffset(TestBytes& dict, int value) {
    dict.u8(29);
    dict.u32(static_cast<uint32_t>(value));
}

/// A DICT real number (byte 30, packed nibbles). `text` uses the digits, '.', '-', 'e' for the
/// exponent mark and 'n' for "E-" (so "1n3" is 1E-3).
inline void appendDictReal(TestBytes& dict, const std::string& text) {
    std::vector<int> nibbles;
    for (char character : text) {
        if (character >= '0' && character <= '9') nibbles.push_back(character - '0');
        else if (character == '.') nibbles.push_back(0xA);
        else if (character == 'e') nibbles.push_back(0xB);
        else if (character == 'n') nibbles.push_back(0xC);
        else if (character == '-') nibbles.push_back(0xE);
    }
    nibbles.push_back(0xF);
    if (nibbles.size() % 2 == 1) nibbles.push_back(0xF);
    dict.u8(30);
    for (size_t index = 0; index < nibbles.size(); index += 2) dict.u8((nibbles[index] << 4) | nibbles[index + 1]);
}

inline void appendDictOperator(TestBytes& dict, int code) {
    if (code >= 0x0C00) dict.u8(12);
    dict.u8(code & 0xFF);
}

/// The FontMatrix operator and its six reals, when `numbers` has them.
inline void appendDictMatrix(TestBytes& dict, const std::vector<std::string>& numbers) {
    if (numbers.empty()) return;
    for (const std::string& number : numbers) appendDictReal(dict, number);
    appendDictOperator(dict, 0x0C07);
}

struct CFFTestFontDictionary {
    std::vector<TestBytes> localSubroutines;
    int defaultWidthX = 500;
    int nominalWidthX = 500;
    /// Six reals as text, such as {"0.001", "0", "0", "0.001", "0", "0"}; empty gives the FD no matrix.
    std::vector<std::string> fontMatrix;
};

struct CFFTestFontSpec {
    /// Charstrings, glyph 0 (.notdef) first.
    std::vector<TestBytes> charstrings;
    std::vector<TestBytes> globalSubroutines;
    /// The Private DICT of a name-keyed font.
    CFFTestFontDictionary privateData;
    /// The Top DICT's FontMatrix, six reals as text; empty gives none.
    std::vector<std::string> fontMatrix;
    /// Non-empty makes the font CID-keyed: these are its FDs, `dictionaryOfGlyph` says which each glyph uses
    /// and `selectFormat0` writes FDSelect as one byte per glyph instead of ranges (format 3).
    std::vector<CFFTestFontDictionary> fontDictionaries;
    std::vector<int> dictionaryOfGlyph;
    bool selectFormat0 = false;
    /// Advance widths for `hmtx`, one per glyph (500 when empty).
    std::vector<int> advances;
    int unitsPerEm = 1000;
    /// The table's tag, "CFF " or, for a refusal test, "CFF2".
    std::string tableTag = "CFF ";
};

/// The bytes of the `CFF ` table for `spec`: header, name, Top DICT, string and global subroutine
/// INDEXes, the charstrings, then the Private DICT(s) with their local subroutines (and, for a CID font,
/// FDSelect and the FDArray). Offsets are written in a fixed size so the layout is worked out in two passes.
inline TestBytes cffTable(const CFFTestFontSpec& spec) {
    const bool isCID = !spec.fontDictionaries.empty();
    TestBytes header;
    header.u8(1);
    header.u8(0);
    header.u8(4);
    header.u8(2);
    const TestBytes nameIndex = cffIndex({TestBytes{{'T', 'e', 's', 't'}}});
    const TestBytes stringIndex = cffIndex({});
    const TestBytes globalIndex = cffIndex(spec.globalSubroutines);
    const TestBytes charStringsIndex = cffIndex(spec.charstrings);

    // A Private DICT with its widths and, when `hasSubroutines`, the Subrs offset (from the start of the
    // dictionary, which is its own size, as the subroutines follow it).
    auto privateDictionary = [](const CFFTestFontDictionary& dictionary) {
        auto build = [&](int subroutinesOffset) {
            TestBytes dict;
            appendCompactNumber(dict, dictionary.defaultWidthX);
            appendDictOperator(dict, 20);
            appendCompactNumber(dict, dictionary.nominalWidthX);
            appendDictOperator(dict, 21);
            if (!dictionary.localSubroutines.empty()) {
                appendDictOffset(dict, subroutinesOffset);
                appendDictOperator(dict, 19);
            }
            return dict;
        };
        return build(static_cast<int>(build(0).data.size()));
    };

    auto topDictionary = [&](int charStringsOffset, int privateSize, int privateOffset, int arrayOffset, int selectOffset) {
        TestBytes dict;
        if (isCID) {
            appendCompactNumber(dict, 391);
            appendCompactNumber(dict, 392);
            appendCompactNumber(dict, 0);
            appendDictOperator(dict, 0x0C1E);
        }
        appendDictMatrix(dict, spec.fontMatrix);
        appendDictOffset(dict, charStringsOffset);
        appendDictOperator(dict, 17);
        if (isCID) {
            appendDictOffset(dict, arrayOffset);
            appendDictOperator(dict, 0x0C24);
            appendDictOffset(dict, selectOffset);
            appendDictOperator(dict, 0x0C25);
        } else {
            appendDictOffset(dict, privateSize);
            appendDictOffset(dict, privateOffset);
            appendDictOperator(dict, 18);
        }
        return dict;
    };

    const int topIndexSize = static_cast<int>(cffIndex({topDictionary(0, 0, 0, 0, 0)}).data.size());
    int cursor = static_cast<int>(header.data.size() + nameIndex.data.size()) + topIndexSize +
                 static_cast<int>(stringIndex.data.size() + globalIndex.data.size());
    const int charStringsOffset = cursor;
    cursor += static_cast<int>(charStringsIndex.data.size());

    TestBytes rest; // everything after the charstrings
    int privateSize = 0, privateOffset = 0, arrayOffset = 0, selectOffset = 0;
    if (!isCID) {
        const TestBytes dictionary = privateDictionary(spec.privateData);
        privateSize = static_cast<int>(dictionary.data.size());
        privateOffset = cursor;
        rest.append(dictionary);
        if (!spec.privateData.localSubroutines.empty()) rest.append(cffIndex(spec.privateData.localSubroutines));
    } else {
        TestBytes select;
        if (spec.selectFormat0) {
            select.u8(0);
            for (size_t glyph = 0; glyph < spec.charstrings.size(); glyph += 1) select.u8(spec.dictionaryOfGlyph[glyph]);
        } else {
            std::vector<std::pair<int, int>> ranges; // first glyph, font dictionary
            for (size_t glyph = 0; glyph < spec.charstrings.size(); glyph += 1) {
                if (ranges.empty() || ranges.back().second != spec.dictionaryOfGlyph[glyph]) {
                    ranges.push_back({static_cast<int>(glyph), spec.dictionaryOfGlyph[glyph]});
                }
            }
            select.u8(3);
            select.u16(static_cast<int>(ranges.size()));
            for (const auto& [first, dictionary] : ranges) {
                select.u16(first);
                select.u8(dictionary);
            }
            select.u16(static_cast<int>(spec.charstrings.size()));
        }
        selectOffset = cursor;
        rest.append(select);
        std::vector<TestBytes> fontDictionaryData;
        for (const CFFTestFontDictionary& dictionary : spec.fontDictionaries) {
            const TestBytes privateData = privateDictionary(dictionary);
            TestBytes fontDictionary;
            appendDictOffset(fontDictionary, static_cast<int>(privateData.data.size()));
            appendDictOffset(fontDictionary, cursor + static_cast<int>(rest.data.size()));
            appendDictOperator(fontDictionary, 18);
            appendDictMatrix(fontDictionary, dictionary.fontMatrix);
            fontDictionaryData.push_back(fontDictionary);
            rest.append(privateData);
            if (!dictionary.localSubroutines.empty()) rest.append(cffIndex(dictionary.localSubroutines));
        }
        arrayOffset = cursor + static_cast<int>(rest.data.size());
        rest.append(cffIndex(fontDictionaryData));
    }

    TestBytes table;
    table.append(header);
    table.append(nameIndex);
    table.append(cffIndex({topDictionary(charStringsOffset, privateSize, privateOffset, arrayOffset, selectOffset)}));
    table.append(stringIndex);
    table.append(globalIndex);
    table.append(charStringsIndex);
    table.append(rest);
    return table;
}

/// An OpenType font (sfnt version "OTTO") around the CFF table of `spec`: head, hhea, maxp 0.5, hmtx and a
/// format 4 cmap that maps 'A' onward to glyphs 1 onward.
inline std::vector<uint8_t> cffTestFont(const CFFTestFontSpec& spec) {
    const int glyphCount = static_cast<int>(spec.charstrings.size());
    TestBytes head;
    head.u32(0x00010000);
    head.u32(0x00010000);
    head.u32(0);
    head.u32(0x5F0F3CF5);
    head.u16(0);
    head.u16(spec.unitsPerEm);
    for (int index = 0; index < 4; index += 1) head.u32(0);
    for (int index = 0; index < 4; index += 1) head.i16(0);
    head.u16(0);
    head.u16(8);
    head.i16(2);
    head.i16(0);
    head.i16(0);

    TestBytes maxp;
    maxp.u32(0x00005000);
    maxp.u16(glyphCount);

    TestBytes hhea;
    hhea.u32(0x00010000);
    hhea.i16(800);
    hhea.i16(-200);
    hhea.i16(100);
    hhea.u16(700);
    for (int index = 0; index < 11; index += 1) hhea.i16(0);
    hhea.u16(glyphCount);
    TestBytes hmtx;
    for (int glyph = 0; glyph < glyphCount; glyph += 1) {
        hmtx.u16(spec.advances.empty() ? 500 : spec.advances[static_cast<size_t>(glyph)]);
        hmtx.i16(0);
    }

    // One segment 'A' ... 'A' + glyphCount - 2 onto glyphs 1 ..., and the closing segment.
    TestBytes cmap;
    cmap.u16(0);
    cmap.u16(1);
    cmap.u16(3);
    cmap.u16(1);
    cmap.u32(12);
    cmap.u16(4);
    cmap.u16(32);
    cmap.u16(0);
    cmap.u16(4);
    cmap.u16(4);
    cmap.u16(1);
    cmap.u16(0);
    cmap.u16(65 + std::max(glyphCount - 2, 0));
    cmap.u16(0xFFFF);
    cmap.u16(0);
    cmap.u16(65);
    cmap.u16(0xFFFF);
    cmap.i16(1 - 65);
    cmap.i16(1);
    cmap.u16(0);
    cmap.u16(0);

    return assembleSfnt(0x4F54544F, {{spec.tableTag, cffTable(spec)}, {"cmap", cmap}, {"head", head}, {"hhea", hhea}, {"hmtx", hmtx}, {"maxp", maxp}});
}

/// The spec of the standard CFF test font described at the top of this file.
inline CFFTestFontSpec minimalCFFTestSpec() {
    CFFTestFontSpec spec;
    spec.advances = {500, 600, 500, 400, 300};
    spec.charstrings.push_back(TestCharstring().op(cs::endchar).bytes);
    spec.charstrings.push_back(TestCharstring().args({100, 0, 0}).op(cs::rmoveto).args({500, 700, -500}).op(cs::hlineto).op(cs::endchar).bytes);
    spec.charstrings.push_back(TestCharstring()
                                   .args({100, 0})
                                   .op(cs::rmoveto)
                                   .args({50, 0, 50, 50, 0, 100})
                                   .op(cs::rrcurveto)
                                   .args({30, -20, 20, -30, -40, -20, -20, -30})
                                   .op(cs::vhcurveto)
                                   .args({-30, -20, -20, -40, 10})
                                   .op(cs::hvcurveto)
                                   .op(cs::endchar)
                                   .bytes);
    // Width -100, five horizontal stems, three vertical stems, then two numbers that become a ninth stem
    // when the hintmask meets them, so the mask is two bytes; its bytes are `rmoveto` and `endchar` if
    // they were ever run.
    spec.charstrings.push_back(TestCharstring()
                                   .args({-100, 0, 50, 100, 50, 200, 50, 300, 50, 400, 50})
                                   .op(cs::hstemhm)
                                   .args({0, 40, 100, 40, 200, 40})
                                   .op(cs::vstemhm)
                                   .args({10, 20})
                                   .op(cs::hintmask)
                                   .raw(cs::rmoveto)
                                   .raw(cs::endchar)
                                   .args({20, 30})
                                   .op(cs::rmoveto)
                                   .args({100, -107})
                                   .op(cs::callsubr)
                                   .args({80, -107})
                                   .op(cs::callgsubr)
                                   .args({-100})
                                   .op(cs::hlineto)
                                   .op(cs::endchar)
                                   .bytes);
    spec.charstrings.push_back(TestCharstring().args({-200}).op(cs::endchar).bytes);
    spec.privateData.localSubroutines.push_back(TestCharstring().op(cs::hlineto).op(cs::returnFromSubroutine).bytes);
    spec.globalSubroutines.push_back(TestCharstring().op(cs::vlineto).op(cs::returnFromSubroutine).bytes);
    return spec;
}

inline std::vector<uint8_t> minimalCFFTestFont() { return cffTestFont(minimalCFFTestSpec()); }

} // namespace editor::testing
