// The CFF reader (OpenType fonts with Type 2 charstrings) and `OpenTypeFont` on such fonts, on the tiny
// fonts TestFontBuilder.h builds in memory. Every expected number is worked out by hand from the
// charstrings in that file: a path operator's arguments are deltas from the pen, so each test lists the
// points the pen visits.
#include <cmath>

#include "../OpenType/CompactFontFormat.h"
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

/// True when `segment` is the cubic with these control points and end.
bool isCurve(const GlyphSegment& segment, FontPoint control1, FontPoint control2, FontPoint end) {
    return segment.kind == GlyphSegment::Kind::cubic && near(segment.control1, control1) && near(segment.control2, control2) && near(segment.end, end);
}

/// True when `segment` is a straight line to `end`.
bool isLine(const GlyphSegment& segment, FontPoint end) { return segment.kind == GlyphSegment::Kind::line && near(segment.end, end); }

/// A font whose glyph 0 is `.notdef` and whose other glyphs are `charstrings`, read straight from its CFF table.
CompactFontFormat fontOfCharstrings(const std::vector<TestCharstring>& charstrings) {
    CFFTestFontSpec spec;
    spec.charstrings.push_back(TestCharstring().op(cs::endchar).bytes);
    for (const TestCharstring& charstring : charstrings) spec.charstrings.push_back(charstring.bytes);
    return CompactFontFormat::parse(cffTable(spec).data);
}

/// The single contour glyph `glyph` of `font` draws.
GlyphContour onlyContour(const CompactFontFormat& font, int glyph) {
    const CompactGlyph drawn = font.glyph(glyph);
    EXPECT_EQUAL(drawn.outline.contours.size(), size_t(1));
    return drawn.outline.contours.at(0);
}

/// Where a contour starts and where each of its pieces ends: the anchors the pen visits.
std::vector<FontPoint> anchorsOf(const GlyphContour& contour) {
    std::vector<FontPoint> anchors = {contour.start};
    for (const GlyphSegment& segment : contour.segments) anchors.push_back(segment.end);
    return anchors;
}

} // namespace

TEST(cffFontKeepsTheSharedTablesWorking) {
    const OpenTypeFont font = OpenTypeFont::parsed(minimalCFFTestFont());
    EXPECT_EQUAL(font.unitsPerEm(), 1000);
    EXPECT_EQUAL(font.glyphCount(), 5);
    EXPECT_EQUAL(font.ascender(), 800);
    EXPECT_EQUAL(font.descender(), -200);
    EXPECT_EQUAL(font.lineGap(), 100);
    // cmap: 'A' ... 'D' are glyphs 1 ... 4; anything else is the missing glyph.
    EXPECT_EQUAL(font.glyphIndex('A'), 1);
    EXPECT_EQUAL(font.glyphIndex('B'), 2);
    EXPECT_EQUAL(font.glyphIndex('C'), 3);
    EXPECT_EQUAL(font.glyphIndex('D'), 4);
    EXPECT_EQUAL(font.glyphIndex('E'), 0);
    EXPECT_EQUAL(font.glyphIndex('a'), 0);
    // hmtx advances 600 and 400 font units; a glyph that is not there advances 0; no kern table, no kerning.
    EXPECT_EQUAL(font.advance(1), 600);
    EXPECT_EQUAL(font.advance(3), 400);
    EXPECT_EQUAL(font.advance(99), 0);
    EXPECT_EQUAL(font.kerning(1, 2), 0);
}

