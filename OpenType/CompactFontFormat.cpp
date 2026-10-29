#include "CompactFontFormat.h"

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>

namespace editor {

namespace {

/// The limits of the Type 2 charstring format (Adobe Technical Note #5177, appendix B), plus an
/// operator budget so a hostile font whose subroutines call each other in a wide tree cannot run for ever.
constexpr size_t maximumOperandStack = 48;
constexpr int maximumSubroutineDepth = 10;
constexpr int maximumOperatorCount = 1'000'000;
/// The FontMatrix a CFF font has when it gives none: 1000 units per em.
constexpr float defaultMatrixScale = 0.001f;

// The Top DICT and Private DICT operators this reader uses. A two-byte operator (12 n) is 0x0C00 + n.
constexpr int charStringsOperator = 17;
constexpr int privateOperator = 18;
constexpr int subroutinesOperator = 19;
constexpr int defaultWidthOperator = 20;
constexpr int nominalWidthOperator = 21;
constexpr int charStringTypeOperator = 0x0C06;
constexpr int fontMatrixOperator = 0x0C07;
constexpr int registryOrderingSupplementOperator = 0x0C1E;
constexpr int fontDictionaryArrayOperator = 0x0C24;
constexpr int fontDictionarySelectOperator = 0x0C25;

/// A bounds-checked reader of the table's bytes: every read past the end throws.
class CFFBytes {
public:
    explicit CFFBytes(const std::vector<uint8_t>& data) : data(data) {}

    size_t size() const { return data.size(); }

