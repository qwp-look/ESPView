// ESPView M8-B (B2) - SceneRenderer: LogicalScene -> 128x64 mono OledFb.
//
// Spec: M8-B task book section 3/5/6/7/8/9/25 (Physical Renderer / Text
// Renderer / Responsive Physical Layout / Scene Tests).
//
// This is the replacement for the old thumbnail path (320x240 RGB565 ->
// 2/5 nearest-neighbor -> center crop -> mono). The scene renderer draws the
// shared LogicalScene directly into the 128x64 page-mode framebuffer using
// the built-in 8x8 OLED font, with a deterministic compact physical layout.
//
// Semantic mirror rule (section 9): Virtual and Physical consume the SAME
// LogicalScene (same elements, same order, same text, same semantic state).
// Physical presentation is a compact transformation of that shared layout:
//   row0  title            (clipped to 15 chars)
//   row1  [A] [B]          (button boxes; pressed = filled+inverted label,
//                           focused = leading '>' marker)
//   row3  C:<counter>      (panel box + compact prefix)
//   row5  K:<keyboard>
//   row6  M:<mouse>
// Rendering is deterministic, pure C++17, zero platform deps, no heap.

#pragma once

#include <cstddef>
#include <cstdint>

#include "logical_scene.h"
#include "oled_fb.h"

namespace espview {
namespace oled {

class SceneRenderer {
public:
    // 8x8 font grid on 128x64: 16 cols x 8 rows.
    static constexpr int kCols = OledFb::kWidth / OledFb::kFontWidth;    // 16
    static constexpr int kRows = OledFb::kHeight / OledFb::kFontHeight;  // 8
    // Max text chars per row (15*8 = 120px < 128px; avoids edge glyph clip).
    static constexpr int kTextMaxChars = kCols - 1;

    // Physical compact layout rows (pixel rows = row * 8).
    static constexpr int kRowTitle = 0;
    static constexpr int kRowButtons = 1;
    static constexpr int kRowCounter = 3;
    static constexpr int kRowKeyboard = 5;
    static constexpr int kRowMouse = 6;

    // Render the scene into fb (full redraw, deterministic).
    void render(const display::LogicalScene& scene, OledFb& fb) const;

    // ---- Presentation helpers (public for host tests / diagnostics) ----
    // Clip text to kTextMaxChars chars (NUL terminated).
    static void clipText(const char* text, char* out, size_t cap);
    // "Button A" -> "A" (last whitespace token); no space -> whole text.
    static const char* buttonLabel(const char* text);
    // "Counter:" -> "C:", "Keyboard:" -> "K:", "Mouse:" -> "M:"; else as-is.
    static void compactPrefix(const char* full, char* out, size_t cap);

private:
    void drawButton(OledFb& fb, int boxX, int boxW, const display::SceneElement& e) const;
    void drawBox(OledFb& fb, int x, int y, int w, int h, bool fill) const;
    void drawRowText(OledFb& fb, int row, const char* text) const;
    void drawCentered(OledFb& fb, int boxX, int boxY, int boxW, const char* text,
                      bool inverted) const;
    // XOR glyph draw (black text on white fill / focus markers).
    void drawTextXor(OledFb& fb, int x, int y, const char* text) const;
};

}  // namespace oled
}  // namespace espview