TEST(cffSquareFromRmovetoAndAlternatingLines) {
    const OpenTypeFont font = OpenTypeFont::parsed(minimalCFFTestFont());
    // `100 0 0 rmoveto 500 700 -500 hlineto`: the pen goes (0,0) -> (500,0) -> (500,700) -> (0,700); the contour closes by itself.
    const GlyphOutline outline = font.outline(1);
    EXPECT_EQUAL(outline.contours.size(), size_t(1));
    // Three lines: the way back to (0,0) is implied by the closed contour.
    const GlyphContour& contour = outline.contours[0];
    EXPECT_EQUAL(contour.segments.size(), size_t(3));
    const FontPoint corners[4] = {FontPoint{0, 0}, FontPoint{500, 0}, FontPoint{500, 700}, FontPoint{0, 700}};
    const std::vector<FontPoint> anchors = anchorsOf(contour);
    EXPECT_EQUAL(anchors.size(), size_t(4));
    for (size_t index = 0; index < 4; index += 1) EXPECT(near(anchors[index], corners[index]));
    for (const GlyphSegment& segment : contour.segments) EXPECT(segment.kind == GlyphSegment::Kind::line);
    // The same square as the TrueType test font draws: the two kinds of font meet in the same corners
    // (the TrueType contour also lists the line back to its start).
    const OpenTypeFont trueType = OpenTypeFont::parsed(minimalTestFont());
    const std::vector<FontPoint> trueTypeAnchors = anchorsOf(trueType.outline(1).contours.at(0));
    EXPECT_EQUAL(trueTypeAnchors.size(), size_t(5));
    for (size_t index = 0; index < 4; index += 1) EXPECT(near(trueTypeAnchors[index], anchors[index]));
}

TEST(cffCurvesKeepTheirCubicControlPoints) {
    const OpenTypeFont font = OpenTypeFont::parsed(minimalCFFTestFont());
    const GlyphOutline outline = font.outline(2);
    EXPECT_EQUAL(outline.contours.size(), size_t(1));
    const GlyphContour& contour = outline.contours[0];
    // Start (100,0), then
    //   rrcurveto 50 0 50 50 0 100:      c1 (150,0)   c2 (200,50)  end (200,150)
    //   vhcurveto 30 -20 20 -30 ...:     c1 (200,180) c2 (180,200) end (150,200)   (vertical first)
    //             ... -40 -20 -20 -30:   c1 (110,200) c2 (90,180)  end (90,150)    (then horizontal first)
    //   hvcurveto -30 -20 -20 -40 10:    c1 (60,150)  c2 (40,130)  end (50,90)     (the fifth argument moves the end in x)
    EXPECT(near(contour.start, FontPoint{100, 0}));
    EXPECT_EQUAL(contour.segments.size(), size_t(4));
    EXPECT(isCurve(contour.segments[0], FontPoint{150, 0}, FontPoint{200, 50}, FontPoint{200, 150}));
    EXPECT(isCurve(contour.segments[1], FontPoint{200, 180}, FontPoint{180, 200}, FontPoint{150, 200}));
    EXPECT(isCurve(contour.segments[2], FontPoint{110, 200}, FontPoint{90, 180}, FontPoint{90, 150}));
    EXPECT(isCurve(contour.segments[3], FontPoint{60, 150}, FontPoint{40, 130}, FontPoint{50, 90}));
    // The piece back to the start is not written: the contour closes itself.
}

TEST(cffSubroutinesHintMasksAndWidths) {
    const CompactFontFormat cff = CompactFontFormat::parse(cffTable(minimalCFFTestSpec()).data);
    EXPECT_EQUAL(cff.glyphCount(), 5);
    EXPECT(!cff.isCIDKeyed());
    // Widths: nominalWidthX 500 plus the first argument, or defaultWidthX 500 when there is none.
    EXPECT(near(cff.glyph(0).width, 500));
    EXPECT(near(cff.glyph(1).width, 600));
    EXPECT(near(cff.glyph(2).width, 500));
    EXPECT(near(cff.glyph(3).width, 400));
    EXPECT(near(cff.glyph(4).width, 300));

    // Glyph 3: stems 5 + 3 + 1 (the two numbers before the hintmask) = 9, so the mask is two bytes: if only
    // one were skipped the second (`endchar`) would end the glyph with nothing drawn. Then `20 30 rmoveto`, a
    // local subroutine `hlineto` fed 100 from the caller's stack, a global `vlineto` fed 80, and `-100 hlineto`.
    const OpenTypeFont font = OpenTypeFont::parsed(minimalCFFTestFont());
    const GlyphOutline outline = font.outline(3);
    EXPECT_EQUAL(outline.contours.size(), size_t(1));
    const FontPoint expected[4] = {FontPoint{20, 30}, FontPoint{120, 30}, FontPoint{120, 110}, FontPoint{20, 110}};
    const std::vector<FontPoint> anchors = anchorsOf(outline.contours[0]);
    EXPECT_EQUAL(anchors.size(), size_t(4));
    for (size_t index = 0; index < 4; index += 1) EXPECT(near(anchors[index], expected[index]));

    // A space (width only), the missing glyph and a glyph out of range have no outline.
    EXPECT(font.outline(4).contours.empty());
    EXPECT(font.outline(0).contours.empty());
    EXPECT(font.outline(99).contours.empty());
    EXPECT(font.outline(-1).contours.empty());
}

