// ESPView M8-B (B1) - LogicalScene: logical display scene (pure C++17, zero platform deps).
//
// Spec: M8-B task book section 3/4/5/9/25 (Scene Tests).
// Target architecture: Application/LVGL -> Logical Display Scene ->
// VirtualRenderer (Qt) and PhysicalRenderer (OLED); both backends share the
// same Scene/Layout/State, NOT the same final pixels (Semantic Mirror).
//
// This file only expresses 'what the UI is':
//   - logical width/height (dynamic resolution; 320x240 is default, not law);
//   - element list (stable id / kind / logical bounds / visibility / z-order /
//     text / semantic state enabled|pressed|focused).
// It never expresses 'which framebuffer pixels': no Qt / QImage / LVGL /
// SSD1306 / I2C / ESP-IDF dependency.
//
// Fixed capacity (ESP32-friendly): kSceneMaxElements elements, each with
// kSceneTextCapacity text bytes; no heap allocation, no exceptions on error
// paths. Element id is the stable identity: the PhysicalRenderer compact
// layout maps by id/order, the Virtual side is the LVGL widget itself - this
// is the carrier of the 'shared Layout' rule.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace espview {
namespace display {

// Scene element kind (primitive type).
enum class SceneElementKind : uint8_t {
    kText = 0,    // text (title / status label)
    kButton = 1,  // clickable button (pressed/focused semantics)
    kPanel = 2,   // container (border + inner text)
};

// Semantic state (shared by Virtual/Physical backends).
enum class SceneElementState : uint8_t {
    kNormal = 0,  // regular
    kPressed = 1, // pressed / active (e.g. Button on state)
    kFocused = 2, // current focus (keyboard/wheel target)
};

// Logical rectangle (Logical Layout coordinates; dynamic-resolution space).
struct SceneRect {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
};

// Scene capacity and per-element text limit (fixed, no heap).
constexpr size_t kSceneMaxElements = 12;
constexpr size_t kSceneTextCapacity = 32;

// Stable element identity (demo app LVGL widgets; the PhysicalRenderer
// compact layout derives rows from these ids. New elements must register
// here so both backends share semantics).
enum SceneElementId : uint8_t {
    kSceneIdTitle = 1,
    kSceneIdButtonA = 2,
    kSceneIdButtonB = 3,
    kSceneIdCounter = 4,
    kSceneIdKeyboard = 5,
    kSceneIdMouse = 6,
    kSceneIdMax = 7,
};

struct SceneElement {
    uint8_t id = 0;  // SceneElementId
    SceneElementKind kind = SceneElementKind::kText;
    SceneElementState state = SceneElementState::kNormal;
    SceneRect bounds = {};  // logical coords (virtual resolution)
    bool visible = true;
    uint8_t zOrder = 0;
    std::array<char, kSceneTextCapacity> text{};  // NUL terminated
};

// Logical display scene (value object; builder fills, renderers read only).
class LogicalScene {
public:
    // Dynamic resolution (real logical size; set by builder after
    // HELLO/FRAME_BEGIN propagation).
    uint16_t logicalWidth = 320;
    uint16_t logicalHeight = 240;

    void clear() { count = 0; }

    // Append element; returns nullptr at capacity (caller skips).
    SceneElement* add(uint8_t id, SceneElementKind kind, int16_t x, int16_t y,
                      int16_t w, int16_t h) {
        if (count >= kSceneMaxElements) {
            return nullptr;
        }
        SceneElement& e = elements[count++];
        e = SceneElement{};
        e.id = id;
        e.kind = kind;
        e.bounds = SceneRect{x, y, w, h};
        return &e;
    }

    // Semantic text write (truncated to capacity-1, NUL guaranteed;
    // nullptr treated as empty string).
    static void setText(SceneElement& e, const char* s) {
        if (s == nullptr) {
            e.text[0] = '\0';
            return;
        }
        std::strncpy(e.text.data(), s, kSceneTextCapacity - 1);
        e.text[kSceneTextCapacity - 1] = '\0';
    }

    // Find by stable id (nullptr when absent).
    const SceneElement* find(uint8_t id) const {
        for (size_t i = 0; i < count; ++i) {
            if (elements[i].id == id) {
                return &elements[i];
            }
        }
        return nullptr;
    }
    SceneElement* find(uint8_t id) {
        return const_cast<SceneElement*>(
            static_cast<const LogicalScene*>(this)->find(id));
    }

    // Element order == layout order (shared by Virtual/Physical).
    std::array<SceneElement, kSceneMaxElements> elements{};
    size_t count = 0;
};

}  // namespace display
}  // namespace espview
