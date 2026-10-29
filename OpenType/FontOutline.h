// What reading an OpenType font gives back: glyph outlines in the font's own units, and the one kind
// of error every font reader here throws. Standard library only; the app's drawing types stay on its
// own side of the line, and whoever calls turns these into whatever it draws with.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace editor {

/// A damaged or unsupported font: a table that runs past the end of the file, a charstring that breaks
/// the Type 2 limits, an outline format that is not read. The message is a readable sentence.
///
/// Use when: catching what `OpenTypeFont::parsed` and `OpenTypeFont::outline` throw
/// Tags: font error, font problem, damaged font, exception
/// Node: none (file format)
class FontProblem : public std::runtime_error {
public:
    explicit FontProblem(const std::string& message) : std::runtime_error(message) {}
};

/// A point of a glyph in font units: x to the right, y up, the baseline at y = 0, as the font stores it.
///
/// Use when: reading a glyph outline
/// Tags: font point, glyph point, font units
/// Node: none (file format)
struct FontPoint {
    float x = 0;
    float y = 0;

    bool operator==(const FontPoint& other) const = default;
};

/// One piece of a glyph contour after the pen has moved: a straight line, a quadratic Bézier
/// (TrueType glyphs) or a cubic Bézier (CFF glyphs). It starts where the piece before it ended.
///
/// Conventions: a line uses only `end`; a quadratic has its one control point in `control1`; a cubic
/// has `control1` (the handle of its start) and `control2` (the handle of its end).
///
/// Use when: walking the pieces of a glyph contour
/// Tags: glyph segment, line, quadratic, cubic, outline piece
/// Node: none (file format)
struct GlyphSegment {
    enum class Kind { line, quadratic, cubic };

    Kind kind = Kind::line;
    FontPoint control1;
    FontPoint control2;
    FontPoint end;
};

/// A closed contour of a glyph: where it starts, then the pieces drawn from there. A contour always
/// closes: when the last piece stops short of `start`, the straight line back is implied.
///
/// Use when: turning a glyph into strokes or paths
/// Tags: glyph contour, subpath, closed contour
/// Node: none (file format)
struct GlyphContour {
    FontPoint start;
    std::vector<GlyphSegment> segments;
};

/// The outline of one glyph: its closed contours, in font units, as `OpenTypeFont::outline` returns
/// them. Fill the contours together with the nonzero rule. A space, or a glyph that is not in the
/// font, has no contours.
///
/// Use when: drawing a glyph; turning text into outlines
/// Tags: glyph outline, glyph contours, font outline
/// Node: none (file format)
struct GlyphOutline {
    std::vector<GlyphContour> contours;
};

} // namespace editor