TEST(cffSubroutineNumbersAreBiasedByTheIndexSize) {
    // 1240 global subroutines add 1131 to the operand; 33900 local ones add 32768. Only the last of each draws:
    // operand 108 reaches global subroutine 1239, operand 1131 reaches local subroutine 33899.
    CFFTestFontSpec spec;
    spec.charstrings.push_back(TestCharstring().op(cs::endchar).bytes);
    spec.charstrings.push_back(TestCharstring()
                                   .args({0, 0})
                                   .op(cs::rmoveto)
                                   .args({30, 1131})
                                   .op(cs::callsubr)
                                   .args({40, 108})
                                   .op(cs::callgsubr)
                                   .op(cs::endchar)
                                   .bytes);
    const TestBytes doNothing = TestCharstring().op(cs::returnFromSubroutine).bytes;
    spec.globalSubroutines.assign(1239, doNothing);
    spec.globalSubroutines.push_back(TestCharstring().op(cs::vlineto).op(cs::returnFromSubroutine).bytes);
    spec.privateData.localSubroutines.assign(33899, doNothing);
    spec.privateData.localSubroutines.push_back(TestCharstring().op(cs::hlineto).op(cs::returnFromSubroutine).bytes);
    const CompactFontFormat font = CompactFontFormat::parse(cffTable(spec).data);
    // (0,0) -> hlineto 30 -> (30,0) -> vlineto 40 -> (30,40).
    const GlyphContour contour = onlyContour(font, 1);
    EXPECT(near(contour.start, FontPoint{0, 0}));
    EXPECT_EQUAL(contour.segments.size(), size_t(2));
    EXPECT(isLine(contour.segments[0], FontPoint{30, 0}));
    EXPECT(isLine(contour.segments[1], FontPoint{30, 40}));
}

