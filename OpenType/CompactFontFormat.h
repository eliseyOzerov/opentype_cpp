// The outlines of an OpenType font that are not TrueType: the `CFF ` table (Compact Font Format,
// version 1) and its Type 2 charstrings. Written from Adobe Technical Note #5176 (the Compact Font
// Format Specification) and #5177 (The Type 2 Charstring Format). The container (INDEX, DICT, Top
// DICT keys, Private DICT, FDArray and FDSelect) is read in the structure of the SwiftOTF package's
// CFF reader, rewritten to these rules; the charstring interpreter is new, as SwiftOTF does not run
// charstrings. `OpenTypeFont` hands in the table's bytes and takes the contours it gets back.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "FontOutline.h"

namespace editor {

/// Where one object of a CFF INDEX lives: its first byte in the table and its length.
///
/// Use when: reading a charstring, a subroutine or a DICT out of the table
/// Tags: cff index object, byte range
/// Node: none (file format)
struct CFFObjectRange {
    size_t start = 0;
    size_t length = 0;
};

/// A CFF INDEX: an array of variable-sized objects, as the ranges of their bytes, and the offset
/// just past the whole INDEX (where the next structure starts).
///
/// How: Card16 count; when not zero, a one-byte offSize (1 to 4), count + 1 offsets of offSize bytes
/// each (counted from the byte before the data, so the first is 1), then the data. An empty INDEX is
/// only the two count bytes.
///
/// Use when: reading the name, string, subroutine and charstring lists of a CFF table
/// Tags: cff index, object array, subroutines, charstrings
/// Node: none (file format)
struct CFFIndex {
    std::vector<CFFObjectRange> objects;
    size_t end = 0;
};

/// A glyph read from a charstring: its outline in charstring units (before the font matrix) and its
/// advance width. The outline's pieces are lines and cubics only. A charstring closes every contour
/// itself (a move or `endchar` ends the last one), so the last piece of a contour may stop short of
/// its start; the straight line back is implied.
///
/// Use when: reading a glyph out of CompactFontFormat
/// Tags: cff glyph, charstring result, glyph width
/// Node: none (file format)
struct CompactGlyph {
    GlyphOutline outline;
    /// The advance width from the charstring: the font's `nominalWidthX` plus the first argument when
    /// the charstring gives one, else its `defaultWidthX`. `hmtx` is the width the layout uses.
    float width = 0;
};

/// A CFF version 1 table held in memory: its charstrings, subroutines and per-font data, ready to
/// give each glyph's contours.
///
/// Conventions: coordinates are charstring units, y up with the baseline at 0, as stored; the
/// font matrix (`fontMatrix`) says how many ems one unit is. Every read is bounds-checked: a damaged
/// table throws a `FontProblem` rather than reading past its end. CFF2 (variable fonts) is not
/// read; `OpenTypeFont` refuses those fonts before it gets here. Name-keyed and CID-keyed fonts are
/// both read (a CID font's FDSelect, format 0 or 3, picks each glyph's Private DICT and so its
/// subroutines and widths). Glyph names and the charset are not read: OpenType addresses glyphs by
/// number through `cmap`.
///
/// Use when: reading the outlines of an OpenType font with CFF outlines
/// Tags: cff, compact font format, otf, opentype, charstring, type 2, glyph outline
/// Node: none (file format)
class CompactFontFormat {
public:
    /// The font the bytes of a `CFF ` table hold. Reads the header, INDEXes, Top DICT, the Private
    /// DICTs (or the FDArray and FDSelect of a CID font) and the subroutine INDEXes up front, so a
    /// damaged table throws here; charstrings are run when a glyph is asked for.
    ///
    /// Use when: loading the CFF table of an OpenType font
    /// Tags: parse cff, read cff table
    /// Node: none (file format)
    static CompactFontFormat parse(std::vector<uint8_t> table);

    /// How many charstrings (glyphs) the table has.
    ///
    /// Use when: checking a glyph number
    /// Tags: glyph count, charstrings count
    /// Node: none (file format)
    int glyphCount() const { return static_cast<int>(charStrings.objects.size()); }

    /// True for a CID-keyed font (the Top DICT has a ROS operator), whose glyphs choose their
    /// Private DICT through FDSelect.
    ///
    /// Use when: telling the two kinds of CFF font apart
    /// Tags: cid keyed, ros, fdselect
    /// Node: none (file format)
    bool isCIDKeyed() const { return cidKeyed; }

    /// Runs the charstring of glyph `glyphNumber` and returns its contours and width. A glyph number that is not
    /// in the font gives an empty glyph. Throws a `FontProblem` for a charstring that is damaged,
    /// breaks the Type 2 limits (stack of 48, subroutines nested 10 deep, at most 1,000,000
    /// operators) or uses what is not supported: the accented form of `endchar` (seac, four
    /// arguments) and the arithmetic, storage and conditional operators (`12 n`, deprecated by the
    /// specification).
    ///
    /// Use when: turning a CFF glyph into an outline
    /// Tags: charstring interpreter, type 2, glyph contours
    /// Node: none (file format)
    CompactGlyph glyph(int glyphNumber) const;

    /// The matrix [a b c d e f] that takes a point of glyph `glyphNumber` from charstring units to ems:
    /// x' = a·x + c·y + e, y' = b·x + d·y + f. The Top DICT's FontMatrix, the glyph's FD's own matrix
    /// when a CID font has one, or both applied one after the other (FD first); the default
    /// [0.001 0 0 0.001 0 0] (1000 units per em) when neither is given.
    ///
    /// Use when: scaling a CFF glyph by hand
    /// Tags: font matrix, units to em
    /// Node: none (file format)
    std::array<float, 6> fontMatrix(int glyphNumber) const;

    /// True when the font matrix of glyph `glyphNumber` is the default 0.001 scale, so the font's units are the
    /// `head.unitsPerEm` of the OpenType font and `OpenTypeFont` leaves the outline as it is; false for a
    /// font with its own matrix, which `OpenTypeFont` then applies.
    ///
    /// Use when: choosing how to scale a CFF glyph
    /// Tags: default font matrix, units per em
    /// Node: none (file format)
    bool usesDefaultFontMatrix(int glyphNumber) const;

private:
    /// The data of one Private DICT and the FD (font dictionary) that points to it: widths, local
    /// subroutines, and a matrix when the FD has its own. A name-keyed font has one, a CID-keyed font
    /// one per FD.
    struct FontDictionary {
        float defaultWidth = 0;
        float nominalWidth = 0;
        CFFIndex localSubroutines;
        std::optional<std::array<float, 6>> matrix;
    };

    std::vector<uint8_t> bytes;
    CFFIndex globalSubroutines;
    CFFIndex charStrings;
    std::vector<FontDictionary> dictionaries;
    /// Which `dictionaries` entry each glyph uses (FDSelect, expanded); empty means entry 0 for all.
    std::vector<uint8_t> dictionaryOfGlyph;
    std::optional<std::array<float, 6>> topMatrix;
    bool cidKeyed = false;

    const FontDictionary& dictionaryFor(int glyphNumber) const;
};

} // namespace editor
