// Offline check for the text size the panel edits: the number shown is the
// font height in pixels, and the legacy integer glyph multiple the JSON
// protocol carries is derived from it.
//
// Worth pinning because the size now has two audiences that must not disagree.
// The panel and the exported bitmap use the pixel size exactly -- 15 px is
// 15 px -- while the Rust fallback font can only draw whole multiples of its
// 7-pixel cell, so the protocol value is a rounded projection.  If that
// projection truncated instead of rounding, a 15 px label would come out 14 px
// whenever the fallback rendered it; if the range did not line up with the
// protocol's, the panel would offer sizes that get clamped in transit.
//
// Pure arithmetic, so it needs no compositor, no layer shell and no platform
// plugin.  Built only with `-DVSHOT_BUILD_CHECKS=ON`; see the README's
// verification section.

#include "text_size.hpp"

#include <algorithm>
#include <cstdio>

namespace {

int failures = 0;

void expectScale(const char *what, int pixels, std::uint32_t want)
{
    const std::uint32_t got = vshot::textPixelsToScale(pixels);
    if (got != want) {
        std::printf("FAIL  %-52s -> %u (wanted %u)\n", what, got, want);
        ++failures;
        return;
    }
    std::printf("ok    %-52s -> %u\n", what, got);
}

void expectPixels(const char *what, int got, int want)
{
    if (got != want) {
        std::printf("FAIL  %-52s -> %d (wanted %d)\n", what, got, want);
        ++failures;
        return;
    }
    std::printf("ok    %-52s -> %d\n", what, got);
}

} // namespace

int main()
{
    // The number is the pixel size, unmodified: this is the whole point of the
    // change, so it is asserted for a value that is *not* a glyph multiple.
    expectPixels("the default is 14 px", vshot::kDefaultTextPixels, 14);
    expectPixels("the range starts at one glyph cell", vshot::kMinTextPixels, 7);
    expectPixels("the range ends at 64 glyph cells", vshot::kMaxTextPixels, 448);

    // The pixel range must line up with the legacy scale range: 7..448 is
    // exactly scale 1..64, so nothing the panel offers is clamped in transit.
    expectScale("the smallest size is legacy scale 1", vshot::kMinTextPixels, 1);
    expectScale("the largest size is legacy scale 64", vshot::kMaxTextPixels, 64);
    expectScale("the default is legacy scale 2", vshot::kDefaultTextPixels, 2);

    // Every whole glyph multiple must project back to exactly its own scale,
    // or a label drawn at a multiple would not survive the fallback round trip.
    for (int scale = vshot::kMinTextScale; scale <= vshot::kMaxTextScale; ++scale) {
        const int pixels = scale * vshot::kGlyphCellHeight;
        if (vshot::textPixelsToScale(pixels) != static_cast<std::uint32_t>(scale)) {
            std::printf("FAIL  %d px does not project to scale %d\n", pixels, scale);
            ++failures;
        }
    }

    // A size between two multiples rounds to the nearer one rather than
    // shrinking: 15 px is closer to 2 cells (14) than to 3 (21), so it stays
    // scale 2 -- but 18 px must go *up* to scale 3, which truncation would
    // wrongly keep at 2.
    expectScale("15 px rounds to scale 2", 15, 2);
    expectScale("18 px rounds up to scale 3", 18, 3);
    expectScale("21 px is exactly scale 3", 21, 3);
    expectScale("10 px rounds up to scale 1", 10, 1);
    expectScale("11 px rounds up to scale 2", 11, 2);

    // Clamping: out-of-range input lands on the endpoints, never below scale 1
    // (the Rust side rejects or clamps a zero) and never above 64.
    expectPixels("below the range clamps up", vshot::clampTextPixels(0), 7);
    expectPixels("a negative clamps up", vshot::clampTextPixels(-40), 7);
    expectPixels("above the range clamps down", vshot::clampTextPixels(9999), 448);
    expectScale("a tiny size still projects to at least scale 1", 1, 1);
    expectScale("a huge size projects to at most scale 64", 9999, 64);

    if (failures != 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall text-size checks passed\n");
    return 0;
}