TEST(cffLineAndCurveOperators) {
    const CompactFontFormat font = fontOfCharstrings({
        // 1: vlineto 10 20 30 starts vertical: (0,0) (0,10) (20,10) (20,40).
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({10, 20, 30}).op(cs::vlineto).op(cs::endchar),
        // 2: rcurveline: a curve of six, then a line of two.
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({10, 0, 10, 10, 0, 10, 5, 5}).op(cs::rcurveline).op(cs::endchar),
        // 3: rlinecurve: two lines of two, then a curve of six.
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({5, 0, 0, 5, 10, 0, 10, 10, 0, 10}).op(cs::rlinecurve).op(cs::endchar),
        // 4: hhcurveto with the leading dy1, then a second glyph's worth of groups without it.
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({7, 10, 20, 30, 40}).op(cs::hhcurveto).op(cs::endchar),
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({10, 20, 30, 40, 5, 6, 7, 8}).op(cs::hhcurveto).op(cs::endchar),
        // 6: vvcurveto with the leading dx1.
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({3, 10, 20, 30, 40}).op(cs::vvcurveto).op(cs::endchar),
        // 7: the other alternation: vhcurveto with four arguments, then hvcurveto with eight.
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({10, 20, 30, 40}).op(cs::vhcurveto).op(cs::endchar),
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({10, 20, 30, 40, 1, 2, 3, 4}).op(cs::hvcurveto).op(cs::endchar),
    });

    GlyphContour contour = onlyContour(font, 1);
    EXPECT_EQUAL(contour.segments.size(), size_t(3));
    EXPECT(isLine(contour.segments[0], FontPoint{0, 10}));
    EXPECT(isLine(contour.segments[1], FontPoint{20, 10}));
    EXPECT(isLine(contour.segments[2], FontPoint{20, 40}));

    // c1 (10,0), c2 (20,10), end (20,20); the line goes on to (25,25).
    contour = onlyContour(font, 2);
    EXPECT_EQUAL(contour.segments.size(), size_t(2));
    EXPECT(isCurve(contour.segments[0], FontPoint{10, 0}, FontPoint{20, 10}, FontPoint{20, 20}));
    EXPECT(isLine(contour.segments[1], FontPoint{25, 25}));

    // Lines to (5,0) and (5,5); the curve has c1 (15,5), c2 (25,15), end (25,25).
    contour = onlyContour(font, 3);
    EXPECT_EQUAL(contour.segments.size(), size_t(3));
    EXPECT(isLine(contour.segments[0], FontPoint{5, 0}));
    EXPECT(isLine(contour.segments[1], FontPoint{5, 5}));
    EXPECT(isCurve(contour.segments[2], FontPoint{15, 5}, FontPoint{25, 15}, FontPoint{25, 25}));

    // dy1 = 7: c1 (10,7), c2 (30,37), end (70,37).
    contour = onlyContour(font, 4);
    EXPECT_EQUAL(contour.segments.size(), size_t(1));
    EXPECT(isCurve(contour.segments[0], FontPoint{10, 7}, FontPoint{30, 37}, FontPoint{70, 37}));

    // No dy1: (10,0) (30,30) (70,30), then (75,30) (81,37) (89,37).
    contour = onlyContour(font, 5);
    EXPECT_EQUAL(contour.segments.size(), size_t(2));
    EXPECT(isCurve(contour.segments[0], FontPoint{10, 0}, FontPoint{30, 30}, FontPoint{70, 30}));
    EXPECT(isCurve(contour.segments[1], FontPoint{75, 30}, FontPoint{81, 37}, FontPoint{89, 37}));

    // dx1 = 3: c1 (3,10), c2 (23,40), end (23,80).
    contour = onlyContour(font, 6);
    EXPECT_EQUAL(contour.segments.size(), size_t(1));
    EXPECT(isCurve(contour.segments[0], FontPoint{3, 10}, FontPoint{23, 40}, FontPoint{23, 80}));

    // vhcurveto 10 20 30 40: c1 (0,10), c2 (20,40), end (60,40).
    contour = onlyContour(font, 7);
    EXPECT_EQUAL(contour.segments.size(), size_t(1));
    EXPECT(isCurve(contour.segments[0], FontPoint{0, 10}, FontPoint{20, 40}, FontPoint{60, 40}));

    // hvcurveto 10 20 30 40 | 1 2 3 4: c1 (10,0), c2 (30,30), end (30,70); then, vertical first, c1 (30,71), c2 (32,74), end (36,74).
    contour = onlyContour(font, 8);
    EXPECT_EQUAL(contour.segments.size(), size_t(2));
    EXPECT(isCurve(contour.segments[0], FontPoint{10, 0}, FontPoint{30, 30}, FontPoint{30, 70}));
    EXPECT(isCurve(contour.segments[1], FontPoint{30, 71}, FontPoint{32, 74}, FontPoint{36, 74}));
}

