// The TrueType reader (`glyf` outlines, `cmap`, `hmtx`, `kern`) on the tiny font TestFontBuilder.h
// builds in memory. Every expected number is worked out by hand from that font's glyph data, in font
// units: the outlines come out unscaled, with quadratic pieces kept as quadratics.
#include <cmath>

#include "../OpenType/OpenTypeFont.h"
#include "TestFontBuilder.h"
#include "Testing.h"

using namespace editor;
using namespace editor::testing;

namespace {

bool near(float actual, float expected, float tolerance = 1e-3f) { return std::fabs(actual - expected) <= tolerance; }
bool near(FontPoint actual, FontPoint expected, float tolerance = 1e-3f) {
    return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance);
}

/// True when `segment` is a line to `end`.
bool isLine(const GlyphSegment& segment, FontPoint end) { return segment.kind == GlyphSegment::Kind::line && near(segment.end, end); }

/// True when `segment` is the quadratic with this control point and end.
bool isQuadratic(const GlyphSegment& segment, FontPoint control, FontPoint end) {
    return segment.kind == GlyphSegment::Kind::quadratic && near(segment.control1, control) && near(segment.end, end);
}

} // namespace

TEST(openTypeReadsTheHeaderMetrics) {
    const OpenTypeFont font = OpenTypeFont::parsed(minimalTestFont());
    EXPECT_EQUAL(font.unitsPerEm(), 1000);
    EXPECT_EQUAL(font.glyphCount(), 6);
    // Ascender 800, descender -200, gap 100 font units.
    EXPECT_EQUAL(font.ascender(), 800);
    EXPECT_EQUAL(font.descender(), -200);
    EXPECT_EQUAL(font.lineGap(), 100);
}

TEST(openTypeMapsCharactersWithFormat4AndFormat12) {
    const OpenTypeFont format4 = OpenTypeFont::parsed(minimalTestFont());
    EXPECT_EQUAL(format4.glyphIndex('A'), 1);
    EXPECT_EQUAL(format4.glyphIndex('B'), 2);
    EXPECT_EQUAL(format4.glyphIndex('C'), 3);
    EXPECT_EQUAL(format4.glyphIndex(' '), 4);
    EXPECT_EQUAL(format4.glyphIndex('O'), 5);
    // Missing characters, inside a gap and past the last segment, give the missing glyph 0.
    EXPECT_EQUAL(format4.glyphIndex('Z'), 0);
    EXPECT_EQUAL(format4.glyphIndex('a'), 0);
    EXPECT_EQUAL(format4.glyphIndex(0x1F600), 0);

    TestFontOptions options;
    options.format12 = true;
    const OpenTypeFont format12 = OpenTypeFont::parsed(minimalTestFont(options));
    EXPECT_EQUAL(format12.glyphIndex('A'), 1);
    EXPECT_EQUAL(format12.glyphIndex('C'), 3);
    EXPECT_EQUAL(format12.glyphIndex('O'), 5);
    EXPECT_EQUAL(format12.glyphIndex('Z'), 0);
    EXPECT_EQUAL(format12.glyphIndex(0x1F600), 1);
}

TEST(openTypeAdvancesAndKerning) {
    const OpenTypeFont font = OpenTypeFont::parsed(minimalTestFont());
    // Advances 600 and 300 font units; a glyph that is not there advances 0.
    EXPECT_EQUAL(font.advance(1), 600);
    EXPECT_EQUAL(font.advance(4), 300);
    EXPECT_EQUAL(font.advance(99), 0);
    // The pair (A, B) is -50 units; the reverse pair and (A, C) have none.
    EXPECT_EQUAL(font.kerning(1, 2), -50);
    EXPECT_EQUAL(font.kerning(2, 1), 0);
    EXPECT_EQUAL(font.kerning(1, 3), 0);
}

TEST(openTypeOutlineOfASquareGlyph) {
    const OpenTypeFont font = OpenTypeFont::parsed(minimalTestFont());
    const GlyphOutline outline = font.outline(1);
    EXPECT_EQUAL(outline.contours.size(), size_t(1));
    // The corners (0,0) (500,0) (500,700) (0,700), and the line back to the start, which a TrueType contour lists.
    const GlyphContour& contour = outline.contours[0];
    EXPECT(near(contour.start, FontPoint{0, 0}));
    EXPECT_EQUAL(contour.segments.size(), size_t(4));
    EXPECT(isLine(contour.segments[0], FontPoint{500, 0}));
    EXPECT(isLine(contour.segments[1], FontPoint{500, 700}));
    EXPECT(isLine(contour.segments[2], FontPoint{0, 700}));
    EXPECT(isLine(contour.segments[3], FontPoint{0, 0}));
}

