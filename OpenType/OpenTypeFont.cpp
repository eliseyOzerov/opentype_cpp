#include "OpenTypeFont.h"


namespace editor {

namespace {

constexpr int maximumCompositeDepth = 8;

/// The tag as four characters, such as "glyf".
std::string tagText(uint32_t tag) {
    std::string text(4, ' ');
    for (int index = 0; index < 4; index += 1) text[static_cast<size_t>(index)] = static_cast<char>((tag >> (24 - 8 * index)) & 0xFF);
    return text;
}

/// The midpoint of two points: (a + b) / 2.
FontPoint midpoint(FontPoint first, FontPoint second) { return FontPoint{(first.x + second.x) * 0.5f, (first.y + second.y) * 0.5f}; }

} // namespace

uint8_t OpenTypeFont::byteAt(size_t offset) const {
    if (offset >= bytes.size()) throw FontProblem("the font file ends too soon.");
    return bytes[offset];
}

uint16_t OpenTypeFont::unsigned16(size_t offset) const {
    return static_cast<uint16_t>((byteAt(offset) << 8) | byteAt(offset + 1));
}

int16_t OpenTypeFont::signed16(size_t offset) const { return static_cast<int16_t>(unsigned16(offset)); }

uint32_t OpenTypeFont::unsigned32(size_t offset) const {
    return (static_cast<uint32_t>(unsigned16(offset)) << 16) | unsigned16(offset + 2);
}

OpenTypeFont::Table OpenTypeFont::tableNamed(const std::string& tag) const {
    const uint16_t count = unsigned16(4);
    for (int index = 0; index < count; index += 1) {
        const size_t record = 12 + 16 * static_cast<size_t>(index);
        if (tagText(unsigned32(record)) == tag) {
            Table table;
            table.offset = unsigned32(record + 8);
            table.length = unsigned32(record + 12);
            table.present = true;
            if (static_cast<size_t>(table.offset) + table.length > bytes.size()) {
                throw FontProblem("the font's " + tag + " table runs past the end of the file.");
            }
            return table;
        }
    }
    return Table{};
}

OpenTypeFont OpenTypeFont::parsed(std::vector<uint8_t> input) {
    OpenTypeFont font;
    font.bytes = std::move(input);
    if (font.bytes.size() < 12) throw FontProblem("the font file is too short to be a font.");
    const uint32_t version = font.unsigned32(0);
    const bool hasCFFOutlines = version == 0x4F54544F; // "OTTO"
    if (!hasCFFOutlines && version != 0x00010000 && version != 0x74727565) throw FontProblem("this is not a TrueType font.");

    font.head = font.tableNamed("head");
    font.maxp = font.tableNamed("maxp");
    font.cmap = font.tableNamed("cmap");
    font.hhea = font.tableNamed("hhea");
    font.hmtx = font.tableNamed("hmtx");
    font.kern = font.tableNamed("kern");
    std::vector<std::pair<const Table*, const char*>> required = {{&font.head, "head"}, {&font.maxp, "maxp"}, {&font.cmap, "cmap"},
                                                                  {&font.hhea, "hhea"}, {&font.hmtx, "hmtx"}};
    Table cffTable;
    if (hasCFFOutlines) {
        cffTable = font.tableNamed("CFF ");
        if (!cffTable.present && font.tableNamed("CFF2").present) {
            throw FontProblem("this font has CFF2 outlines (variable OpenType); only TrueType and CFF outlines are read.");
        }
        required.push_back({&cffTable, "CFF"});
    } else {
        font.loca = font.tableNamed("loca");
        font.glyf = font.tableNamed("glyf");
        required.push_back({&font.loca, "loca"});
        required.push_back({&font.glyf, "glyf"});
    }
    for (const auto& [table, name] : required) {
        if (!table->present) throw FontProblem(std::string("the font has no ") + name + " table.");
    }

    font.unitsPerEmValue = font.unsigned16(font.head.offset + 18);
    if (font.unitsPerEmValue <= 0) throw FontProblem("the font's unitsPerEm is zero.");
    font.hasLongOffsets = font.signed16(font.head.offset + 50) != 0;
    font.glyphCountValue = font.unsigned16(font.maxp.offset + 4);
    font.ascenderUnits = font.signed16(font.hhea.offset + 4);
    font.descenderUnits = font.signed16(font.hhea.offset + 6);
    font.lineGapUnits = font.signed16(font.hhea.offset + 8);
    font.metricCount = font.unsigned16(font.hhea.offset + 34);
    font.chooseCharacterMap();
    if (hasCFFOutlines) {
        const auto begin = font.bytes.begin() + static_cast<std::ptrdiff_t>(cffTable.offset);
        font.cff = CompactFontFormat::parse(std::vector<uint8_t>(begin, begin + static_cast<std::ptrdiff_t>(cffTable.length)));
    }
    return font;
}

void OpenTypeFont::chooseCharacterMap() {
    const uint16_t count = unsigned16(cmap.offset + 2);
    int bestScore = 0;
    for (int index = 0; index < count; index += 1) {
        const size_t record = cmap.offset + 4 + 8 * static_cast<size_t>(index);
        const uint16_t platform = unsigned16(record);
        const uint16_t encoding = unsigned16(record + 2);
        const uint32_t offset = cmap.offset + unsigned32(record + 4);
        const uint16_t format = unsigned16(offset);
        // Unicode maps only: platform 0, or Windows (3) with encoding 1 (BMP) or 10 (full).
        const bool isUnicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!isUnicode || (format != 4 && format != 12)) continue;
        const int score = format == 12 ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            cmapOffset = offset;
            cmapFormat = format;
        }
    }
    if (bestScore == 0) throw FontProblem("the font has no Unicode character map (cmap format 4 or 12).");
}


