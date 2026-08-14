// ESPView M3 — CoordinateMapper 实现（见 coordinate_mapper.h）。
// 与 Qt::KeepAspectRatio 语义一致（scale = min(widgetW/displayW, widgetH/displayH)，
// 结果居中），全部用整数/有理数运算（无浮点、无 Qt 依赖），widget 可任意
// 大于或小于逻辑分辨率。

#include "coordinate_mapper.h"

namespace espview {
namespace input {

bool CoordinateMapper::mapPoint(int widgetX, int widgetY, int widgetW, int widgetH,
                                int displayW, int displayH, int& outX, int& outY) {
    if (widgetW <= 0 || widgetH <= 0 || displayW <= 0 || displayH <= 0) {
        return false;
    }
    using i64 = long long;
    // 比例比较（避免浮点）：宽度受限 ⇔ widgetW/displayW <= widgetH/displayH
    //   ⇔ widgetW * displayH <= widgetH * displayW。
    int dstW = 0;
    int dstH = 0;
    if (static_cast<i64>(widgetW) * displayH <= static_cast<i64>(widgetH) * displayW) {
        dstW = widgetW;
        dstH = static_cast<int>(static_cast<i64>(displayH) * widgetW / displayW);
    } else {
        dstH = widgetH;
        dstW = static_cast<int>(static_cast<i64>(displayW) * widgetH / displayH);
    }
    if (dstW <= 0 || dstH <= 0) {
        return false;  // 极端小 widget：无法形成有效显示区
    }
    // 居中约定与 Qt QRect::moveCenter 一致：中心对齐（widget/2 - dst/2），
    // 避免 (w-d)/2 向下取整与 Qt 中心对齐差 1px。
    const int dstX = widgetW / 2 - dstW / 2;
    const int dstY = widgetH / 2 - dstH / 2;

    if (widgetX < dstX || widgetX >= dstX + dstW || widgetY < dstY ||
        widgetY >= dstY + dstH) {
        return false;  // letterbox 黑边内：不发送
    }

    // 逻辑坐标 = (widget 内偏移) * 分辨率 / dst 尺寸；向下取整，恒在
    // 0..display-1（结果不会越界，等于 clamp）。
    outX = static_cast<int>(static_cast<i64>(widgetX - dstX) * displayW / dstW);
    outY = static_cast<int>(static_cast<i64>(widgetY - dstY) * displayH / dstH);
    return true;
}

}  // namespace input
}  // namespace espview