    uint8_t byteAt(size_t offset) const {
        if (offset >= data.size()) throw FontProblem("the CFF table ends too soon.");
        return data[offset];
    }
    uint16_t unsigned16(size_t offset) const { return static_cast<uint16_t>((byteAt(offset) << 8) | byteAt(offset + 1)); }
    /// An unsigned big-endian number of `byteCount` bytes (1 to 4): an INDEX offset.
    uint32_t unsignedOfSize(size_t offset, int byteCount) const {
        uint32_t value = 0;
        for (int index = 0; index < byteCount; index += 1) value = (value << 8) | byteAt(offset + static_cast<size_t>(index));
        return value;
    }

private:
    const std::vector<uint8_t>& data;
};

/// The INDEX that starts at `offset`; see `CFFIndex`. Every object is checked to lie inside the
/// table, and the offsets must not run backwards.
CFFIndex readIndex(const CFFBytes& reader, size_t offset) {
    CFFIndex index;
    const size_t count = reader.unsigned16(offset);
    if (count == 0) {
        index.end = offset + 2;
        return index;
    }
    const int offsetSize = reader.byteAt(offset + 2);
    if (offsetSize < 1 || offsetSize > 4) {
        throw FontProblem("a CFF INDEX has an offset size of " + std::to_string(offsetSize) + ", not 1 to 4.");
    }
    const size_t offsetsStart = offset + 3;
    // Offsets count from the byte just before the data, which is why the first one is 1.
    const size_t dataBase = offsetsStart + (count + 1) * static_cast<size_t>(offsetSize) - 1;
    size_t previous = reader.unsignedOfSize(offsetsStart, offsetSize);
    if (previous != 1) throw FontProblem("a CFF INDEX does not start its data at offset 1.");
    index.objects.reserve(count);
    for (size_t object = 0; object < count; object += 1) {
        const size_t next = reader.unsignedOfSize(offsetsStart + (object + 1) * static_cast<size_t>(offsetSize), offsetSize);
        if (next < previous) throw FontProblem("a CFF INDEX has offsets that run backwards.");
        if (dataBase + next > reader.size()) throw FontProblem("a CFF INDEX object runs past the end of the table.");
        index.objects.push_back(CFFObjectRange{dataBase + previous, next - previous});
        previous = next;
    }
    index.end = dataBase + previous;
    return index;
}

/// Reads the operand that DICTs and charstrings write the same way, starting at `offset`, into
/// `value` and returns its length in bytes; returns 0 when the byte there starts some other kind of
/// operand or an operator.
///
/// How (Technical Note #5176 table 3, #5177 table 1): b0 32 to 246 is b0 - 139; 247 to 250 is
/// (b0 - 247)·256 + b1 + 108; 251 to 254 is -(b0 - 251)·256 - b1 - 108; 28 is a signed 16-bit integer.
/// `end` is one past the last byte of the object being read, so a number cut by it throws.
size_t readSharedOperand(const CFFBytes& reader, size_t offset, size_t end, double& value) {
    const int firstByte = reader.byteAt(offset);
    auto requireBytes = [&](size_t count) {
        if (offset + count > end) throw FontProblem("a CFF DICT or charstring ends in the middle of a number.");
    };
    if (firstByte >= 32 && firstByte <= 246) {
        value = firstByte - 139;
        return 1;
    }
    if (firstByte >= 247 && firstByte <= 250) {
        requireBytes(2);
        value = (firstByte - 247) * 256 + reader.byteAt(offset + 1) + 108;
        return 2;
    }
    if (firstByte >= 251 && firstByte <= 254) {
        requireBytes(2);
        value = -(firstByte - 251) * 256 - reader.byteAt(offset + 1) - 108;
        return 2;
    }
    if (firstByte == 28) {
        requireBytes(3);
        value = static_cast<int16_t>(reader.unsigned16(offset + 1));
        return 3;
    }
    return 0;
}

/// Reads a DICT real number (the byte 30 and its packed decimal digits) that starts at `offset` into
/// `value` and returns its length in bytes.
///
/// How: each byte holds two nibbles, high first: 0 to 9 a digit, 0xA the decimal point, 0xB the
/// exponent mark "E", 0xC "E-", 0xE a minus sign, 0xF the end. The nibbles are written out as text and
/// converted with `strtod`.
size_t readRealOperand(const CFFBytes& reader, size_t offset, size_t end, double& value) {
    std::string text;
    size_t at = offset + 1;
    bool isFinished = false;
    while (!isFinished) {
        if (at >= end) throw FontProblem("a CFF DICT ends in the middle of a real number.");
        const int packed = reader.byteAt(at);
        at += 1;
        for (int nibble : {packed >> 4, packed & 0x0F}) {
            if (nibble <= 9) {
                text += static_cast<char>('0' + nibble);
            } else if (nibble == 0xA) {
                text += '.';
            } else if (nibble == 0xB) {
                text += 'E';
            } else if (nibble == 0xC) {
                text += "E-";
            } else if (nibble == 0xE) {
                text += '-';
            } else if (nibble == 0xF) {
                isFinished = true;
                break;
            } else {
                throw FontProblem("a CFF DICT has a real number with a reserved digit.");
            }
        }
        if (text.size() > 64) throw FontProblem("a CFF DICT has a real number that is too long.");
    }
    value = text.empty() ? 0 : std::strtod(text.c_str(), nullptr);
    return at - offset;
}

using DictionaryEntries = std::map<int, std::vector<double>>;

/// The operators of the DICT in `range`, each with the operands written before it. An operator that
/// appears twice keeps its last operands.
///
/// How: a DICT is a flat run of operands then operators. A byte 0 to 21 is an operator (12 takes a
/// second byte: 0x0C00 + b1); 28, 29 (a 32-bit integer), 30 (a real) and 32 to 254 start an operand;
/// anything else is reserved and throws.
DictionaryEntries readDictionary(const CFFBytes& reader, CFFObjectRange range) {
    DictionaryEntries entries;
    std::vector<double> operands;
    const size_t end = range.start + range.length;
    size_t at = range.start;
    while (at < end) {
        const int firstByte = reader.byteAt(at);
        double value = 0;
        if (firstByte <= 21) {
            int code = firstByte;
            at += 1;
            if (firstByte == 12) {
                if (at >= end) throw FontProblem("a CFF DICT ends in the middle of an operator.");
                code = 0x0C00 | reader.byteAt(at);
                at += 1;
            }
            entries[code] = operands;
            operands.clear();
        } else if (const size_t length = readSharedOperand(reader, at, end, value); length > 0) {
            operands.push_back(value);
            at += length;
        } else if (firstByte == 29) {
            if (at + 5 > end) throw FontProblem("a CFF DICT ends in the middle of a number.");
            value = static_cast<int32_t>((static_cast<uint32_t>(reader.unsigned16(at + 1)) << 16) | reader.unsigned16(at + 3));
            operands.push_back(value);
            at += 5;
        } else if (firstByte == 30) {
            at += readRealOperand(reader, at, end, value);
            operands.push_back(value);
        } else {
            throw FontProblem("a CFF DICT has the reserved byte " + std::to_string(firstByte) + ".");
        }
        if (operands.size() > 64) throw FontProblem("a CFF DICT has too many operands.");
    }
    return entries;
}

/// The `operandIndex`th operand of `code` as a non-negative whole number such as an offset or a
/// size; throws when the operator has fewer operands or the number is negative or absurd.
size_t offsetOperand(const DictionaryEntries& entries, int code, size_t operandIndex, const char* what) {
    const auto found = entries.find(code);
    if (found == entries.end() || operandIndex >= found->second.size()) {
        throw FontProblem(std::string("the CFF table's DICT has no ") + what + ".");
    }
    const double value = found->second[operandIndex];
    if (!(value >= 0) || value > 2147483647.0) throw FontProblem(std::string("the CFF table's ") + what + " is not a valid offset.");
    return static_cast<size_t>(value);
}

/// The six numbers of a FontMatrix operator, or nothing when the DICT has none.
std::optional<std::array<float, 6>> matrixOperand(const DictionaryEntries& entries) {
    const auto found = entries.find(fontMatrixOperator);
    if (found == entries.end()) return std::nullopt;
    if (found->second.size() != 6) throw FontProblem("the CFF table's FontMatrix does not have six numbers.");
    std::array<float, 6> matrix{};
    for (size_t index = 0; index < 6; index += 1) matrix[index] = static_cast<float>(found->second[index]);
    return matrix;
}

/// The matrix that applies `first`, then `second`: x' = a₂(a₁x + c₁y + e₁) + c₂(b₁x + d₁y + f₁) + e₂,
/// and likewise for y, written out for [a b c d e f].
std::array<float, 6> concatenated(const std::array<float, 6>& first, const std::array<float, 6>& second) {
    return {second[0] * first[0] + second[2] * first[1],
            second[1] * first[0] + second[3] * first[1],
            second[0] * first[2] + second[2] * first[3],
            second[1] * first[2] + second[3] * first[3],
            second[0] * first[4] + second[2] * first[5] + second[4],
            second[1] * first[4] + second[3] * first[5] + second[5]};
}

/// The subroutine bias for an INDEX of `count` subroutines: 107 below 1240, 1131 below 33900, else
/// 32768. A `callsubr` operand is added to it to find the subroutine, so that small numbers (cheap
/// to write) reach the first 1240 subroutines from either side of zero (#5177 section 4.7).
int subroutineBias(size_t count) {
    if (count < 1240) return 107;
    if (count < 33900) return 1131;
    return 32768;
}

/// The charstring interpreter: runs one glyph's charstring (and the subroutines it
/// calls) and collects its contours and width.
///
/// How (Technical Note #5177): a charstring is operands pushed on a stack of at most 48 numbers and
/// operators that consume them. Path operators move the pen by deltas (`rmoveto`, `rlineto`,
/// `rrcurveto` and the shorthand forms with a zero delta left out) and each takes its arguments from
/// the bottom of the stack and clears it. The first stack-clearing operator may carry one extra
/// leading argument, the glyph's width relative to `nominalWidthX`. Hint operators draw nothing, but
/// they count stems because `hintmask` is followed by ceil(stems / 8) bytes of mask that must be skipped.
/// A move or `endchar` closes the contour being drawn.
class CharstringInterpreter {
public:
    CharstringInterpreter(const std::vector<uint8_t>& bytes, const CFFIndex& globalSubroutines, const CFFIndex& localSubroutines,
                          float defaultWidth, float nominalWidth)
        : reader(bytes),
          globalSubroutines(globalSubroutines),
          localSubroutines(localSubroutines),
          nominalWidth(nominalWidth),
          width(defaultWidth) {}

