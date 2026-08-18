// ESPView M8-B (B1) - LogicalScene Host Tests
//
// Spec: M8-B task book section 4/5/9/25 (Scene Tests: element layout /
// text layout / alignment / responsive layout / dynamic resolution /
// semantic state). Pure host, zero platform deps; follows test_util.h style.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "logical_scene.h"
#include "test_util.h"

namespace {

using espview::display::kSceneIdButtonA;
using espview::display::kSceneIdButtonB;
using espview::display::kSceneIdCounter;
using espview::display::kSceneIdKeyboard;
using espview::display::kSceneIdMouse;
using espview::display::kSceneIdTitle;
using espview::display::kSceneMaxElements;
using espview::display::kSceneTextCapacity;
using espview::display::LogicalScene;
using espview::display::SceneElement;
using espview::display::SceneElementKind;
using espview::display::SceneElementState;

// Build a full demo scene (same element set as LvglDemoApp::buildScene).
void buildDemoScene(LogicalScene& s) {
    s.clear();
    s.logicalWidth = 320;
    s.logicalHeight = 240;
    SceneElement* title = s.add(kSceneIdTitle, SceneElementKind::kText, 0, 8, 320, 16);
    LogicalScene::setText(*title, "ESPView LVGL Input Test");
    SceneElement* a = s.add(kSceneIdButtonA, SceneElementKind::kButton, 55, 65, 110, 40);
    LogicalScene::setText(*a, "Button A");
    SceneElement* b = s.add(kSceneIdButtonB, SceneElementKind::kButton, 185, 65, 110, 40);
    LogicalScene::setText(*b, "Button B");
    SceneElement* c = s.add(kSceneIdCounter, SceneElementKind::kPanel, 80, 130, 160, 40);
    LogicalScene::setText(*c, "Counter: 42");
    SceneElement* k = s.add(kSceneIdKeyboard, SceneElementKind::kText, 0, 185, 320, 16);
    LogicalScene::setText(*k, "Keyboard: B");
    SceneElement* m = s.add(kSceneIdMouse, SceneElementKind::kText, 0, 210, 320, 16);
    LogicalScene::setText(*m, "Mouse: 120,80");
}

void test_clear_and_add() {
    LogicalScene s;
    CHECK_EQ(s.count, size_t{0});
    SceneElement* e = s.add(kSceneIdTitle, SceneElementKind::kText, 1, 2, 3, 4);
    CHECK(e != nullptr);
    CHECK_EQ(s.count, size_t{1});
    CHECK_EQ(e->id, kSceneIdTitle);
    CHECK_EQ(e->kind, SceneElementKind::kText);
    CHECK_EQ(e->state, SceneElementState::kNormal);
    CHECK(e->visible);
    CHECK_EQ(e->bounds.x, 1);
    CHECK_EQ(e->bounds.y, 2);
    CHECK_EQ(e->bounds.w, 3);
    CHECK_EQ(e->bounds.h, 4);
    s.clear();
    CHECK_EQ(s.count, size_t{0});
}

void test_capacity_overflow() {
    LogicalScene s;
    for (size_t i = 0; i < kSceneMaxElements; ++i) {
        CHECK(s.add(static_cast<uint8_t>(i + 1), SceneElementKind::kText, 0, 0, 1, 1) != nullptr);
    }
    CHECK_EQ(s.count, kSceneMaxElements);
    // Overflow: returns nullptr, count unchanged.
    CHECK(s.add(200, SceneElementKind::kText, 0, 0, 1, 1) == nullptr);
    CHECK_EQ(s.count, kSceneMaxElements);
}

void test_set_text() {
    LogicalScene s;
    SceneElement* e = s.add(kSceneIdTitle, SceneElementKind::kText, 0, 0, 1, 1);
    LogicalScene::setText(*e, "hello");
    CHECK(std::strcmp(e->text.data(), "hello") == 0);
    LogicalScene::setText(*e, nullptr);
    CHECK_EQ(e->text[0], '\0');
}

void test_set_text_truncation() {
    LogicalScene s;
    SceneElement* e = s.add(kSceneIdTitle, SceneElementKind::kText, 0, 0, 1, 1);
    char longText[kSceneTextCapacity + 16];
    std::memset(longText, 'x', sizeof(longText) - 1);
    longText[sizeof(longText) - 1] = '\0';
    LogicalScene::setText(*e, longText);
    CHECK_EQ(e->text[kSceneTextCapacity - 1], '\0');
    CHECK_EQ(std::strlen(e->text.data()), kSceneTextCapacity - 1);
}

void test_find_by_id() {
    LogicalScene s;
    buildDemoScene(s);
    const SceneElement* t = s.find(kSceneIdTitle);
    CHECK(t != nullptr);
    CHECK_EQ(t->id, kSceneIdTitle);
    CHECK(std::strcmp(t->text.data(), "ESPView LVGL Input Test") == 0);
    const SceneElement* c = s.find(kSceneIdCounter);
    CHECK(c != nullptr);
    CHECK_EQ(c->kind, SceneElementKind::kPanel);
    CHECK(std::strcmp(c->text.data(), "Counter: 42") == 0);
    CHECK(s.find(99) == nullptr);
}

void test_element_order_preserved() {
    LogicalScene s;
    buildDemoScene(s);
    CHECK_EQ(s.count, size_t{6});
    // Order == layout order (title -> A -> B -> counter -> keyboard -> mouse).
    const uint8_t expected[kSceneMaxElements] = {
        kSceneIdTitle, kSceneIdButtonA, kSceneIdButtonB, kSceneIdCounter,
        kSceneIdKeyboard, kSceneIdMouse, 0, 0, 0, 0, 0, 0,
    };
    for (size_t i = 0; i < s.count; ++i) {
        CHECK_EQ(s.elements[i].id, expected[i]);
    }
}

void test_dynamic_resolution() {
    LogicalScene s;
    CHECK_EQ(s.logicalWidth, uint16_t{320});  // default resolution (allowed)
    CHECK_EQ(s.logicalHeight, uint16_t{240});
    s.logicalWidth = 128;
    s.logicalHeight = 128;
    CHECK_EQ(s.logicalWidth, uint16_t{128});
    CHECK_EQ(s.logicalHeight, uint16_t{128});
}

void test_semantic_state() {
    LogicalScene s;
    buildDemoScene(s);
    SceneElement* a = s.find(kSceneIdButtonA);
    SceneElement* b = s.find(kSceneIdButtonB);
    a->state = SceneElementState::kPressed;
    b->state = SceneElementState::kFocused;
    CHECK_EQ(a->state, SceneElementState::kPressed);
    CHECK_EQ(b->state, SceneElementState::kFocused);
    CHECK_EQ(s.find(kSceneIdKeyboard)->state, SceneElementState::kNormal);
}

void test_zorder_and_visibility() {
    LogicalScene s;
    buildDemoScene(s);
    s.find(kSceneIdMouse)->visible = false;
    s.find(kSceneIdCounter)->zOrder = 5;
    CHECK(!s.find(kSceneIdMouse)->visible);
    CHECK_EQ(s.find(kSceneIdCounter)->zOrder, uint8_t{5});
}

}  // namespace

void runLogicalSceneTests() {
    std::printf("[logical_scene]\n");
    test_clear_and_add();
    test_capacity_overflow();
    test_set_text();
    test_set_text_truncation();
    test_find_by_id();
    test_element_order_preserved();
    test_dynamic_resolution();
    test_semantic_state();
    test_zorder_and_visibility();
    std::printf("[logical_scene] all passed\n");
}