TEST(cffFlexOperators) {
    const CompactFontFormat font = fontOfCharstrings({
        // hflex 10 20 30 40 50 60 70.
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({10, 20, 30, 40, 50, 60, 70}).escaped(cs::hflex).op(cs::endchar),
        // flex: two plain curves and the flex depth 50, which is ignored.
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 50}).escaped(cs::flex).op(cs::endchar),
        // hflex1 10 5 20 15 30 40 50 -10 60.
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({10, 5, 20, 15, 30, 40, 50, -10, 60}).escaped(cs::hflex1).op(cs::endchar),
        // flex1 with the x sum (150) longer than the y sum (25): d6 is dx6.
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({10, 5, 20, 5, 30, 5, 40, 5, 50, 5, 60}).escaped(cs::flex1).op(cs::endchar),
        // flex1 with the y sum (50) longer than the x sum (15): d6 is dy6.
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({1, 10, 2, 10, 3, 10, 4, 10, 5, 10, 7}).escaped(cs::flex1).op(cs::endchar),
    });

    // hflex: c1 (10,0) c2 (30,30) end (70,30); then c1 (120,30) c2 (180,0) end (250,0): the second curve comes back to y = 0.
    GlyphContour contour = onlyContour(font, 1);
    EXPECT_EQUAL(contour.segments.size(), size_t(2));
    EXPECT(isCurve(contour.segments[0], FontPoint{10, 0}, FontPoint{30, 30}, FontPoint{70, 30}));
    EXPECT(isCurve(contour.segments[1], FontPoint{120, 30}, FontPoint{180, 0}, FontPoint{250, 0}));

    // flex: (1,2) (4,6) (9,12); then (16,20) (25,30) (36,42).
    contour = onlyContour(font, 2);
    EXPECT_EQUAL(contour.segments.size(), size_t(2));
    EXPECT(isCurve(contour.segments[0], FontPoint{1, 2}, FontPoint{4, 6}, FontPoint{9, 12}));
    EXPECT(isCurve(contour.segments[1], FontPoint{16, 20}, FontPoint{25, 30}, FontPoint{36, 42}));

    // hflex1: (10,5) (30,20) (60,20); then (100,20) (150,10) (210,0): the last point is back on y = 0 (-(5 + 15 - 10) = -10).
    contour = onlyContour(font, 3);
    EXPECT(isCurve(contour.segments[0], FontPoint{10, 5}, FontPoint{30, 20}, FontPoint{60, 20}));
    EXPECT(isCurve(contour.segments[1], FontPoint{100, 20}, FontPoint{150, 10}, FontPoint{210, 0}));

    // flex1, x longer: (10,5) (30,10) (60,15); then (100,20) (150,25) and dx6 = 60, dy6 = -25: end (210,0).
    contour = onlyContour(font, 4);
    EXPECT(isCurve(contour.segments[0], FontPoint{10, 5}, FontPoint{30, 10}, FontPoint{60, 15}));
    EXPECT(isCurve(contour.segments[1], FontPoint{100, 20}, FontPoint{150, 25}, FontPoint{210, 0}));

    // flex1, y longer: (1,10) (3,20) (6,30); then (10,40) (15,50) and dy6 = 7, dx6 = -15: end (0,57).
    contour = onlyContour(font, 5);
    EXPECT(isCurve(contour.segments[0], FontPoint{1, 10}, FontPoint{3, 20}, FontPoint{6, 30}));
    EXPECT(isCurve(contour.segments[1], FontPoint{10, 40}, FontPoint{15, 50}, FontPoint{0, 57}));
}

TEST(cffNumberEncodingsAndContours) {
    const CompactFontFormat font = fontOfCharstrings({
        // Every operand form: 32-246 (small), 247-254 (±108 to ±1131), 28 (a 16-bit integer) and 255 (16.16 fixed).
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({2000, -1500, 1131, -1131, 108, -108}).op(cs::rlineto).op(cs::endchar),
        TestCharstring().args({0, 0}).op(cs::rmoveto).fixed(2.5f).n(0).n(10).fixed(-1.25f).op(cs::rlineto).op(cs::endchar),
        // Two contours: a move closes the one before it, and a move with nothing drawn since makes none.
        TestCharstring()
            .args({0, 0})
            .op(cs::rmoveto)
            .args({10, 10})
            .op(cs::rlineto)
            .args({100, 0})
            .op(cs::rmoveto)
            .args({50, 50})
            .op(cs::rmoveto)
            .args({5, 5})
            .op(cs::rlineto)
            .op(cs::endchar),
        // hmoveto and vmoveto, each with a width argument in front on the first.
        TestCharstring().args({7, 20}).op(cs::hmoveto).args({30}).op(cs::vmoveto).args({4, 4}).op(cs::rlineto).op(cs::endchar),
    });
    GlyphContour contour = onlyContour(font, 1);
    EXPECT_EQUAL(contour.segments.size(), size_t(3));
    EXPECT(isLine(contour.segments[0], FontPoint{2000, -1500}));
    EXPECT(isLine(contour.segments[1], FontPoint{3131, -2631}));
    EXPECT(isLine(contour.segments[2], FontPoint{3239, -2739}));

    contour = onlyContour(font, 2);
    EXPECT_EQUAL(contour.segments.size(), size_t(2));
    EXPECT(isLine(contour.segments[0], FontPoint{2.5f, 0}));
    EXPECT(isLine(contour.segments[1], FontPoint{12.5f, -1.25f}));

    // First contour (0,0) -> (10,10); the move to (110,10) draws nothing and is replaced by the move to (160,60); then (165,65).
    const CompactGlyph twoContours = font.glyph(3);
    EXPECT_EQUAL(twoContours.outline.contours.size(), size_t(2));
    EXPECT(near(twoContours.outline.contours[0].start, FontPoint{0, 0}));
    EXPECT(isLine(twoContours.outline.contours[0].segments[0], FontPoint{10, 10}));
    EXPECT(near(twoContours.outline.contours[1].start, FontPoint{160, 60}));
    EXPECT(isLine(twoContours.outline.contours[1].segments[0], FontPoint{165, 65}));

    // Width 500 (nominalWidthX) + 7, then hmoveto 20 and vmoveto 30 put the pen at (20,30), and the line goes on to (24,34).
    const CompactGlyph moved = font.glyph(4);
    EXPECT(near(moved.width, 507));
    EXPECT(near(moved.outline.contours.at(0).start, FontPoint{20, 30}));
    EXPECT(isLine(moved.outline.contours.at(0).segments.at(0), FontPoint{24, 34}));
}

