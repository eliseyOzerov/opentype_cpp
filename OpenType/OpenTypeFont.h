// A TrueType or OpenType font read from its bytes: which glyph a character has, how far each glyph
// advances, and the glyph's outline, all in the font's own units. Written from Apple's published
// TrueType specification and the OpenType specification (the `sfnt` table directory, `head`, `maxp`,
// `cmap`, `hhea`, `hmtx`, `loca`, `glyf`, `kern`) and, for OpenType fonts with CFF outlines, Adobe
// Technical Notes #5176 and #5177 (`CompactFontFormat`, whose container structure follows the
// SwiftOTF package's reader); no font library is used and nothing outside the C++ standard library.
// The caller hands in the bytes of a font file and scales the units to whatever it draws in.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "CompactFontFormat.h"
#include "FontOutline.h"

namespace editor {

/// A TrueType or OpenType font file held in memory and read table by table.
///
/// Conventions: every number comes out in font units, as the font stores it: `unitsPerEm` of them make
/// one em, x runs right and y up, the baseline is y = 0. Every read is bounds-checked: a damaged file
/// throws a `FontProblem` rather than reading past its end. Two kinds of outline are read: TrueType
/// (`glyf`, sfnt version 0x00010000 or "true"; quadratic pieces) and CFF version 1 (a `CFF ` table
/// with Type 2 charstrings, sfnt version "OTTO", read by `CompactFontFormat`; cubic pieces).
/// `cmap`, `hmtx`, `hhea` and `kern` are shared by both kinds. A font with only a `CFF2` table
/// (variable OpenType) is refused with a readable message naming CFF2.
///
/// Use when: turning text into outlines; measuring text for layout; placing text on a path
/// Tags: truetype, ttf, opentype, otf, cff, font file, glyph outline, cmap, glyf, hmtx, kerning, text
/// Node: none (file format)
class OpenTypeFont {
public:
    /// The font the bytes hold. Reads the table directory and `head`, `maxp`, `hhea`, `cmap` and the
    /// tables the outlines need up front (`loca` and `glyf`, or the whole `CFF ` table), so a font
    /// missing one throws here with its name.
    ///
    /// Use when: loading the bytes of a font file
    /// Tags: parse font, load ttf, load otf, read font bytes
    /// Node: none (file format)
    static OpenTypeFont parsed(std::vector<uint8_t> bytes);

    /// Font units in one em (`head.unitsPerEm`, usually 1000 or 2048).
    ///
    /// Use when: scaling font units to a size
    /// Tags: units per em, em size
    /// Node: none (file format)
    int unitsPerEm() const { return unitsPerEmValue; }

    /// How many glyphs the font has (`maxp.numGlyphs`).
    ///
    /// Use when: checking a glyph number
    /// Tags: glyph count
    /// Node: none (file format)
    int glyphCount() const { return glyphCountValue; }

    /// The distance from the baseline up to the top of the line (`hhea.ascender`), in font units.
    ///
    /// Use when: placing a line of text by its top
    /// Tags: ascent, ascender, line top
    /// Node: none (file format)
    int ascender() const { return ascenderUnits; }

    /// The distance from the baseline down to the bottom of the line, as a negative number
    /// (`hhea.descender`), in font units.
    ///
    /// Use when: placing a line of text by its bottom
    /// Tags: descent, descender, line bottom
    /// Node: none (file format)
    int descender() const { return descenderUnits; }

    /// The extra space between two lines (`hhea.lineGap`), in font units.
    ///
    /// Use when: stacking lines of text
    /// Tags: line gap, leading
    /// Node: none (file format)
    int lineGap() const { return lineGapUnits; }

    /// The glyph the font draws for Unicode scalar `codepoint`, or 0 (the font's "missing glyph") when
    /// it has none.
    ///
    /// How: the `cmap` subtable, format 12 (segmented coverage, 32-bit) when there is one, else format 4
    /// (segment mapping to delta values, 16-bit): find the segment whose range holds the character, then
    /// add the segment's delta or read the glyph from its index array.
    ///
    /// Use when: turning a string into glyphs
    /// Tags: character to glyph, cmap lookup, unicode to glyph
    /// Node: none (file format)
    int glyphIndex(uint32_t codepoint) const;

