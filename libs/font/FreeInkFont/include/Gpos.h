#pragma once

// FreeInk SDK — minimal OpenType GPOS pair-kerning reader.
//
// FreeType's FT_Get_Kerning() reads only the legacy 'kern' table, but fonts
// built with modern toolchains (fontmake, Glyphs, current Google Fonts
// releases, all CFF/.otf fonts) ship their kerning exclusively in the GPOS
// table's 'kern' feature — interpreting that is normally HarfBuzz's job,
// which is too heavy to vendor here for one lookup type. This reads exactly
// the 'kern' feature's Pair Adjustment lookups (type 2, and 9 wrapping 2) in
// both PairPos formats (1: per-glyph pair sets, 2: class matrices),
// bounds-checked and capped against malformed/hostile font data, in the same
// style as the GSUB ligature reader (see Gsub.h).
// Only features referenced by the `latn` script's DefaultLangSys are active;
// `DFLT`'s DefaultLangSys is the fallback — same selection policy as Gsub.h,
// and for the same reason: this API has no language input.
//
// Only the FIRST value record's X-advance adjustment is returned (how much
// the left glyph's advance changes before drawing the right glyph) — that is
// the horizontal advance component this reader supports. Placement
// adjustments, second-glyph records, and mark/cursive attachment are out
// of scope.

#include <stddef.h>
#include <stdint.h>

namespace freeink {
namespace font {
namespace gpos {

// Borrowed for one fixed face instance. Coordinates are normalized 16.16
// values AFTER avar mapping, in fvar axis order (FreeType's blend coordinates).
// The parser allocates nothing and shares the pair lookup's bounded work
// budget.
struct VariationContext {
  const uint8_t* gdefBytes = nullptr;
  size_t gdefSize = 0;
  const int32_t* coordinates = nullptr;
  size_t axisCount = 0;
};
constexpr size_t kMaxVariationAxes = 16;

// Returns the X-advance adjustment for drawing `rightGlyph` after
// `leftGlyph`, in FONT UNITS (FUnits, typically negative), or 0 when no
// active lookup covers the pair (or the table is absent/malformed). Scale by
// the face's x_scale for 26.6 pixels at the current size; keep an int64
// intermediate and clamp before narrowing, since deltas can be 32-bit.
// `gposBytes`/`gposSize` is the raw content of the font's 'GPOS' table (e.g.
// from FT_Load_Sfnt_Table), not the whole font file.
// Optional GDEF ItemVariationStore deltas apply to the first record's XAdvance.
// Pixel-size Device adjustments and FeatureVariations substitutions are
// separate mechanisms and are not applied by this design-unit pair-kerning API.
int32_t PairKernAdjustment(const uint8_t* gposBytes, size_t gposSize, uint32_t leftGlyph, uint32_t rightGlyph,
                           const VariationContext* variation = nullptr);

}  // namespace gpos
}  // namespace font
}  // namespace freeink
