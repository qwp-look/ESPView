// ESPView M8-B (B2) - SceneRenderer implementation (see scene_renderer.h).

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

}  // namespace

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

void SceneRenderer::drawRowText(OledFb& fb, int row, const char* text) const {
    char buf[kTextMaxChars + 1];
    clipText(text, buf, sizeof(buf));
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
                               const display::SceneElement& e) const {
    const bool pressed = e.state == display::SceneElementState::kPressed;
    drawBox(fb, boxX, rowY(kRowButtons), boxW, kBoxH, pressed);
    char label[16];
    const char* tok = buttonLabel(e.text.data());
    if (e.state == display::SceneElementState::kFocused && !pressed) {
        std::snprintf(label, sizeof(label), ">%s", tok);
    } else {
        std::snprintf(label, sizeof(label), "%s", tok);
    }
    drawCentered(fb, boxX, rowY(kRowButtons), boxW, label, pressed);
}

void SceneRenderer::render(const display::LogicalScene& scene, OledFb& fb) const {
    fb.clear();

    const display::SceneElement* title = scene.find(display::kSceneIdTitle);
    if (title != nullptr && title->visible) {
        char buf[kTextMaxChars + 1];
        clipText(title->text.data(), buf, sizeof(buf));
        drawRowText(fb, kRowTitle, buf);
    }

    const display::SceneElement* a = scene.find(display::kSceneIdButtonA);
    if (a != nullptr && a->visible) {
        drawButton(fb, 0, kButtonBoxW, *a);
    }
    const display::SceneElement* b = scene.find(display::kSceneIdButtonB);
    if (b != nullptr && b->visible) {
        drawButton(fb, kButtonBoxW, kButtonBoxW, *b);
    }

    const display::SceneElement* c = scene.find(display::kSceneIdCounter);
    if (c != nullptr && c->visible) {
        char buf[kTextMaxChars + 1];
        compactPrefix(c->text.data(), buf, sizeof(buf));
        drawBox(fb, 0, rowY(kRowCounter), OledFb::kWidth, kBoxH, false);
        drawCentered(fb, 0, rowY(kRowCounter), OledFb::kWidth, buf, false);
    }

    const display::SceneElement* k = scene.find(display::kSceneIdKeyboard);
    if (k != nullptr && k->visible) {
        char buf[kTextMaxChars + 1];
        compactPrefix(k->text.data(), buf, sizeof(buf));
        drawRowText(fb, kRowKeyboard, buf);
    }

    const display::SceneElement* m = scene.find(display::kSceneIdMouse);
    if (m != nullptr && m->visible) {
        char buf[kTextMaxChars + 1];
        compactPrefix(m->text.data(), buf, sizeof(buf));
        drawRowText(fb, kRowMouse, buf);
    }
}

}  // namespace oled
}  // namespace espview