    /// How far the pen moves after `glyph` (`hmtx` advance width), in font units. A glyph beyond the
    /// font's metrics list shares the last advance, as the format says; a glyph that is not there advances 0.
    ///
    /// Use when: laying out text; centring a glyph on a path
    /// Tags: advance width, glyph width, hmtx
    /// Node: none (file format)
    int advance(int glyph) const;

    /// The extra pen movement between `left` and `right` from the `kern` table (format 0, horizontal),
    /// in font units; 0 when the font has no pair for them or no `kern` table.
    ///
    /// Use when: spacing pairs such as "AV"
    /// Tags: kerning, pair kerning, kern table
    /// Node: none (file format)
    int kerning(int left, int right) const;

    /// The outline of `glyph` in font units, with the pen at the origin: one closed contour per
    /// TrueType or CFF contour. A TrueType quadratic piece (on-curve, off-curve, on-curve points) stays a
    /// quadratic piece with its control point; two off-curve points in a row imply an on-curve point at
    /// their midpoint, and a contour that starts off-curve starts at that midpoint. A composite glyph is
    /// its parts' outlines moved and scaled by each part's offset and matrix. A CFF glyph is its
    /// charstring's contours: lines and cubic pieces. A CFF font that gives its own font matrix (not the
    /// default 0.001 scale, so its charstring units are not `unitsPerEm` to the em) has its points
    /// mapped by that matrix times `unitsPerEm`, so that every font, TrueType or CFF, comes out in units
    /// of `unitsPerEm` per em. An empty glyph (a space) or a glyph number that is not in the font gives
    /// no contours; a contour with fewer than two pieces draws nothing and is left out of a TrueType glyph.
    ///
    /// Use when: turning text into outlines; drawing a glyph in the editor
    /// Tags: glyph outline, glyph contours, truetype contours, composite glyph, cff outline, quadratic, cubic
    /// Node: none (file format)
    GlyphOutline outline(int glyph) const;

private:
    struct Table {
        uint32_t offset = 0;
        uint32_t length = 0;
        bool present = false;
    };
    struct GlyphPoint {
        float x = 0;
        float y = 0;
        bool isOnCurve = true;
    };
    using Contour = std::vector<GlyphPoint>;

    std::vector<uint8_t> bytes;
    Table head, maxp, cmap, hhea, hmtx, loca, glyf, kern;
    /// The outlines of a font with a `CFF ` table; empty for a TrueType font.
    std::optional<CompactFontFormat> cff;
    int unitsPerEmValue = 1;
    int glyphCountValue = 0;
    bool hasLongOffsets = false;
    int metricCount = 0;
    int ascenderUnits = 0;
    int descenderUnits = 0;
    int lineGapUnits = 0;
    /// Where the chosen `cmap` subtable starts, and its format (4 or 12).
    uint32_t cmapOffset = 0;
    int cmapFormat = 0;

    OpenTypeFont() = default;

    uint8_t byteAt(size_t offset) const;
    uint16_t unsigned16(size_t offset) const;
    int16_t signed16(size_t offset) const;
    uint32_t unsigned32(size_t offset) const;
    Table tableNamed(const std::string& tag) const;
    void chooseCharacterMap();
    /// The glyph's contours in font units, with composites resolved to `depth` levels.
    std::vector<Contour> contoursOf(int glyph, int depth) const;
    /// The byte range of `glyph` in `glyf`: offset and length (0 for an empty glyph).
    std::pair<uint32_t, uint32_t> glyphRange(int glyph) const;
    /// The outline of a TrueType glyph (see `outline`).
    GlyphOutline trueTypeOutline(int glyph) const;
    /// The outline of a CFF glyph (see `outline`).
    GlyphOutline compactOutline(int glyph) const;
};

} // namespace editor
