// ESPView M8-B (B2) - SceneRenderer Host Tests
//
// Spec: M8-B task book section 25 (Physical Renderer Tests: mono output /
// text glyph / button / scale geometry / no crop text loss / deterministic)
// + section 9 (semantic mirror). Pure host, zero platform deps.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "logical_scene.h"
#include "oled_fb.h"
#include "scene_renderer.h"
#include "test_util.h"

namespace {

using espview::display::kSceneIdButtonA;
using espview::display::kSceneIdButtonB;
using espview::display::kSceneIdCounter;
using espview::display::kSceneIdKeyboard;
using espview::display::kSceneIdMouse;
using espview::display::kSceneIdTitle;
using espview::display::LogicalScene;
using espview::display::SceneElement;
using espview::display::SceneElementKind;
using espview::display::SceneElementState;
using espview::oled::OledFb;
using espview::oled::SceneRenderer;

constexpr int kFbSize = static_cast<int>(OledFb::kSizeBytes);

void buildDemoScene(LogicalScene& s, const char* counter = "Counter: 42",
                    const char* key = "Keyboard: B",
                    const char* mouse = "Mouse: 120,80") {
    s.clear();
    s.logicalWidth = 320;
    s.logicalHeight = 240;
    SceneElement* t = s.add(kSceneIdTitle, SceneElementKind::kText, 0, 8, 320, 16);
    LogicalScene::setText(*t, "ESPView LVGL Input Test");
    SceneElement* a = s.add(kSceneIdButtonA, SceneElementKind::kButton, 55, 65, 110, 40);
    LogicalScene::setText(*a, "Button A");
    SceneElement* b = s.add(kSceneIdButtonB, SceneElementKind::kButton, 185, 65, 110, 40);
    LogicalScene::setText(*b, "Button B");
    SceneElement* c = s.add(kSceneIdCounter, SceneElementKind::kPanel, 80, 130, 160, 40);
    LogicalScene::setText(*c, counter);
    SceneElement* k = s.add(kSceneIdKeyboard, SceneElementKind::kText, 0, 185, 320, 16);
    LogicalScene::setText(*k, key);
    SceneElement* m = s.add(kSceneIdMouse, SceneElementKind::kText, 0, 210, 320, 16);
    LogicalScene::setText(*m, mouse);
}

bool fbEqual(const OledFb& a, const OledFb& b) {
    return std::memcmp(a.data(), b.data(), OledFb::kSizeBytes) == 0;
}

void test_empty_scene_clears() {
    SceneRenderer r;
    OledFb fb;
    fb.fill(true);
    LogicalScene s;  // count == 0
    r.render(s, fb);
    for (int i = 0; i < kFbSize; ++i) {
        CHECK_EQ(fb.data()[i], uint8_t{0});
    }
}

void test_title_glyph_rendered() {
    SceneRenderer r;
    OledFb fb;
    LogicalScene s;
    buildDemoScene(s);
    r.render(s, fb);
    // 'E' glyph at x=0..7, y=0..7 (row0).
    const uint8_t* g = OledFb::fontGlyph('E');
    for (int rr = 0; rr < OledFb::kFontHeight; ++rr) {
        for (int col = 0; col < OledFb::kFontWidth; ++col) {
            const bool expect = ((g[rr] >> col) & 1u) != 0;
            CHECK_EQ(fb.getPixel(col, rr), expect);
        }
    }
}

void test_title_clipped_to_15_chars() {
    SceneRenderer r;
    OledFb fb;
    LogicalScene s;
    buildDemoScene(s);
    r.render(s, fb);
    // "ESPView LVGL Input Test": 15th char index 14 = 'p'? verify char 16 absent:
    // chars 0..14 drawn (x 0..119); x=120..127 must be empty on row0.
    for (int x = 120; x < OledFb::kWidth; ++x) {
        for (int y = 0; y < OledFb::kFontHeight; ++y) {
            CHECK(!fb.getPixel(x, y));
        }
    }
}

void test_button_pressed_filled_inverted() {
    SceneRenderer r;
    OledFb fb;
    LogicalScene s;
    buildDemoScene(s);
    s.find(kSceneIdButtonA)->state = SceneElementState::kPressed;
    r.render(s, fb);
    // Box A filled: corner and interior pixels ON.
    CHECK(fb.getPixel(0, 8));
    CHECK(fb.getPixel(1, 9));
    // Label 'A' centered in box A (box x=0..63, w=64): x = (64-8)/2 = 28.
    // Inverted on fill: glyph pixel OFF.
    const uint8_t* g = OledFb::fontGlyph('A');
    for (int rr = 0; rr < OledFb::kFontHeight; ++rr) {
        for (int col = 0; col < OledFb::kFontWidth; ++col) {
            if ((g[rr] >> col) & 1u) {
                CHECK(!fb.getPixel(28 + col, 8 + rr));
            }
        }
    }
}

void test_button_focused_marker() {
    SceneRenderer r;
    OledFb a, b;
    LogicalScene s1, s2;
    buildDemoScene(s1);
    buildDemoScene(s2);
    s2.find(kSceneIdButtonB)->state = SceneElementState::kFocused;
    r.render(s1, a);
    r.render(s2, b);
    // Focused scene must differ; difference only inside box B region (x 64..127).
    bool diffInsideB = false;
    bool diffOutside = false;
    for (int y = 0; y < OledFb::kHeight; ++y) {
        for (int x = 0; x < OledFb::kWidth; ++x) {
            const bool same = a.getPixel(x, y) == b.getPixel(x, y);
            if (!same) {
                if (x >= 64 && y >= 8 && y < 16) {
                    diffInsideB = true;
                } else {
                    diffOutside = true;
                }
            }
        }
    }
    CHECK(diffInsideB);
    CHECK(!diffOutside);
}

void test_compact_prefix_rules() {
    char out[32];
    SceneRenderer::compactPrefix("Counter: 42", out, sizeof(out));
    CHECK(std::strcmp(out, "C: 42") == 0);
    SceneRenderer::compactPrefix("Keyboard: B", out, sizeof(out));
    CHECK(std::strcmp(out, "K: B") == 0);
    SceneRenderer::compactPrefix("Mouse: 120,80", out, sizeof(out));
    CHECK(std::strcmp(out, "M: 120,80") == 0);
    SceneRenderer::compactPrefix("Other Text", out, sizeof(out));
    CHECK(std::strcmp(out, "Other Text") == 0);
    SceneRenderer::compactPrefix("", out, sizeof(out));
    CHECK(std::strcmp(out, "") == 0);
}

void test_button_label_last_token() {
    CHECK(std::strcmp(SceneRenderer::buttonLabel("Button A"), "A") == 0);
    CHECK(std::strcmp(SceneRenderer::buttonLabel("Button B"), "B") == 0);
    CHECK(std::strcmp(SceneRenderer::buttonLabel("OK"), "OK") == 0);
    CHECK(std::strcmp(SceneRenderer::buttonLabel(""), "?") == 0);
    CHECK(std::strcmp(SceneRenderer::buttonLabel(nullptr), "?") == 0);
}

void test_clip_text() {
    char out[32];
    SceneRenderer::clipText("abcdefghijklmnopqrstuvwxyz", out, sizeof(out));
    CHECK_EQ(std::strlen(out), size_t{15});
    CHECK(std::strcmp(out, "abcdefghijklmno") == 0);
    SceneRenderer::clipText("short", out, sizeof(out));
    CHECK(std::strcmp(out, "short") == 0);
}

void test_counter_and_rows() {
    SceneRenderer r;
    OledFb fb;
    LogicalScene s;
    buildDemoScene(s);
    r.render(s, fb);
    // Counter panel outline at row3: corner pixel ON.
    CHECK(fb.getPixel(0, 24));
    // Keyboard row5 first char 'K' present (some pixel set).
    bool keyAny = false;
    for (int y = 40; y < 48; ++y) {
        for (int x = 0; x < 8; ++x) {
            keyAny = keyAny || fb.getPixel(x, y);
        }
    }
    CHECK(keyAny);
    // Mouse row6 first char 'M' present.
    bool mouseAny = false;
    for (int y = 48; y < 56; ++y) {
        for (int x = 0; x < 8; ++x) {
            mouseAny = mouseAny || fb.getPixel(x, y);
        }
    }
    CHECK(mouseAny);
}

void test_invisible_element_skipped() {
    SceneRenderer r;
    OledFb fb;
    LogicalScene s;
    buildDemoScene(s);
    s.find(kSceneIdMouse)->visible = false;
    r.render(s, fb);
    bool mouseAny = false;
    for (int y = 48; y < 56; ++y) {
        for (int x = 0; x < OledFb::kWidth; ++x) {
            mouseAny = mouseAny || fb.getPixel(x, y);
        }
    }
    CHECK(!mouseAny);
}

void test_deterministic() {
    SceneRenderer r;
    OledFb a, b;
    LogicalScene s;
    buildDemoScene(s);
    r.render(s, a);
    r.render(s, b);
    CHECK(fbEqual(a, b));
}

void test_resolution_independent_physical() {
    // Dynamic resolution must not change the compact physical output.
    SceneRenderer r;
    OledFb a, b;
    LogicalScene s1, s2;
    buildDemoScene(s1);
    buildDemoScene(s2);
    s1.logicalWidth = 320;
    s1.logicalHeight = 240;
    s2.logicalWidth = 640;
    s2.logicalHeight = 480;
    r.render(s1, a);
    r.render(s2, b);
    CHECK(fbEqual(a, b));
}

void test_semantic_state_propagates() {
    // Same scene, counter value differs -> fb differs (semantic consistency).
    SceneRenderer r;
    OledFb a, b;
    LogicalScene s1, s2;
    buildDemoScene(s1, "Counter: 1", "Keyboard: NONE", "Mouse: 0,0");
    buildDemoScene(s2, "Counter: 2", "Keyboard: NONE", "Mouse: 0,0");
    r.render(s1, a);
    r.render(s2, b);
    CHECK(!fbEqual(a, b));
}

// ---- M8-C (C3): Physical Layout Policy tests ----

void test_layout_for_policies() {
    using espview::oled::layoutFor;
    using espview::oled::PhysicalLayoutPolicy;

    const auto normal = layoutFor(PhysicalLayoutPolicy::kNormal);
    CHECK_EQ(normal.rowTitle, 0);
    CHECK_EQ(normal.rowButtonA, 1);
    CHECK_EQ(normal.rowButtonB, 2);
    CHECK_EQ(normal.rowCounter, 3);
    CHECK_EQ(normal.rowKeyboard, 4);
    CHECK_EQ(normal.rowMouse, 5);
    CHECK_EQ(normal.textMaxChars, SceneRenderer::kTextMaxChars);
    CHECK(!normal.compactPrefix);

    const auto compact = layoutFor(PhysicalLayoutPolicy::kCompact);
    CHECK_EQ(compact.rowTitle, 0);
    CHECK_EQ(compact.rowButtonA, 1);
    CHECK_EQ(compact.rowButtonB, 1);
    CHECK_EQ(compact.rowCounter, 3);
    CHECK_EQ(compact.rowKeyboard, 5);
    CHECK_EQ(compact.rowMouse, 6);
    CHECK_EQ(compact.textMaxChars, SceneRenderer::kTextMaxChars);
    CHECK(compact.compactPrefix);

    const auto ultra = layoutFor(PhysicalLayoutPolicy::kUltraCompact);
    CHECK_EQ(ultra.rowTitle, 0);
    CHECK_EQ(ultra.rowButtonA, 1);
    CHECK_EQ(ultra.rowButtonB, 1);
    CHECK_EQ(ultra.rowCounter, 2);
    CHECK_EQ(ultra.rowKeyboard, 3);
    CHECK_EQ(ultra.rowMouse, 3);
    CHECK_EQ(ultra.textMaxChars, SceneRenderer::kTextMaxChars);
    CHECK(ultra.compactPrefix);
}

void test_layout_invalid_geometry_falls_back_to_compact() {
    using espview::oled::layoutFor;
    using espview::oled::PhysicalLayoutPolicy;
    const auto fallback = layoutFor(PhysicalLayoutPolicy::kNormal, 64, 32);
    const auto compact = layoutFor(PhysicalLayoutPolicy::kCompact, 128, 64);
    CHECK_EQ(fallback.rowTitle, compact.rowTitle);
    CHECK_EQ(fallback.rowButtonA, compact.rowButtonA);
    CHECK_EQ(fallback.rowButtonB, compact.rowButtonB);
    CHECK_EQ(fallback.rowCounter, compact.rowCounter);
    CHECK_EQ(fallback.rowKeyboard, compact.rowKeyboard);
    CHECK_EQ(fallback.rowMouse, compact.rowMouse);
    CHECK_EQ(fallback.textMaxChars, compact.textMaxChars);
    CHECK_EQ(fallback.compactPrefix, compact.compactPrefix);
}

void test_render_normal_rows_full_text() {
    SceneRenderer r(espview::oled::PhysicalLayoutPolicy::kNormal);
    OledFb fb;
    LogicalScene s;
    buildDemoScene(s);
    r.render(s, fb);
    // Title row0 'E' glyph unchanged.
    const uint8_t* g = OledFb::fontGlyph('E');
    for (int rr = 0; rr < OledFb::kFontHeight; ++rr) {
        for (int col = 0; col < OledFb::kFontWidth; ++col) {
            CHECK_EQ(fb.getPixel(col, rr), ((g[rr] >> col) & 1u) != 0);
        }
    }
    // Buttons on separate rows: A box corner row1, B box corner row2.
    CHECK(fb.getPixel(0, 8));
    CHECK(fb.getPixel(0, 16));
    // Counter panel outline row3.
    CHECK(fb.getPixel(0, 24));
    // Keyboard row4 (y=32..39): full text "Keyboard: B" -> second glyph 'e'
    // (compact "K: B" would show ':' here), proving no compact prefix.
    const uint8_t* eg = OledFb::fontGlyph('e');
    for (int rr = 0; rr < OledFb::kFontHeight; ++rr) {
        for (int col = 0; col < OledFb::kFontWidth; ++col) {
            CHECK_EQ(fb.getPixel(8 + col, 32 + rr), ((eg[rr] >> col) & 1u) != 0);
        }
    }
    // Mouse row5 (y=40..47): 'M' glyph at x0.
    const uint8_t* mg = OledFb::fontGlyph('M');
    for (int rr = 0; rr < OledFb::kFontHeight; ++rr) {
        for (int col = 0; col < OledFb::kFontWidth; ++col) {
            CHECK_EQ(fb.getPixel(col, 40 + rr), ((mg[rr] >> col) & 1u) != 0);
        }
    }
}

void test_render_ultra_compact_merged_row() {
    SceneRenderer r(espview::oled::PhysicalLayoutPolicy::kUltraCompact);
    OledFb fb;
    LogicalScene s;
    buildDemoScene(s);
    r.render(s, fb);
    // K/M merged on row3 (y=24..31): 'K' at x0.
    const uint8_t* kg = OledFb::fontGlyph('K');
    for (int rr = 0; rr < OledFb::kFontHeight; ++rr) {
        for (int col = 0; col < OledFb::kFontWidth; ++col) {
            CHECK_EQ(fb.getPixel(col, 24 + rr), ((kg[rr] >> col) & 1u) != 0);
        }
    }
    // 'M' glyph present later on the same row (x 8..127).
    const uint8_t* mg = OledFb::fontGlyph('M');
    bool found = false;
    for (int x = 8; x + OledFb::kFontWidth <= OledFb::kWidth && !found; ++x) {
        bool match = true;
        for (int rr = 0; rr < OledFb::kFontHeight && match; ++rr) {
            for (int col = 0; col < OledFb::kFontWidth && match; ++col) {
                if (fb.getPixel(x + col, 24 + rr) != (((mg[rr] >> col) & 1u) != 0)) {
                    match = false;
                }
            }
        }
        if (match) {
            found = true;
        }
    }
    CHECK(found);
}

void test_policies_deterministic_and_semantic() {
    for (int p = 0; p < 3; ++p) {
        const auto policy = static_cast<espview::oled::PhysicalLayoutPolicy>(p);
        SceneRenderer r(policy);
        OledFb a, b;
        LogicalScene s1, s2;
        buildDemoScene(s1, "Counter: 1", "Keyboard: NONE", "Mouse: 0,0");
        buildDemoScene(s2, "Counter: 2", "Keyboard: NONE", "Mouse: 0,0");
        r.render(s1, a);
        r.render(s1, b);
        CHECK(fbEqual(a, b));   // deterministic per policy
        r.render(s2, b);
        CHECK(!fbEqual(a, b));  // semantic state propagates per policy
    }
}

void test_policy_getter() {
    using espview::oled::PhysicalLayoutPolicy;
    SceneRenderer def;
    CHECK(def.policy() == PhysicalLayoutPolicy::kCompact);
    SceneRenderer normal(PhysicalLayoutPolicy::kNormal);
    CHECK(normal.policy() == PhysicalLayoutPolicy::kNormal);
    SceneRenderer ultra(PhysicalLayoutPolicy::kUltraCompact);
    CHECK(ultra.policy() == PhysicalLayoutPolicy::kUltraCompact);
}

}  // namespace

void runSceneRendererTests() {
    std::printf("[scene_renderer]\n");
    test_empty_scene_clears();
    test_title_glyph_rendered();
    test_title_clipped_to_15_chars();
    test_button_pressed_filled_inverted();
    test_button_focused_marker();
    test_compact_prefix_rules();
    test_button_label_last_token();
    test_clip_text();
    test_counter_and_rows();
    test_invisible_element_skipped();
    test_deterministic();
    test_resolution_independent_physical();
    test_semantic_state_propagates();
    test_layout_for_policies();
    test_layout_invalid_geometry_falls_back_to_compact();
    test_render_normal_rows_full_text();
    test_render_ultra_compact_merged_row();
    test_policies_deterministic_and_semantic();
    test_policy_getter();
    std::printf("[scene_renderer] all passed\n");
}