// ESPView M8-B (B2) / M8-C (C3) - SceneRenderer: LogicalScene -> 128x64 mono OledFb.
//
// Spec: M8-B task book section 3/5/6/7/8/9/25 (Physical Renderer / Text
// Renderer / Responsive Physical Layout / Scene Tests) + M8-C §十五/§十七
// （文字必须直接绘制到 Mono1，位置来自 Logical Layout；Physical Layout
// Policy：Normal / Compact / UltraCompact，由 display capability / resolution /
// font metrics 决定）。
//
// 语义镜像（section 9）：Virtual 与 Physical 消费同一个 LogicalScene（相同
// 元素 / 顺序 / 文本 / 语义状态）。Physical 呈现是共享布局的紧凑变换：
//   row0  title
//   row1  [A] [B]（button boxes；pressed = 填充反色标签，focused = 前置 '>'）
//   row3  C:<counter>（panel box + 紧凑前缀）
//   row5  K:<keyboard>
//   row6  M:<mouse>
// 行号/前缀由 PhysicalLayoutPolicy 决定（layoutFor()，确定性；C3 起可测）。
// 文字直绘 Mono1（8x8 内置字体，从不 bitmap 缩小 —— M8-C §十五 根因修复），
// 渲染确定性、纯 C++17、零平台依赖、无堆分配。

#pragma once

#include <cstddef>
#include <cstdint>

#include "logical_scene.h"
#include "oled_fb.h"

namespace espview {
namespace oled {

// Physical Layout Policy（M8-C C3）：物理侧排版策略，由 display capability /
// 分辨率 / font metrics 决定。v0.1 三档（任务 §十七 示例 Normal/Compact/
// UltraCompact）：
//   kNormal       每元素一行、完整文本（128x64 下 6 行：title/A/B/counter/
//                 keyboard/mouse）；
//   kCompact      当前默认（M8-B B2 定稿）：[A][B] 同行 + 紧凑前缀
//                 （C:/K:/M:）；
//   kUltraCompact 更密：紧凑前缀 + keyboard/mouse 合并行（适合超小屏）。
// 选择入口：profile/pipeline factory 层（C3 先以编译期/构造期 policy 注入，
// 运行期热切换留给后续里程碑）。
enum class PhysicalLayoutPolicy : uint8_t {
    kNormal = 0,
    kCompact = 1,
    kUltraCompact = 2,
};

// 具体行布局（由 layoutFor(policy, fbW, fbH) 确定性计算；行 = 8px 字体行）。
struct PhysicalLayout {
    int rowTitle = 0;
    int rowButtonA = 0;
    int rowButtonB = 0;   // kNormal 时与 A 分行；Compact/UltraCompact 同行
    int rowCounter = 0;
    int rowKeyboard = 0;
    int rowMouse = 0;     // kUltraCompact 时与 keyboard 同行（前置 M:）
    int textMaxChars = 0; // 每行文本最大字符数（避免边缘 glyph 裁剪）
    bool compactPrefix = false;   // true = C:/K:/M: 紧凑前缀
};

// 计算指定 policy 的行布局（fbW/fbH 目前固定 128x64；行数由 font 高度推导，
// 非法几何回退 kCompact 语义）。确定性、无堆分配。
PhysicalLayout layoutFor(PhysicalLayoutPolicy policy, int fbW = OledFb::kWidth,
                         int fbH = OledFb::kHeight);

class SceneRenderer {
public:
    // 8x8 font grid on 128x64: 16 cols x 8 rows.
    static constexpr int kCols = OledFb::kWidth / OledFb::kFontWidth;    // 16
    static constexpr int kRows = OledFb::kHeight / OledFb::kFontHeight;  // 8
    // Max text chars per row (15*8 = 120px < 128px; avoids edge glyph clip).
    static constexpr int kTextMaxChars = kCols - 1;

    explicit SceneRenderer(PhysicalLayoutPolicy policy = PhysicalLayoutPolicy::kCompact)
        : policy_(policy) {}

    // Render the scene into fb (full redraw, deterministic).
    void render(const display::LogicalScene& scene, OledFb& fb) const;

    PhysicalLayoutPolicy policy() const { return policy_; }

    // ---- Presentation helpers (public for host tests / diagnostics) ----
    // Clip text to cap-1 chars (NUL terminated).
    static void clipText(const char* text, char* out, size_t cap);
    // "Button A" -> "A" (last whitespace token); no space -> whole text.
    static const char* buttonLabel(const char* text);
    // "Counter:" -> "C:", "Keyboard:" -> "K:", "Mouse:" -> "M:"; else as-is.
    static void compactPrefix(const char* full, char* out, size_t cap);

private:
    void drawButton(OledFb& fb, int boxX, int boxW, const display::SceneElement& e,
                    int row) const;
    void drawBox(OledFb& fb, int x, int y, int w, int h, bool fill) const;
    void drawRowText(OledFb& fb, int row, const char* text, int maxChars) const;
    void drawCentered(OledFb& fb, int boxX, int boxY, int boxW, const char* text,
                      bool inverted) const;
    // XOR glyph draw (black text on white fill / focus markers).
    void drawTextXor(OledFb& fb, int x, int y, const char* text) const;

    PhysicalLayoutPolicy policy_;
};

}  // namespace oled
}  // namespace espview