    /// Runs `charstring` to its `endchar` (or its end) and returns what it drew.
    CompactGlyph run(CFFObjectRange charstring) {
        execute(charstring.start, charstring.start + charstring.length, 0);
        finishContour();
        CompactGlyph glyph;
        glyph.outline.contours = std::move(contours);
        glyph.width = width;
        return glyph;
    }

private:
    CFFBytes reader;
    const CFFIndex& globalSubroutines;
    const CFFIndex& localSubroutines;
    float nominalWidth;
    float width;

    std::vector<float> stack;
    std::vector<GlyphContour> contours;
    GlyphContour current;
    bool hasContour = false;
    float penX = 0;
    float penY = 0;
    int stemCount = 0;
    bool hasSeenFirstOperator = false;
    int operatorCount = 0;

    void push(float value) {
        if (stack.size() >= maximumOperandStack) throw FontProblem("a charstring pushes more than 48 numbers.");
        stack.push_back(value);
    }

    /// Throws the readable problem for an operator given an argument count it does not accept.
    void requireArguments(bool isValid, const char* name) const {
        if (!isValid) throw FontProblem(std::string("a charstring gives ") + name + " the wrong number of arguments.");
    }

    /// The width argument: the first stack-clearing operator of a glyph (a stem, a mask, a move or
    /// `endchar`) may have one more argument than it needs, in front; that is the width, as a
    /// difference from `nominalWidthX`. Takes it off the stack when `isPresent`. Later operators never
    /// carry one, so a wrong count there is left for `requireArguments`.
    void takeWidth(bool isPresent) {
        if (!hasSeenFirstOperator && isPresent) {
            width = nominalWidth + stack.front();
            stack.erase(stack.begin());
        }
        hasSeenFirstOperator = true;
    }

