// ESPView M8-B (B2) / M8-C (C3) - SceneRenderer implementation (see scene_renderer.h).
//
// C3: 布局不再硬编码行号，而是由 PhysicalLayoutPolicy -> layoutFor() 决定
// （Normal / Compact / UltraCompact）。kCompact 保持 M8-B B2 冻结行为
// （[A][B] 同行 + C:/K:/M: 前缀），host 既有 golden 测试不得变化。

#include "scene_renderer.h"

#include <cstdio>
#include <cstring>

namespace espview {
namespace oled {

namespace {

constexpr int kBoxH = OledFb::kFontHeight;         // 8 (one font row)
constexpr int kButtonBoxW = OledFb::kWidth / 2;    // 64

// Prefix map for compact labels (semantic text -> compact physical label).
struct PrefixMap {
    const char* from;
    const char* to;
};
constexpr PrefixMap kPrefixMap[] = {
    {"Counter:", "C:"},
    {"Keyboard:", "K:"},
    {"Mouse:", "M:"},
};

int rowY(int row) { return row * OledFb::kFontHeight; }

// Ultra-compact 合并行："K: <keys> M: <mouse>"，先各自 clip，再整体按
// maxChars 截断（确定性、无堆分配）。
void drawMergedRow(OledFb& fb, int row, const char* kb, const char* ms, int maxChars) {
    char buf[SceneRenderer::kTextMaxChars * 2 + 2];
    std::snprintf(buf, sizeof(buf), "%s %s", kb, ms);
    char clipped[SceneRenderer::kTextMaxChars + 1];
    SceneRenderer::clipText(buf, clipped, sizeof(clipped));  // not in-place
    if (maxChars > 0 && maxChars < SceneRenderer::kTextMaxChars) {
        clipped[maxChars] = '\0';
    }
    fb.drawText(0, rowY(row), clipped);
}

}  // namespace

PhysicalLayout layoutFor(PhysicalLayoutPolicy policy, int fbW, int fbH) {
    // 非法几何（小于 128x64 标准物理屏）回退 kCompact 语义。
    if (fbW < OledFb::kWidth || fbH < OledFb::kHeight) {
        policy = PhysicalLayoutPolicy::kCompact;
    }
    PhysicalLayout layout;
    layout.textMaxChars = SceneRenderer::kTextMaxChars;
    switch (policy) {
        case PhysicalLayoutPolicy::kNormal:
            // 每元素一行、完整文本：title/A/B/counter/keyboard/mouse = 0..5。
            layout.rowTitle = 0;
            layout.rowButtonA = 1;
            layout.rowButtonB = 2;
            layout.rowCounter = 3;
            layout.rowKeyboard = 4;
            layout.rowMouse = 5;
            layout.compactPrefix = false;
            break;
        case PhysicalLayoutPolicy::kUltraCompact:
            // 最密：counter 上移，keyboard/mouse 合并一行。
            layout.rowTitle = 0;
            layout.rowButtonA = 1;
            layout.rowButtonB = 1;
            layout.rowCounter = 2;
            layout.rowKeyboard = 3;
            layout.rowMouse = 3;
            layout.compactPrefix = true;
            break;
        case PhysicalLayoutPolicy::kCompact:
        default:
            // 冻结的 M8-B B2 行为。
            layout.rowTitle = 0;
            layout.rowButtonA = 1;
            layout.rowButtonB = 1;
            layout.rowCounter = 3;
            layout.rowKeyboard = 5;
            layout.rowMouse = 6;
            layout.compactPrefix = true;
            break;
    }
    return layout;
}

void SceneRenderer::clipText(const char* text, char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (text == nullptr) {
        return;
    }
    size_t n = 0;
    while (text[n] != '\0' && n + 1 < cap && n < static_cast<size_t>(kTextMaxChars)) {
        out[n] = text[n];
        ++n;
    }
    out[n] = '\0';
}

const char* SceneRenderer::buttonLabel(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return "?";
    }
    const char* last = text;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == ' ') {
            last = p + 1;
        }
    }
    return (last[0] != '\0') ? last : text;
}

void SceneRenderer::compactPrefix(const char* full, char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (full == nullptr) {
        return;
    }
    for (const PrefixMap& m : kPrefixMap) {
        const size_t fromLen = std::strlen(m.from);
        if (std::strncmp(full, m.from, fromLen) == 0) {
            std::snprintf(out, cap, "%s%s", m.to, full + fromLen);
            return;
        }
    }
    std::snprintf(out, cap, "%s", full);
}

void SceneRenderer::drawBox(OledFb& fb, int x, int y, int w, int h, bool fill) const {
    for (int dy = 0; dy < h; ++dy) {
        for (int dx = 0; dx < w; ++dx) {
            const bool edge = dx == 0 || dy == 0 || dx == w - 1 || dy == h - 1;
            if (fill || edge) {
                fb.setPixel(x + dx, y + dy, true);  // OOB ignored by OledFb
            }
        }
    }
}

