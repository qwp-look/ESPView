// ESPView M3 — 鼠标坐标映射（letterbox 等比例缩放，纯 C++17，可宿主测试）。
//
// 规范来源：spec §11/§12（鼠标坐标映射：找到 display rect → 去掉 letterbox 偏移
//   → 按比例缩放回逻辑分辨率 → clamp）。与 VirtualScreenWidget 的绘制规则
//   （Qt KeepAspectRatio + 居中）保持一致，保证「所见即所点」。
//
// 语义：
//   - 点落在 letterbox 黑边内（display rect 外）→ 返回 false（不发送，spec §12
//     推荐行为，不产生逻辑屏幕外坐标）；
//   - 点落在 display rect 内 → 缩放到逻辑坐标，结果恒在 0..displayW/H-1（本身
//     已 clamp：缩放向下取整，不会越界）。

#pragma once

#include <cstdint>

namespace espview {
namespace input {

class CoordinateMapper {
public:
    // widget 上一点 (wx,wy)（widget 尺寸 widgetW x widgetH）→ 逻辑显示坐标。
    // displayW/displayH = 逻辑分辨率（如 320x240）。返回 false = 在 letterbox 外。
    static bool mapPoint(int widgetX, int widgetY, int widgetW, int widgetH, int displayW,
                         int displayH, int& outX, int& outY);
};

}  // namespace input
}  // namespace espview