TEST(cffCIDKeyedFontsChooseTheirDictionaryPerGlyph) {
    for (bool selectFormat0 : {false, true}) {
        CFFTestFontSpec spec;
        spec.selectFormat0 = selectFormat0;
        CFFTestFontDictionary first;
        first.defaultWidthX = 300;
        first.nominalWidthX = 400;
        first.localSubroutines.push_back(TestCharstring().op(cs::hlineto).op(cs::returnFromSubroutine).bytes);
        CFFTestFontDictionary second;
        second.defaultWidthX = 700;
        second.nominalWidthX = 100;
        second.localSubroutines.push_back(TestCharstring().op(cs::vlineto).op(cs::returnFromSubroutine).bytes);
        spec.fontDictionaries = {first, second};
        // Glyphs 0, 1 and 4 use the first dictionary, 2 and 3 the second (format 3: ranges 0, 2 and 4).
        spec.dictionaryOfGlyph = {0, 0, 1, 1, 0};
        spec.charstrings.push_back(TestCharstring().op(cs::endchar).bytes);
        // The same `50 callsubr` reaches a different subroutine in each dictionary.
        spec.charstrings.push_back(TestCharstring().args({0, 0}).op(cs::rmoveto).args({50, -107}).op(cs::callsubr).args({60}).op(cs::vlineto).op(cs::endchar).bytes);
        spec.charstrings.push_back(TestCharstring().args({0, 0}).op(cs::rmoveto).args({50, -107}).op(cs::callsubr).args({60}).op(cs::hlineto).op(cs::endchar).bytes);
        spec.charstrings.push_back(TestCharstring().args({25, 0, 0}).op(cs::rmoveto).args({10}).op(cs::hlineto).op(cs::endchar).bytes);
        spec.charstrings.push_back(TestCharstring().args({5, 0, 0}).op(cs::rmoveto).args({10}).op(cs::hlineto).op(cs::endchar).bytes);

        const CompactFontFormat font = CompactFontFormat::parse(cffTable(spec).data);
        EXPECT(font.isCIDKeyed());
        EXPECT_EQUAL(font.glyphCount(), 5);
        // Glyph 1 (first dictionary): hlineto 50 -> (50,0), then vlineto 60 -> (50,60); no width, so defaultWidthX 300.
        GlyphContour contour = onlyContour(font, 1);
        EXPECT(isLine(contour.segments.at(0), FontPoint{50, 0}));
        EXPECT(isLine(contour.segments.at(1), FontPoint{50, 60}));
        EXPECT(near(font.glyph(1).width, 300));
        // Glyph 2 (second dictionary): its subroutine is vlineto: (0,50), then hlineto 60 -> (60,50); defaultWidthX 700.
        contour = onlyContour(font, 2);
        EXPECT(isLine(contour.segments.at(0), FontPoint{0, 50}));
        EXPECT(isLine(contour.segments.at(1), FontPoint{60, 50}));
        EXPECT(near(font.glyph(2).width, 700));
        // Widths from the first argument: 100 + 25 in the second dictionary, 400 + 5 in the first.
        EXPECT(near(font.glyph(3).width, 125));
        EXPECT(near(font.glyph(4).width, 405));
    }
}