    void finishContour() {
        if (hasContour && !current.segments.empty()) contours.push_back(std::move(current));
        current = GlyphContour{};
        hasContour = false;
    }

    /// A draw before any move starts its contour where the pen is.
    void ensureContour() {
        if (hasContour) return;
        current.start = FontPoint{penX, penY};
        hasContour = true;
    }

    /// Moves the pen by (dx, dy) and starts a new contour there, closing the last one.
    void moveBy(float dx, float dy) {
        finishContour();
        penX += dx;
        penY += dy;
        current.start = FontPoint{penX, penY};
        hasContour = true;
    }

    void lineBy(float dx, float dy) {
        ensureContour();
        penX += dx;
        penY += dy;
        GlyphSegment segment;
        segment.end = FontPoint{penX, penY};
        current.segments.push_back(segment);
    }

    /// A cubic whose control points and end are offsets, each from the point before it:
    /// c₁ = p + (dx1, dy1), c₂ = c₁ + (dx2, dy2), end = c₂ + (dx3, dy3).
    void curveBy(float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) {
        ensureContour();
        GlyphSegment segment;
        segment.kind = GlyphSegment::Kind::cubic;
        penX += dx1;
        penY += dy1;
        segment.control1 = FontPoint{penX, penY};
        penX += dx2;
        penY += dy2;
        segment.control2 = FontPoint{penX, penY};
        penX += dx3;
        penY += dy3;
        segment.end = FontPoint{penX, penY};
        current.segments.push_back(segment);
    }

    /// hstem, vstem, hstemhm, vstemhm, and the implicit vstem in front of a mask: count the stems
    /// (two arguments each, after an optional width) and draw nothing.
    void countStems() {
        takeWidth(stack.size() % 2 == 1);
        requireArguments(stack.size() % 2 == 0, "a hint");
        stemCount += static_cast<int>(stack.size() / 2);
        stack.clear();
    }