void SceneRenderer::drawTextXor(OledFb& fb, int x, int y, const char* text) const {
    if (text == nullptr) {
        return;
    }
    int cx = x;
    for (const char* p = text; *p != '\0' && cx < OledFb::kWidth; ++p) {
        const uint8_t* g = OledFb::fontGlyph(*p);
        for (int r = 0; r < OledFb::kFontHeight; ++r) {
            for (int col = 0; col < OledFb::kFontWidth; ++col) {
                if ((g[r] >> col) & 1u) {
                    const int px = cx + col;
                    const int py = y + r;
                    if (px >= 0 && px < OledFb::kWidth && py >= 0 && py < OledFb::kHeight) {
                        fb.setPixel(px, py, !fb.getPixel(px, py));
                    }
                }
            }
        }
        cx += OledFb::kFontWidth;
    }
}

void SceneRenderer::drawRowText(OledFb& fb, int row, const char* text, int maxChars) const {
    char buf[kTextMaxChars + 1];
    clipText(text, buf, sizeof(buf));
    if (maxChars > 0 && maxChars < kTextMaxChars) {
        buf[maxChars] = '\0';
    }
    fb.drawText(0, rowY(row), buf);
}

void SceneRenderer::drawCentered(OledFb& fb, int boxX, int boxY, int boxW,
                                 const char* text, bool inverted) const {
    if (text == nullptr) {
        return;
    }
    const int tw = static_cast<int>(std::strlen(text)) * OledFb::kFontWidth;
    const int x = boxX + ((boxW - tw) / 2);
    if (inverted) {
        drawTextXor(fb, x, boxY, text);
    } else {
        fb.drawText(x, boxY, text);
    }
}

void SceneRenderer::drawButton(OledFb& fb, int boxX, int boxW,
                               const display::SceneElement& e, int row) const {
    const bool pressed = e.state == display::SceneElementState::kPressed;
    drawBox(fb, boxX, rowY(row), boxW, kBoxH, pressed);
    char label[16];
    const char* tok = buttonLabel(e.text.data());
    if (e.state == display::SceneElementState::kFocused && !pressed) {
        std::snprintf(label, sizeof(label), ">%s", tok);
    } else {
        std::snprintf(label, sizeof(label), "%s", tok);
    }
    drawCentered(fb, boxX, rowY(row), boxW, label, pressed);
}

void SceneRenderer::render(const display::LogicalScene& scene, OledFb& fb) const {
    fb.clear();
    const PhysicalLayout layout = layoutFor(policy_);

    const display::SceneElement* title = scene.find(display::kSceneIdTitle);
    if (title != nullptr && title->visible) {
        char buf[kTextMaxChars + 1];
        clipText(title->text.data(), buf, sizeof(buf));
        drawRowText(fb, layout.rowTitle, buf, layout.textMaxChars);
    }

    const bool buttonsOnSameRow = layout.rowButtonA == layout.rowButtonB;
    const display::SceneElement* a = scene.find(display::kSceneIdButtonA);
    if (a != nullptr && a->visible) {
        drawButton(fb, 0, buttonsOnSameRow ? kButtonBoxW : OledFb::kWidth, *a,
                   layout.rowButtonA);
    }
    const display::SceneElement* b = scene.find(display::kSceneIdButtonB);
    if (b != nullptr && b->visible) {
        drawButton(fb, buttonsOnSameRow ? kButtonBoxW : 0,
                   buttonsOnSameRow ? kButtonBoxW : OledFb::kWidth, *b,
                   layout.rowButtonB);
    }

    const display::SceneElement* c = scene.find(display::kSceneIdCounter);
    if (c != nullptr && c->visible) {
        char buf[kTextMaxChars + 1];
        if (layout.compactPrefix) {
            compactPrefix(c->text.data(), buf, sizeof(buf));
        } else {
            clipText(c->text.data(), buf, sizeof(buf));
        }
        drawBox(fb, 0, rowY(layout.rowCounter), OledFb::kWidth, kBoxH, false);
        drawCentered(fb, 0, rowY(layout.rowCounter), OledFb::kWidth, buf, false);
    }

    const display::SceneElement* k = scene.find(display::kSceneIdKeyboard);
    const display::SceneElement* m = scene.find(display::kSceneIdMouse);
    const bool kVis = k != nullptr && k->visible;
    const bool mVis = m != nullptr && m->visible;

    auto rowText = [this, &layout](const char* raw, char* out) {
        if (layout.compactPrefix) {
            compactPrefix(raw, out, static_cast<size_t>(kTextMaxChars) + 1);
        } else {
            clipText(raw, out, static_cast<size_t>(kTextMaxChars) + 1);
        }
    };

    if (layout.rowKeyboard == layout.rowMouse) {
        // Ultra-compact：keyboard/mouse 合并一行。
        char kb[kTextMaxChars + 1];
        char ms[kTextMaxChars + 1];
        if (kVis) {
            rowText(k->text.data(), kb);
        } else {
            kb[0] = '\0';
        }
        if (mVis) {
            rowText(m->text.data(), ms);
        } else {
            ms[0] = '\0';
        }
        if (kb[0] != '\0' || ms[0] != '\0') {
            drawMergedRow(fb, layout.rowKeyboard, kb, ms, layout.textMaxChars);
        }
    } else {
        if (kVis) {
            char buf[kTextMaxChars + 1];
            rowText(k->text.data(), buf);
            drawRowText(fb, layout.rowKeyboard, buf, layout.textMaxChars);
        }
        if (mVis) {
            char buf[kTextMaxChars + 1];
            rowText(m->text.data(), buf);
            drawRowText(fb, layout.rowMouse, buf, layout.textMaxChars);
        }
    }
}

}  // namespace oled
}  // namespace espview