int OpenTypeFont::glyphIndex(uint32_t codepoint) const {
    if (cmapFormat == 12) {
        const uint32_t groupCount = unsigned32(cmapOffset + 12);
        // Groups are sorted by start code: binary search for the one holding the character.
        uint32_t low = 0;
        uint32_t high = groupCount;
        while (low < high) {
            const uint32_t middle = low + (high - low) / 2;
            const size_t group = cmapOffset + 16 + 12 * static_cast<size_t>(middle);
            const uint32_t first = unsigned32(group);
            const uint32_t last = unsigned32(group + 4);
            if (codepoint < first) {
                high = middle;
            } else if (codepoint > last) {
                low = middle + 1;
            } else {
                return static_cast<int>(unsigned32(group + 8) + (codepoint - first));
            }
        }
        return 0;
    }
    if (codepoint > 0xFFFF) return 0;
    const int segmentCount = unsigned16(cmapOffset + 6) / 2;
    const size_t endCodes = cmapOffset + 14;
    const size_t startCodes = endCodes + 2 * static_cast<size_t>(segmentCount) + 2;
    const size_t deltas = startCodes + 2 * static_cast<size_t>(segmentCount);
    const size_t rangeOffsets = deltas + 2 * static_cast<size_t>(segmentCount);
    for (int segment = 0; segment < segmentCount; segment += 1) {
        const size_t at = 2 * static_cast<size_t>(segment);
        if (unsigned16(endCodes + at) < codepoint) continue;
        const uint16_t start = unsigned16(startCodes + at);
        if (codepoint < start) return 0;
        const uint16_t delta = unsigned16(deltas + at);
        const uint16_t rangeOffset = unsigned16(rangeOffsets + at);
        if (rangeOffset == 0) return static_cast<uint16_t>(codepoint + delta);
        const size_t glyphAt = rangeOffsets + at + rangeOffset + 2 * (codepoint - start);
        const uint16_t glyph = unsigned16(glyphAt);
        return glyph == 0 ? 0 : static_cast<uint16_t>(glyph + delta);
    }
    return 0;
}

int OpenTypeFont::advance(int glyph) const {
    if (glyph < 0 || glyph >= glyphCountValue || metricCount == 0) return 0;
    const int metric = glyph < metricCount ? glyph : metricCount - 1;
    const uint16_t units = unsigned16(hmtx.offset + 4 * static_cast<size_t>(metric));
    return units;
}

int OpenTypeFont::kerning(int left, int right) const {
    if (!kern.present || kern.length < 4) return 0;
    const uint16_t tableCount = unsigned16(kern.offset + 2);
    size_t subtable = kern.offset + 4;
    int units = 0;
    for (int index = 0; index < tableCount; index += 1) {
        const uint16_t length = unsigned16(subtable + 2);
        const uint16_t coverage = unsigned16(subtable + 4);
        const bool isHorizontalFormatZero = (coverage & 1) != 0 && (coverage >> 8) == 0;
        if (isHorizontalFormatZero) {
            const uint16_t pairCount = unsigned16(subtable + 6);
            const uint32_t key = (static_cast<uint32_t>(left) << 16) | static_cast<uint32_t>(right);
            // Pairs are sorted by (left, right) as one 32-bit key: binary search.
            int low = 0;
            int high = pairCount;
            while (low < high) {
                const int middle = low + (high - low) / 2;
                const size_t pair = subtable + 14 + 6 * static_cast<size_t>(middle);
                const uint32_t found = unsigned32(pair);
                if (key < found) {
                    high = middle;
                } else if (key > found) {
                    low = middle + 1;
                } else {
                    units += signed16(pair + 4);
                    break;
                }
            }
        }
        subtable += length;
    }
    return units;
}