    /// Runs the charstring bytes [begin, end) at subroutine depth `depth`. Returns true once `endchar`
    /// has run (every caller then stops), false when the bytes ran out or a `return` was met.
    bool execute(size_t begin, size_t end, int depth) {
        size_t at = begin;
        while (at < end) {
            const int firstByte = reader.byteAt(at);
            if (firstByte >= 32 || firstByte == 28) {
                double value = 0;
                if (firstByte == 255) {
                    // A 16.16 fixed-point number: a signed 32-bit integer over 65536.
                    if (at + 5 > end) throw FontProblem("a charstring ends in the middle of a number.");
                    const int32_t fixed = static_cast<int32_t>((static_cast<uint32_t>(reader.unsigned16(at + 1)) << 16) | reader.unsigned16(at + 3));
                    push(static_cast<float>(fixed) / 65536.0f);
                    at += 5;
                } else {
                    at += readSharedOperand(reader, at, end, value);
                    push(static_cast<float>(value));
                }
                continue;
            }
            at += 1;
            operatorCount += 1;
            if (operatorCount > maximumOperatorCount) throw FontProblem("a charstring runs more than a million operators.");

            switch (firstByte) {
            case 1:  // hstem
            case 3:  // vstem
            case 18: // hstemhm
            case 23: // vstemhm
                countStems();
                break;
            case 19: // hintmask
            case 20: // cntrmask
                // Arguments still on the stack are an implicit vstem; then a bit per stem follows.
                countStems();
                at += static_cast<size_t>((stemCount + 7) / 8);
                if (at > end) throw FontProblem("a charstring ends inside a hint mask.");
                break;
            case 21: // rmoveto
                takeWidth(stack.size() > 2);
                requireArguments(stack.size() == 2, "rmoveto");
                moveBy(stack[0], stack[1]);
                stack.clear();
                break;
            case 22: // hmoveto
                takeWidth(stack.size() > 1);
                requireArguments(stack.size() == 1, "hmoveto");
                moveBy(stack[0], 0);
                stack.clear();
                break;
            case 4: // vmoveto
                takeWidth(stack.size() > 1);
                requireArguments(stack.size() == 1, "vmoveto");
                moveBy(0, stack[0]);
                stack.clear();
                break;
            case 5: // rlineto: pairs of (dx, dy)
                requireArguments(stack.size() >= 2 && stack.size() % 2 == 0, "rlineto");
                for (size_t index = 0; index < stack.size(); index += 2) lineBy(stack[index], stack[index + 1]);
                stack.clear();
                break;
            case 6: // hlineto
            case 7: { // vlineto: one delta each, alternating horizontal and vertical
                requireArguments(!stack.empty(), firstByte == 6 ? "hlineto" : "vlineto");
                bool isHorizontal = firstByte == 6;
                for (float delta : stack) {
                    if (isHorizontal) {
                        lineBy(delta, 0);
                    } else {
                        lineBy(0, delta);
                    }
                    isHorizontal = !isHorizontal;
                }
                stack.clear();
                break;
            }
            case 8: // rrcurveto: sets of six
                requireArguments(stack.size() >= 6 && stack.size() % 6 == 0, "rrcurveto");
                for (size_t index = 0; index < stack.size(); index += 6) {
                    curveBy(stack[index], stack[index + 1], stack[index + 2], stack[index + 3], stack[index + 4], stack[index + 5]);
                }
                stack.clear();
                break;
            case 24: // rcurveline: curves of six, then one line of two
                requireArguments(stack.size() >= 8 && (stack.size() - 2) % 6 == 0, "rcurveline");
                for (size_t index = 0; index + 2 < stack.size(); index += 6) {
                    curveBy(stack[index], stack[index + 1], stack[index + 2], stack[index + 3], stack[index + 4], stack[index + 5]);
                }
                lineBy(stack[stack.size() - 2], stack[stack.size() - 1]);
                stack.clear();
                break;
            case 25: { // rlinecurve: lines of two, then one curve of six
                requireArguments(stack.size() >= 8 && (stack.size() - 6) % 2 == 0, "rlinecurve");
                const size_t curveStart = stack.size() - 6;
                for (size_t index = 0; index < curveStart; index += 2) lineBy(stack[index], stack[index + 1]);
                curveBy(stack[curveStart], stack[curveStart + 1], stack[curveStart + 2], stack[curveStart + 3], stack[curveStart + 4],
                        stack[curveStart + 5]);
                stack.clear();
                break;
            }
            case 26: { // vvcurveto: dx1? {dya dxb dyb dyc}+
                requireArguments(stack.size() >= 4 && stack.size() % 4 <= 1, "vvcurveto");
                size_t index = 0;
                float firstDeltaX = 0;
                if (stack.size() % 4 == 1) {
                    firstDeltaX = stack[0];
                    index = 1;
                }
                for (; index + 4 <= stack.size(); index += 4) {
                    curveBy(firstDeltaX, stack[index], stack[index + 1], stack[index + 2], 0, stack[index + 3]);
                    firstDeltaX = 0;
                }
                stack.clear();
                break;
            }
            case 27: { // hhcurveto: dy1? {dxa dxb dyb dxc}+
                requireArguments(stack.size() >= 4 && stack.size() % 4 <= 1, "hhcurveto");
                size_t index = 0;
                float firstDeltaY = 0;
                if (stack.size() % 4 == 1) {
                    firstDeltaY = stack[0];
                    index = 1;
                }
                for (; index + 4 <= stack.size(); index += 4) {
                    curveBy(stack[index], firstDeltaY, stack[index + 1], stack[index + 2], stack[index + 3], 0);
                    firstDeltaY = 0;
                }
                stack.clear();
                break;
            }
            case 30: // vhcurveto
            case 31: { // hvcurveto: groups of four that start alternately horizontal and vertical; the last
                       // group may have a fifth argument, the delta of its end on the axis it was not moving along.
                requireArguments(stack.size() >= 4 && stack.size() % 4 <= 1, firstByte == 31 ? "hvcurveto" : "vhcurveto");
                bool isHorizontal = firstByte == 31;
                for (size_t index = 0; index + 4 <= stack.size(); index += 4) {
                    const bool isLast = stack.size() - index == 5;
                    const float extra = isLast ? stack[index + 4] : 0;
                    if (isHorizontal) {
                        curveBy(stack[index], 0, stack[index + 1], stack[index + 2], extra, stack[index + 3]);
                    } else {
                        curveBy(0, stack[index], stack[index + 1], stack[index + 2], stack[index + 3], extra);
                    }
                    isHorizontal = !isHorizontal;
                }
                stack.clear();
                break;
            }
            case 10: // callsubr
            case 29: { // callgsubr
                requireArguments(!stack.empty(), firstByte == 10 ? "callsubr" : "callgsubr");
                const CFFIndex& subroutines = firstByte == 10 ? localSubroutines : globalSubroutines;
                const long number = std::lround(stack.back()) + subroutineBias(subroutines.objects.size());
                stack.pop_back();
                if (number < 0 || static_cast<size_t>(number) >= subroutines.objects.size()) {
                    throw FontProblem("a charstring calls subroutine " + std::to_string(number) + ", which is not there.");
                }
                if (depth + 1 > maximumSubroutineDepth) throw FontProblem("a charstring nests subroutines more than 10 deep.");
                const CFFObjectRange subroutine = subroutines.objects[static_cast<size_t>(number)];
                if (execute(subroutine.start, subroutine.start + subroutine.length, depth + 1)) return true;
                break;
            }
            case 11: // return
                return false;
            case 14: // endchar
                takeWidth(stack.size() == 1 || stack.size() == 5);
                // The accented form (adx ady bchar achar, "seac") draws two other glyphs found by
                // their Standard Encoding names through the charset; this reader refuses it instead.
                if (!stack.empty()) throw FontProblem("a charstring uses the accented form of endchar (seac), which is not supported.");
                finishContour();
                return true;
            case 12: { // an escaped operator: 12 n
                if (at >= end) throw FontProblem("a charstring ends in the middle of an operator.");
                const int second = reader.byteAt(at);
                at += 1;
                runEscapedOperator(second);
                break;
            }
            default:
                throw FontProblem("a charstring uses the operator " + std::to_string(firstByte) + ", which is reserved.");
            }
        }
        return false;
    }