TEST(openTypeQuadraticsStayQuadratic) {
    const OpenTypeFont font = OpenTypeFont::parsed(minimalTestFont());
    const GlyphOutline outline = font.outline(2);
    EXPECT_EQUAL(outline.contours.size(), size_t(1));
    // (0,0) on, (250,500) off, (500,0) on: a quadratic arch with control (250,500), closed by a line back.
    const GlyphContour& contour = outline.contours[0];
    EXPECT(near(contour.start, FontPoint{0, 0}));
    EXPECT_EQUAL(contour.segments.size(), size_t(2));
    EXPECT(isQuadratic(contour.segments[0], FontPoint{250, 500}, FontPoint{500, 0}));
    EXPECT(isLine(contour.segments[1], FontPoint{0, 0}));
}

TEST(openTypeImpliesOnCurvePointsBetweenOffCurvePoints) {
    const OpenTypeFont font = OpenTypeFont::parsed(minimalTestFont());
    const GlyphOutline outline = font.outline(5);
    EXPECT_EQUAL(outline.contours.size(), size_t(1));
    // Four off-curve corners (0,0) (500,0) (500,500) (0,500): the contour starts at the midpoint of the
    // last and first (0,250); the implied on-curve points are the midpoints of neighbours, and each piece
    // is a quadratic whose control is the corner between them.
    const GlyphContour& contour = outline.contours[0];
    EXPECT(near(contour.start, FontPoint{0, 250}));
    EXPECT_EQUAL(contour.segments.size(), size_t(4));
    EXPECT(isQuadratic(contour.segments[0], FontPoint{0, 0}, FontPoint{250, 0}));
    EXPECT(isQuadratic(contour.segments[1], FontPoint{500, 0}, FontPoint{500, 250}));
    EXPECT(isQuadratic(contour.segments[2], FontPoint{500, 500}, FontPoint{250, 500}));
    EXPECT(isQuadratic(contour.segments[3], FontPoint{0, 500}, FontPoint{0, 250}));
}

TEST(openTypeCompositeGlyphsMoveAndScaleTheirParts) {
    const OpenTypeFont font = OpenTypeFont::parsed(minimalTestFont());
    const GlyphOutline outline = font.outline(3);
    EXPECT_EQUAL(outline.contours.size(), size_t(2));
    // Part one: the square scaled by 0.5 then moved by (100, 50): corners (100,50) (350,50) (350,400) (100,400).
    const GlyphContour& square = outline.contours[0];
    EXPECT(near(square.start, FontPoint{100, 50}));
    EXPECT_EQUAL(square.segments.size(), size_t(4));
    EXPECT(isLine(square.segments[0], FontPoint{350, 50}));
    EXPECT(isLine(square.segments[1], FontPoint{350, 400}));
    EXPECT(isLine(square.segments[2], FontPoint{100, 400}));
    EXPECT(isLine(square.segments[3], FontPoint{100, 50}));
    // Part two: glyph 2 unmoved.
    const GlyphContour& arch = outline.contours[1];
    EXPECT_EQUAL(arch.segments.size(), size_t(2));
    EXPECT(isQuadratic(arch.segments[0], FontPoint{250, 500}, FontPoint{500, 0}));
}

TEST(openTypeReadsLongLocaAndEmptyGlyphs) {
    TestFontOptions options;
    options.longLoca = true;
    const OpenTypeFont font = OpenTypeFont::parsed(minimalTestFont(options));
    EXPECT_EQUAL(font.outline(1).contours.size(), size_t(1));
    EXPECT_EQUAL(font.outline(3).contours.size(), size_t(2));
    // A space, the missing glyph and a glyph number out of range have no outline.
    EXPECT(font.outline(4).contours.empty());
    EXPECT(font.outline(0).contours.empty());
    EXPECT(font.outline(99).contours.empty());
}

TEST(openTypeRefusesDamagedFiles) {
    EXPECT_PROBLEM(OpenTypeFont::parsed({}), "too short");
    // A TrueType font relabelled "OTTO" has no CFF table: refused, naming it (the CFF reader's own tests are in OpenTypeCFFTests.cpp).
    std::vector<uint8_t> wrongKind = minimalTestFont();
    wrongKind[0] = 'O';
    wrongKind[1] = 'T';
    wrongKind[2] = 'T';
    wrongKind[3] = 'O';
    EXPECT_PROBLEM(OpenTypeFont::parsed(wrongKind), "no CFF table");
    std::vector<uint8_t> notAFont = minimalTestFont();
    notAFont[0] = 0x12;
    EXPECT_PROBLEM(OpenTypeFont::parsed(notAFont), "not a TrueType");
    // A file cut short in the middle of its tables.
    std::vector<uint8_t> cut = minimalTestFont();
    cut.resize(60);
    EXPECT_PROBLEM(OpenTypeFont::parsed(cut), "font");
}
