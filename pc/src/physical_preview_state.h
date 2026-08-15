// ESPView M7-D2 — PhysicalPreviewState：物理 OLED 1bpp 预览帧快照模型
// （纯 C++17，零 Qt / 零协议 wire 依赖）。
//
// 规范来源：docs/DESIGN.md AE 节（M7-D2 PHYSICAL_PREVIEW 冻结）：
//   - TYPE 0x13，payload = frameId u16 + width u16 + height u16 +
//     pixelFormat u8(1=Mono1) + flags u8 + pixels[1024]（页式 1bpp），
//     共 1032B 单包，fire-and-forget；
//   - 帧去重/过期：`(int16_t)(frameId - lastFrameId) > 0` 才接受
//     （回绕安全），相等/过期丢弃；握手/重连清 lastFrameId 后首帧无条件接受；
//   - stale 判定：lastUpdate 距今 > staleMs（默认 1000ms，即 >1s）；
//   - 可用性：有像素且 sessionConnected；
//   - QSettings 映射：仅 `ui/previewEnabled` 键，无其他。
//
// 本文件只建模快照 + 可用性/stale/去重判定；不解析 wire（PHYSICAL_PREVIEW
// 消息解析由另一代理负责），不做任何 I/O。线程模型与 DisplayUiState/SplitState
// 一致：GUI 线程单线程使用，零锁。

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace espview {
namespace pc {

// 页式 1bpp 像素格式值（wire 对齐 AE.2 pixelFormat；HELLO 布局不改）。
constexpr uint8_t kPixelFormatMono1 = 1;

// 默认 stale 阈值：任务书要求 stale >1s（1000ms）。
constexpr uint64_t kDefaultPreviewStaleMs = 1000;

// QSettings 键：preview enabled（唯一持久化键；键名含 ui/ 前缀，与
// main.cpp 的 persistedSettingsKeys 白名单风格一致）。
constexpr const char* kPreviewEnabledSettingsKey = "ui/previewEnabled";

// M7-D2 — 物理预览帧快照模型（纯 C++17）。消费者（PhysicalPreviewWidget）
// 只读本快照；生产者（后续 PHYSICAL_PREVIEW 解析器）经 setFrame 写入。
class PhysicalPreviewState {
public:
    PhysicalPreviewState() = default;

    // ---- 快照字段（只读访问）----
    uint16_t frameId() const { return frameId_; }
    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }
    uint8_t pixelFormat() const { return pixelFormat_; }
    const std::vector<uint8_t>& pixels() const { return pixels_; }
    uint64_t lastUpdateMs() const { return lastUpdateMs_; }
    bool sessionConnected() const { return sessionConnected_; }
    uint64_t staleThresholdMs() const { return staleThresholdMs_; }

    // ---- 会话生命周期 ----
    // connected=true：会话建立/重连，清 lastFrameId 语义 → 首帧无条件接受
    // （AE.3：握手时双方清零；断线重连 PC 清 lastFrameId 后首帧无条件接受）。
    void setSessionConnected(bool connected);
    // 断线：清空像素/时间戳/去重基（AE.3：PC 断线清空预览位图）。
    void onDisconnected();

    // ---- 帧写入 ----
    // nowMs：调用方时钟（建议单调 ms）。接受新帧返回 true 并更新快照+时间戳。
    // 拒绝条件（返回 false，状态不变）：
    //   - 几何/像素非法：width/height 为 0，或 pixels 不足
    //     ceil(width*height/8) 字节（1bpp 页式）；
    //   - 帧 id 相等/过期/乱序（`(int16_t)(frameId - lastFrameId) > 0`
    //     才接受，回绕安全）。
    bool setFrame(uint16_t frameId, uint16_t width, uint16_t height,
                  uint8_t pixelFormat, const std::vector<uint8_t>& pixels,
                  uint64_t nowMs);

    // ---- 派生判定 ----
    // 有像素且 sessionConnected。
    bool isAvailable() const;
    // 可用且 (nowMs - lastUpdateMs) > staleThresholdMs（严格大于，>1s 才 stale）。
    // 时钟回退（nowMs < lastUpdateMs）防御返回 false。
    bool isStale(uint64_t nowMs) const;
    // 覆盖默认阈值（测试/产品侧注入；0 表示任意间隔都 stale）。
    void setStaleThresholdMs(uint64_t ms) { staleThresholdMs_ = ms; }

    // ---- preview enabled（QSettings 映射：仅 ui/previewEnabled 键）----
    // 默认 true（AE.5：Preview 使能由 PC 侧 UI 控制）。
    bool previewEnabled() const { return previewEnabled_; }
    void setPreviewEnabled(bool enabled) { previewEnabled_ = enabled; }
    // 导出恰一个键：{ui/previewEnabled: "1"/"0"}；无其他键。
    std::map<std::string, std::string> toSettingsMap() const;
    // 只读 ui/previewEnabled（接受 "1"/"0"/"true"/"false"，大小写不敏感；
    // 未知值/缺键忽略并保持当前值）；其余键一律忽略。
    void fromSettingsMap(const std::map<std::string, std::string>& map);

private:
    // 去重判定：当前会话未接受过帧 → 无条件接受；否则按回绕安全差值。
    bool acceptFrameId(uint16_t id) const;

    uint16_t frameId_ = 0;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint8_t pixelFormat_ = 0;
    std::vector<uint8_t> pixels_;
    uint64_t lastUpdateMs_ = 0;
    uint64_t staleThresholdMs_ = kDefaultPreviewStaleMs;
    bool sessionConnected_ = false;
    bool receivedAnyFrame_ = false;  // 当前会话是否已接受过帧（去重基）
    bool previewEnabled_ = true;
};

}  // namespace pc
}  // namespace espview