TEST(cffFontMatrixScalesTheOutline) {
    // The default matrix written out, with an exponent real (1E-3 as packed nibbles 1, E-, 3), changes nothing.
    CFFTestFontSpec spec = minimalCFFTestSpec();
    spec.fontMatrix = {"1n3", "0", "0", "0.001", "0", "0"};
    const CompactFontFormat defaulted = CompactFontFormat::parse(cffTable(spec).data);
    EXPECT(defaulted.usesDefaultFontMatrix(1));
    EXPECT(near(defaulted.fontMatrix(1)[0], 0.001f, 1e-7f));
    EXPECT(near(anchorsOf(OpenTypeFont::parsed(cffTestFont(spec)).outline(1).contours.at(0))[3], FontPoint{0, 700}));

    // A matrix of 0.002 means 500 units to the em: with 1000 font units to the em the square comes out twice as large.
    spec.fontMatrix = {"0.002", "0", "0", "0.002", "0", "0"};
    const OpenTypeFont doubled = OpenTypeFont::parsed(cffTestFont(spec));
    EXPECT(!CompactFontFormat::parse(cffTable(spec).data).usesDefaultFontMatrix(1));
    const GlyphOutline outline = doubled.outline(1);
    EXPECT_EQUAL(outline.contours.size(), size_t(1));
    const std::vector<FontPoint> anchors = anchorsOf(outline.contours[0]);
    EXPECT(near(anchors[1], FontPoint{1000, 0}));
    EXPECT(near(anchors[2], FontPoint{1000, 1400}));
    // Metrics still come from head and hmtx, not the matrix.
    EXPECT_EQUAL(doubled.advance(1), 600);

    // A CID font whose second dictionary has its own matrix: only its glyphs are scaled.
    CFFTestFontSpec cid;
    CFFTestFontDictionary plain;
    CFFTestFontDictionary scaled;
    scaled.fontMatrix = {"0.002", "0", "0", "0.002", "0", "0"};
    cid.fontDictionaries = {plain, scaled};
    cid.dictionaryOfGlyph = {0, 0, 1};
    cid.charstrings.push_back(TestCharstring().op(cs::endchar).bytes);
    const TestBytes square = TestCharstring().args({0, 0}).op(cs::rmoveto).args({100, 100, -100}).op(cs::hlineto).op(cs::endchar).bytes;
    cid.charstrings.push_back(square);
    cid.charstrings.push_back(square);
    const OpenTypeFont cidFont = OpenTypeFont::parsed(cffTestFont(cid));
    EXPECT(near(anchorsOf(cidFont.outline(1).contours.at(0))[2], FontPoint{100, 100}));
    EXPECT(near(anchorsOf(cidFont.outline(2).contours.at(0))[2], FontPoint{200, 200}));
}

