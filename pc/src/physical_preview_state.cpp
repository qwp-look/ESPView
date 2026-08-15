// ESPView M7-D2 — PhysicalPreviewState 实现（纯 C++17，零 Qt / 零协议 wire 依赖）。
// 规范来源见 physical_preview_state.h（docs/DESIGN.md AE 节冻结）。

#include "physical_preview_state.h"

#include <algorithm>
#include <cctype>

namespace espview {
namespace pc {

namespace {

// 1bpp 页式所需字节数：ceil(width*height/8)。
size_t neededPixelBytes(uint16_t width, uint16_t height) {
    return (static_cast<size_t>(width) * height + 7u) / 8u;
}

// 解析 "1"/"true"（大小写不敏感）→ true；"0"/"false" → false；其余 → 未定义。
bool parseEnabledValue(const std::string& value, bool& out) {
    std::string lower = value;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower == "1" || lower == "true") {
        out = true;
        return true;
    }
    if (lower == "0" || lower == "false") {
        out = false;
        return true;
    }
    return false;
}

}  // namespace

void PhysicalPreviewState::setSessionConnected(bool connected) {
    sessionConnected_ = connected;
    // 会话建立/重连：清 lastFrameId 语义 → 下一帧无条件接受（AE.3）。
    receivedAnyFrame_ = false;
}

void PhysicalPreviewState::onDisconnected() {
    sessionConnected_ = false;
    pixels_.clear();
    lastUpdateMs_ = 0;
    frameId_ = 0;
    width_ = 0;
    height_ = 0;
    pixelFormat_ = 0;
    receivedAnyFrame_ = false;  // 重连后首帧无条件接受
}

bool PhysicalPreviewState::acceptFrameId(uint16_t id) const {
    if (!receivedAnyFrame_) {
        return true;  // 会话首帧（含重连后首帧）无条件接受
    }
    // 回绕安全差：先按 u16 模运算，再位解释为 int16（补码）；>0 才新。
    const int16_t diff =
        static_cast<int16_t>(static_cast<uint16_t>(id - frameId_));
    return diff > 0;
}

bool PhysicalPreviewState::setFrame(uint16_t frameId, uint16_t width,
                                    uint16_t height, uint8_t pixelFormat,
                                    const std::vector<uint8_t>& pixels,
                                    uint64_t nowMs) {
    if (width == 0 || height == 0) {
        return false;  // 几何非法（防御）
    }
    if (pixels.size() < neededPixelBytes(width, height)) {
        return false;  // 像素不足（防御；wire 恒 1024B = 128x64）
    }
    if (!acceptFrameId(frameId)) {
        return false;  // 相等/过期/乱序（回绕安全）
    }
    frameId_ = frameId;
    width_ = width;
    height_ = height;
    pixelFormat_ = pixelFormat;
    pixels_ = pixels;
    lastUpdateMs_ = nowMs;
    receivedAnyFrame_ = true;
    return true;
}

bool PhysicalPreviewState::isAvailable() const {
    return !pixels_.empty() && sessionConnected_;
}

bool PhysicalPreviewState::isStale(uint64_t nowMs) const {
    if (!isAvailable() || nowMs < lastUpdateMs_) {
        return false;  // 无数据/未连接/时钟回退防御
    }
    return (nowMs - lastUpdateMs_) > staleThresholdMs_;  // 严格大于（>1s）
}

std::map<std::string, std::string> PhysicalPreviewState::toSettingsMap() const {
    std::map<std::string, std::string> map;
    map[kPreviewEnabledSettingsKey] = previewEnabled_ ? "1" : "0";
    return map;  // 恰一个键，无其他
}

void PhysicalPreviewState::fromSettingsMap(
    const std::map<std::string, std::string>& map) {
    const auto it = map.find(kPreviewEnabledSettingsKey);
    if (it == map.end()) {
        return;  // 缺键 → 保持当前值
    }
    bool enabled = false;
    if (parseEnabledValue(it->second, enabled)) {
        previewEnabled_ = enabled;
    }
    // 其余键一律忽略（本模型只认识 ui/previewEnabled）。
}

}  // namespace pc
}  // namespace espview