    /// The operators written as 12 and a second byte: the four flex curves, and `dotsection`
    /// (deprecated, a no-op). The arithmetic, storage and conditional operators the specification
    /// deprecated in Type 2 are refused.
    void runEscapedOperator(int second) {
        switch (second) {
        case 0: // dotsection
            stack.clear();
            break;
        case 34: // hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6; both curves start and end on the same horizontal line
            requireArguments(stack.size() == 7, "hflex");
            curveBy(stack[0], 0, stack[1], stack[2], stack[3], 0);
            curveBy(stack[4], 0, stack[5], -stack[2], stack[6], 0);
            stack.clear();
            break;
        case 35: // flex: two curves of six, then the flex depth (ignored: it only picks when to draw straight)
            requireArguments(stack.size() == 13, "flex");
            curveBy(stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]);
            curveBy(stack[6], stack[7], stack[8], stack[9], stack[10], stack[11]);
            stack.clear();
            break;
        case 36: // hflex1: dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6; the last point is back on the starting y
            requireArguments(stack.size() == 9, "hflex1");
            curveBy(stack[0], stack[1], stack[2], stack[3], stack[4], 0);
            curveBy(stack[5], 0, stack[6], stack[7], stack[8], -(stack[1] + stack[3] + stack[7]));
            stack.clear();
            break;
        case 37: { // flex1: five (dx, dy) pairs, then d6, which is dx6 or dy6 by which of the five sums is longer
            requireArguments(stack.size() == 11, "flex1");
            const float sumX = stack[0] + stack[2] + stack[4] + stack[6] + stack[8];
            const float sumY = stack[1] + stack[3] + stack[5] + stack[7] + stack[9];
            curveBy(stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]);
            if (std::fabs(sumX) > std::fabs(sumY)) {
                curveBy(stack[6], stack[7], stack[8], stack[9], stack[10], -sumY);
            } else {
                curveBy(stack[6], stack[7], stack[8], stack[9], -sumX, stack[10]);
            }
            stack.clear();
            break;
        }
        default:
            throw FontProblem("a charstring uses the operator 12 " + std::to_string(second) +
                                 " (arithmetic, storage or conditional), which is not supported.");
        }
    }
};

} // namespace