std::pair<uint32_t, uint32_t> OpenTypeFont::glyphRange(int glyph) const {
    if (glyph < 0 || glyph >= glyphCountValue) return {0, 0};
    uint32_t start = 0;
    uint32_t end = 0;
    if (hasLongOffsets) {
        start = unsigned32(loca.offset + 4 * static_cast<size_t>(glyph));
        end = unsigned32(loca.offset + 4 * static_cast<size_t>(glyph) + 4);
    } else {
        start = 2u * unsigned16(loca.offset + 2 * static_cast<size_t>(glyph));
        end = 2u * unsigned16(loca.offset + 2 * static_cast<size_t>(glyph) + 2);
    }
    if (end <= start) return {0, 0};
    if (end > glyf.length) throw FontProblem("glyph " + std::to_string(glyph) + " runs past the end of the glyf table.");
    return {start, end - start};
}

std::vector<OpenTypeFont::Contour> OpenTypeFont::contoursOf(int glyph, int depth) const {
    std::vector<Contour> contours;
    const auto [start, length] = glyphRange(glyph);
    if (length < 10) return contours;
    const size_t base = glyf.offset + start;
    const int contourCount = signed16(base);
    size_t cursor = base + 10;

    if (contourCount >= 0) {
        std::vector<int> ends;
        for (int index = 0; index < contourCount; index += 1) {
            ends.push_back(unsigned16(cursor));
            cursor += 2;
        }
        const int pointCount = ends.empty() ? 0 : ends.back() + 1;
        const uint16_t instructionLength = unsigned16(cursor);
        cursor += 2 + instructionLength;

        // Flags, with a repeat count after any flag whose bit 3 is set.
        std::vector<uint8_t> flags;
        while (static_cast<int>(flags.size()) < pointCount) {
            const uint8_t flag = byteAt(cursor);
            cursor += 1;
            flags.push_back(flag);
            if ((flag & 0x08) != 0) {
                const int repeats = byteAt(cursor);
                cursor += 1;
                for (int repeat = 0; repeat < repeats && static_cast<int>(flags.size()) < pointCount; repeat += 1) flags.push_back(flag);
            }
        }
        // Coordinates are deltas: a short one is a byte whose sign is a flag bit, a "same" one is 0, else a 16-bit value.
        auto readCoordinates = [&](uint8_t shortBit, uint8_t sameBit) {
            std::vector<int> values;
            int current = 0;
            for (uint8_t flag : flags) {
                if ((flag & shortBit) != 0) {
                    const int magnitude = byteAt(cursor);
                    cursor += 1;
                    current += (flag & sameBit) != 0 ? magnitude : -magnitude;
                } else if ((flag & sameBit) == 0) {
                    current += signed16(cursor);
                    cursor += 2;
                }
                values.push_back(current);
            }
            return values;
        };
        const std::vector<int> xs = readCoordinates(0x02, 0x10);
        const std::vector<int> ys = readCoordinates(0x04, 0x20);
        int first = 0;
        for (int end : ends) {
            Contour contour;
            for (int index = first; index <= end && index < pointCount; index += 1) {
                contour.push_back(GlyphPoint{static_cast<float>(xs[static_cast<size_t>(index)]),
                                             static_cast<float>(ys[static_cast<size_t>(index)]),
                                             (flags[static_cast<size_t>(index)] & 0x01) != 0});
            }
            contours.push_back(contour);
            first = end + 1;
        }
        return contours;
    }

    // A composite glyph: parts, each placed by an offset and a 2 × 2 matrix.
    if (depth >= maximumCompositeDepth) throw FontProblem("glyph " + std::to_string(glyph) + " nests composite glyphs too deeply.");
    constexpr uint16_t wordArguments = 0x0001, xyValues = 0x0002, singleScale = 0x0008, moreComponents = 0x0020,
                       separateScales = 0x0040, fullMatrix = 0x0080;
    uint16_t flags = 0;
    do {
        flags = unsigned16(cursor);
        const int part = unsigned16(cursor + 2);
        cursor += 4;
        float offsetX = 0;
        float offsetY = 0;
        if ((flags & wordArguments) != 0) {
            if ((flags & xyValues) != 0) {
                offsetX = signed16(cursor);
                offsetY = signed16(cursor + 2);
            }
            cursor += 4;
        } else {
            if ((flags & xyValues) != 0) {
                offsetX = static_cast<int8_t>(byteAt(cursor));
                offsetY = static_cast<int8_t>(byteAt(cursor + 1));
            }
            cursor += 2;
        }
        // F2Dot14 numbers: a signed 16-bit value over 2^14.
        auto fixed = [&](size_t at) { return static_cast<float>(signed16(at)) / 16384.0f; };
        float xx = 1, xy = 0, yx = 0, yy = 1;
        if ((flags & singleScale) != 0) {
            xx = yy = fixed(cursor);
            cursor += 2;
        } else if ((flags & separateScales) != 0) {
            xx = fixed(cursor);
            yy = fixed(cursor + 2);
            cursor += 4;
        } else if ((flags & fullMatrix) != 0) {
            // The file order is x scale, 01, 10, y scale; x' = xx·x + xy·y, y' = yx·x + yy·y.
            xx = fixed(cursor);
            yx = fixed(cursor + 2);
            xy = fixed(cursor + 4);
            yy = fixed(cursor + 6);
            cursor += 8;
        }
        for (Contour contour : contoursOf(part, depth + 1)) {
            for (GlyphPoint& point : contour) {
                const float x = point.x;
                const float y = point.y;
                point.x = xx * x + xy * y + offsetX;
                point.y = yx * x + yy * y + offsetY;
            }
            contours.push_back(contour);
        }
    } while ((flags & moreComponents) != 0);
    return contours;
}