TEST(cffRefusesWhatItCannotRead) {
    // CFF2 only: no `CFF ` table, a `CFF2` one.
    CFFTestFontSpec variable = minimalCFFTestSpec();
    variable.tableTag = "CFF2";
    EXPECT_PROBLEM(OpenTypeFont::parsed(cffTestFont(variable)), "CFF2");
    // Neither table.
    CFFTestFontSpec missing = minimalCFFTestSpec();
    missing.tableTag = "XXXX";
    EXPECT_PROBLEM(OpenTypeFont::parsed(cffTestFont(missing)), "no CFF table");

    // Charstring features that are refused with a readable message.
    const CompactFontFormat font = fontOfCharstrings({
        TestCharstring().args({1, 2, 65, 66}).op(cs::endchar),                                   // 1: seac
        TestCharstring().args({1, 2}).escaped(10).op(cs::endchar),                                // 2: 12 10 (add)
        TestCharstring().args({0, 0}).op(cs::rmoveto).args({10}).op(cs::rlineto),                 // 3: rlineto with one argument
        TestCharstring().args({100}).op(cs::callsubr),                                            // 4: a subroutine that is not there
        TestCharstring().args({0, 0}).op(cs::rmoveto).op(cs::vlineto),                            // 5: no arguments
        TestCharstring().n(0).op(2),                                                              // 6: a reserved operator
    });
    EXPECT_PROBLEM(font.glyph(1), "seac");
    EXPECT_PROBLEM(font.glyph(2), "not supported");
    EXPECT_PROBLEM(font.glyph(3), "wrong number");
    EXPECT_PROBLEM(font.glyph(4), "not there");
    EXPECT_PROBLEM(font.glyph(5), "wrong number");
    EXPECT_PROBLEM(font.glyph(6), "reserved");

    // 49 operands overflow the stack of 48; 48 do not.
    TestCharstring tooMany;
    for (int count = 0; count < 49; count += 1) tooMany.n(1);
    tooMany.op(cs::endchar);
    TestCharstring atTheLimit;
    for (int count = 0; count < 48; count += 1) atTheLimit.n(1);
    atTheLimit.op(cs::hstem).op(cs::endchar);
    const CompactFontFormat stack = fontOfCharstrings({tooMany, atTheLimit});
    EXPECT_PROBLEM(stack.glyph(1), "48");
    EXPECT_EQUAL(stack.glyph(2).outline.contours.size(), size_t(0));

    // A subroutine that calls itself nests past the limit of 10.
    CFFTestFontSpec loop;
    loop.charstrings.push_back(TestCharstring().op(cs::endchar).bytes);
    loop.charstrings.push_back(TestCharstring().n(-107).op(cs::callsubr).op(cs::endchar).bytes);
    loop.privateData.localSubroutines.push_back(TestCharstring().n(-107).op(cs::callsubr).op(cs::returnFromSubroutine).bytes);
    const CompactFontFormat looping = CompactFontFormat::parse(cffTable(loop).data);
    EXPECT_PROBLEM(looping.glyph(1), "nests");

    // A number cut off by the end of its charstring.
    CFFTestFontSpec cut;
    cut.charstrings.push_back(TestCharstring().op(cs::endchar).bytes);
    cut.charstrings.push_back(TestCharstring().raw(28).raw(1).bytes);
    EXPECT_PROBLEM(CompactFontFormat::parse(cffTable(cut).data).glyph(1), "middle of a number");
}

TEST(cffDamagedTablesThrowInsteadOfReadingPastTheEnd) {
    const CFFTestFontSpec spec = minimalCFFTestSpec();
    const std::vector<uint8_t> table = cffTable(spec).data;
    // Every prefix of the table either parses (and then every glyph runs or throws) or throws a problem.
    for (size_t length = 0; length < table.size(); length += 1) {
        try {
            const CompactFontFormat font = CompactFontFormat::parse(std::vector<uint8_t>(table.begin(), table.begin() + static_cast<std::ptrdiff_t>(length)));
            for (int glyph = 0; glyph < font.glyphCount(); glyph += 1) font.glyph(glyph);
        } catch (const FontProblem&) {
        }
    }
    EXPECT_PROBLEM(CompactFontFormat::parse({}), "ends too soon");
    EXPECT_PROBLEM(CompactFontFormat::parse(std::vector<uint8_t>(table.begin(), table.begin() + 40)), "CFF");
    // Every byte replaced in turn by a few other values: a problem or a result, nothing else.
    for (size_t position = 0; position < table.size(); position += 1) {
        for (uint8_t value : {uint8_t(0), uint8_t(0x1D), uint8_t(0xFF)}) {
            std::vector<uint8_t> damaged = table;
            damaged[position] = value;
            try {
                const CompactFontFormat font = CompactFontFormat::parse(damaged);
                for (int glyph = 0; glyph < font.glyphCount(); glyph += 1) font.glyph(glyph);
            } catch (const FontProblem&) {
            }
        }
    }
    // A major version other than 1 is not CFF version 1.
    std::vector<uint8_t> versionTwo = table;
    versionTwo[0] = 2;
    EXPECT_PROBLEM(CompactFontFormat::parse(versionTwo), "version 2");

    // In a whole font: a file cut inside its CFF table is refused by the table directory.
    std::vector<uint8_t> file = minimalCFFTestFont();
    file.resize(file.size() - 40);
    EXPECT_PROBLEM(OpenTypeFont::parsed(file), "font");
    // A table that is only the start of a real one.
    std::vector<uint8_t> font = cffTestFont(spec);
    // The CFF table is first in the file (right after the directory of six records); shorten its length field to 20 bytes.
    const size_t directoryRecord = 12;
    font[directoryRecord + 12] = 0;
    font[directoryRecord + 13] = 0;
    font[directoryRecord + 14] = 0;
    font[directoryRecord + 15] = 20;
    EXPECT_PROBLEM(OpenTypeFont::parsed(font), "CFF");
}