namespace {

/// What a Private DICT holds that the charstrings need: the two widths and the local subroutines.
struct PrivateData {
    float defaultWidth = 0;
    float nominalWidth = 0;
    CFFIndex localSubroutines;
};

/// The data of the Private DICT that `entries` (a Top DICT or an FD) point to with the Private
/// operator (size, offset), or the defaults (widths 0, no local subroutines) when there is none.
///
/// How: the Private DICT is `size` bytes at `offset` from the start of the table; its Subrs operator
/// is an offset to the local subroutine INDEX measured from the start of the Private DICT.
PrivateData readPrivateData(const CFFBytes& reader, const DictionaryEntries& entries) {
    PrivateData data;
    const auto found = entries.find(privateOperator);
    if (found == entries.end()) return data;
    const size_t size = offsetOperand(entries, privateOperator, 0, "Private DICT size");
    const size_t offset = offsetOperand(entries, privateOperator, 1, "Private DICT offset");
    if (size == 0) return data;
    if (offset + size > reader.size()) throw FontProblem("the CFF table's Private DICT runs past the end of the table.");
    const DictionaryEntries privateEntries = readDictionary(reader, CFFObjectRange{offset, size});
    const auto widthOf = [&](int code) {
        const auto width = privateEntries.find(code);
        return width == privateEntries.end() || width->second.empty() ? 0.0f : static_cast<float>(width->second[0]);
    };
    data.defaultWidth = widthOf(defaultWidthOperator);
    data.nominalWidth = widthOf(nominalWidthOperator);
    if (privateEntries.count(subroutinesOperator) != 0) {
        data.localSubroutines = readIndex(reader, offset + offsetOperand(privateEntries, subroutinesOperator, 0, "local subroutines offset"));
    }
    return data;
}

/// FDSelect, expanded to one font dictionary number per glyph.
///
/// How: format 0 is one byte per glyph. Format 3 is Card16 nRanges, then nRanges (first glyph Card16,
/// fd Card8) pairs, then a sentinel Card16 one past the last glyph; a range runs to the next range's
/// first glyph. The first range must start at glyph 0, firsts must rise, and every number must
/// name an FD that exists.
std::vector<uint8_t> readFontDictionarySelect(const CFFBytes& reader, size_t offset, size_t glyphCount, size_t dictionaryCount) {
    std::vector<uint8_t> select(glyphCount, 0);
    const int format = reader.byteAt(offset);
    auto checked = [&](int number) {
        if (static_cast<size_t>(number) >= dictionaryCount) throw FontProblem("the CFF table's FDSelect names a font dictionary that is not there.");
        return static_cast<uint8_t>(number);
    };
    if (format == 0) {
        for (size_t glyph = 0; glyph < glyphCount; glyph += 1) select[glyph] = checked(reader.byteAt(offset + 1 + glyph));
        return select;
    }
    if (format == 3) {
        const size_t rangeCount = reader.unsigned16(offset + 1);
        const size_t ranges = offset + 3;
        const size_t sentinel = reader.unsigned16(ranges + 3 * rangeCount);
        if (rangeCount == 0 || reader.unsigned16(ranges) != 0) throw FontProblem("the CFF table's FDSelect does not start at glyph 0.");
        if (sentinel < glyphCount) throw FontProblem("the CFF table's FDSelect stops before the last glyph.");
        size_t previousFirst = 0;
        for (size_t range = 0; range < rangeCount; range += 1) {
            const size_t first = reader.unsigned16(ranges + 3 * range);
            const size_t next = range + 1 < rangeCount ? reader.unsigned16(ranges + 3 * (range + 1)) : sentinel;
            if (range > 0 && first <= previousFirst) throw FontProblem("the CFF table's FDSelect ranges do not rise.");
            const uint8_t dictionary = checked(reader.byteAt(ranges + 3 * range + 2));
            for (size_t glyph = first; glyph < next && glyph < glyphCount; glyph += 1) select[glyph] = dictionary;
            previousFirst = first;
        }
        return select;
    }
    throw FontProblem("the CFF table's FDSelect has format " + std::to_string(format) + ", not 0 or 3.");
}

} // namespace