GlyphOutline OpenTypeFont::trueTypeOutline(int glyph) const {
    GlyphOutline outline;
    for (const Contour& contour : contoursOf(glyph, 0)) {
        const size_t count = contour.size();
        if (count < 2) continue;
        auto pointOf = [](const GlyphPoint& point) { return FontPoint{point.x, point.y}; };

        // The walk starts on a point of the curve: the first on-curve point, or, when every point is
        // off-curve, the midpoint of the last and the first.
        size_t firstOn = count;
        for (size_t index = 0; index < count; index += 1) {
            if (contour[index].isOnCurve) {
                firstOn = index;
                break;
            }
        }
        FontPoint start;
        std::vector<GlyphPoint> rest;
        if (firstOn < count) {
            start = pointOf(contour[firstOn]);
            for (size_t step = 1; step < count; step += 1) rest.push_back(contour[(firstOn + step) % count]);
        } else {
            start = midpoint(pointOf(contour[count - 1]), pointOf(contour[0]));
            for (const GlyphPoint& point : contour) rest.push_back(point);
        }
        GlyphContour drawn;
        drawn.start = start;
        FontPoint current = start;
        bool hasControl = false;
        FontPoint control;
        auto addLine = [&](FontPoint end) {
            GlyphSegment segment;
            segment.end = end;
            drawn.segments.push_back(segment);
        };
        auto addQuadratic = [&](FontPoint quadraticControl, FontPoint end) {
            GlyphSegment segment;
            segment.kind = GlyphSegment::Kind::quadratic;
            segment.control1 = quadraticControl;
            segment.end = end;
            drawn.segments.push_back(segment);
        };
        for (const GlyphPoint& point : rest) {
            const FontPoint position = pointOf(point);
            if (point.isOnCurve) {
                if (hasControl) {
                    addQuadratic(control, position);
                    hasControl = false;
                } else if (!(position == current)) {
                    addLine(position);
                }
                current = position;
            } else {
                if (hasControl) {
                    // Two controls in a row: an on-curve point is implied halfway between them.
                    const FontPoint implied = midpoint(control, position);
                    addQuadratic(control, implied);
                    current = implied;
                }
                control = position;
                hasControl = true;
            }
        }
        // The contour closes back on the start.
        if (hasControl) {
            addQuadratic(control, start);
        } else if (!(current == start)) {
            addLine(start);
        }
        if (drawn.segments.size() >= 2) outline.contours.push_back(std::move(drawn));
    }
    return outline;
}

GlyphOutline OpenTypeFont::compactOutline(int glyph) const {
    if (glyph < 0 || glyph >= cff->glyphCount()) return GlyphOutline{};
    GlyphOutline outline = cff->glyph(glyph).outline;
    // The default font matrix means the charstring units are `unitsPerEm` per em, as in TrueType, and the
    // points are already in font units. A font with its own matrix says how big an em is itself: the matrix
    // takes a point to ems, and times `unitsPerEm` to font units.
    if (cff->usesDefaultFontMatrix(glyph)) return outline;
    std::array<float, 6> matrix = cff->fontMatrix(glyph);
    for (float& number : matrix) number *= static_cast<float>(unitsPerEmValue);
    auto place = [&](FontPoint point) {
        return FontPoint{matrix[0] * point.x + matrix[2] * point.y + matrix[4], matrix[1] * point.x + matrix[3] * point.y + matrix[5]};
    };
    for (GlyphContour& contour : outline.contours) {
        contour.start = place(contour.start);
        for (GlyphSegment& segment : contour.segments) {
            segment.control1 = place(segment.control1);
            segment.control2 = place(segment.control2);
            segment.end = place(segment.end);
        }
    }
    return outline;
}

GlyphOutline OpenTypeFont::outline(int glyph) const {
    return cff ? compactOutline(glyph) : trueTypeOutline(glyph);
}

} // namespace editor