CompactFontFormat CompactFontFormat::parse(std::vector<uint8_t> table) {
    CompactFontFormat font;
    font.bytes = std::move(table);
    const CFFBytes reader(font.bytes);

    // Header: major, minor, hdrSize, offSize. The offSize is the size of the offsets in the *old* per-glyph
    // tables; INDEXes carry their own, so only the major version and the header size matter here.
    const int major = reader.byteAt(0);
    const size_t headerSize = reader.byteAt(2);
    if (major != 1) throw FontProblem("this CFF table is version " + std::to_string(major) + "; only version 1 is read.");
    if (headerSize < 4) throw FontProblem("the CFF header is too small.");

    const CFFIndex names = readIndex(reader, headerSize);
    const CFFIndex topDictionaries = readIndex(reader, names.end);
    const CFFIndex strings = readIndex(reader, topDictionaries.end);
    font.globalSubroutines = readIndex(reader, strings.end);
    if (topDictionaries.objects.size() != 1) {
        throw FontProblem("the CFF table holds " + std::to_string(topDictionaries.objects.size()) + " fonts; an OpenType font has one.");
    }

    const DictionaryEntries top = readDictionary(reader, topDictionaries.objects[0]);
    const auto charStringType = top.find(charStringTypeOperator);
    if (charStringType != top.end() && !charStringType->second.empty() && charStringType->second[0] != 2) {
        throw FontProblem("this CFF font uses Type 1 charstrings; only Type 2 is read.");
    }
    font.charStrings = readIndex(reader, offsetOperand(top, charStringsOperator, 0, "CharStrings offset"));
    font.topMatrix = matrixOperand(top);
    font.cidKeyed = top.count(registryOrderingSupplementOperator) != 0;

    if (!font.cidKeyed) {
        const PrivateData data = readPrivateData(reader, top);
        font.dictionaries.push_back(FontDictionary{data.defaultWidth, data.nominalWidth, data.localSubroutines, std::nullopt});
        return font;
    }

    // A CID-keyed font: the FDArray is an INDEX of font DICTs, each with a Private DICT and perhaps its
    // own FontMatrix, and FDSelect says which one each glyph uses.
    const CFFIndex fontDictionaries = readIndex(reader, offsetOperand(top, fontDictionaryArrayOperator, 0, "FDArray offset"));
    if (fontDictionaries.objects.empty()) throw FontProblem("the CFF table's FDArray is empty.");
    if (fontDictionaries.objects.size() > 256) throw FontProblem("the CFF table's FDArray has more than 256 font dictionaries.");
    for (const CFFObjectRange& object : fontDictionaries.objects) {
        const DictionaryEntries entries = readDictionary(reader, object);
        const PrivateData data = readPrivateData(reader, entries);
        font.dictionaries.push_back(FontDictionary{data.defaultWidth, data.nominalWidth, data.localSubroutines, matrixOperand(entries)});
    }
    font.dictionaryOfGlyph = readFontDictionarySelect(reader, offsetOperand(top, fontDictionarySelectOperator, 0, "FDSelect offset"),
                                                      font.charStrings.objects.size(), font.dictionaries.size());
    return font;
}

const CompactFontFormat::FontDictionary& CompactFontFormat::dictionaryFor(int glyphNumber) const {
    if (dictionaryOfGlyph.empty()) return dictionaries[0];
    return dictionaries[dictionaryOfGlyph[static_cast<size_t>(glyphNumber)]];
}

CompactGlyph CompactFontFormat::glyph(int glyphNumber) const {
    if (glyphNumber < 0 || glyphNumber >= glyphCount()) return CompactGlyph{};
    const FontDictionary& dictionary = dictionaryFor(glyphNumber);
    CharstringInterpreter interpreter(bytes, globalSubroutines, dictionary.localSubroutines, dictionary.defaultWidth, dictionary.nominalWidth);
    return interpreter.run(charStrings.objects[static_cast<size_t>(glyphNumber)]);
}

std::array<float, 6> CompactFontFormat::fontMatrix(int glyphNumber) const {
    std::optional<std::array<float, 6>> own;
    if (glyphNumber >= 0 && glyphNumber < glyphCount()) own = dictionaryFor(glyphNumber).matrix;
    if (topMatrix && own) return concatenated(*own, *topMatrix);
    if (topMatrix) return *topMatrix;
    if (own) return *own;
    return {defaultMatrixScale, 0, 0, defaultMatrixScale, 0, 0};
}

bool CompactFontFormat::usesDefaultFontMatrix(int glyphNumber) const {
    const std::array<float, 6> matrix = fontMatrix(glyphNumber);
    const std::array<float, 6> standard = {defaultMatrixScale, 0, 0, defaultMatrixScale, 0, 0};
    for (size_t index = 0; index < 6; index += 1) {
        if (std::fabs(matrix[index] - standard[index]) > 1e-7f) return false;
    }
    return true;
}

} // namespace editor
