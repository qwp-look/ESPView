// ESPView — Display 抽象层（M5-A）
//
// 规范来源：docs/DESIGN.md D 节（IDisplay / DisplayManager）+ M5-A 任务书。
// 纯 C++17，零平台依赖（无 ESP-IDF / Qt / LVGL / Windows API）。
//
// 与 DESIGN.md D.1 的差异（M5-A 实现语义，详见 docs/DESIGN.md §31）：
//   - IDisplay 方法返回平台无关的 display::DisplayStatus（而非 esp_err_t）；
//     语义等价（kOk == 0 == ESP_OK；其余为负值错误码），ESP32 适配层负责日志映射。
//
// 边界（DESIGN.md J 节）：本层不持有整屏 framebuffer；像素只经 writeRect()
// 汇入有界 staging 队列（packet-sized staging + bounded TX queue + rect
// metadata），所有权始终归 Application/LVGL。

#pragma once

#include <cstdint>

#include "protocol.h"  // proto::PixelFormat / proto::FrameType

namespace espview {
namespace display {

// 显示后端模式（DESIGN.md D.2）。v0.1 只实现编译期 WINDOW 模式；
// DEVICE / MIRROR 仅保留接口（M5-A 不做 runtime DisplayMode）。
enum class DisplayMode : uint8_t {
    kWindow = 0,  // RemoteDisplay（M5-A）
    kDevice = 1,  // HardwareDisplay（未来，stub）
    kMirror = 2,  // MirrorDisplay（未来，stub）
};

// 平台无关状态码。0 = OK（与 esp_err_t 的 ESP_OK==0 约定一致）；负值 = 错误。
enum class DisplayStatus : int32_t {
    kOk = 0,
    kInvalidParam = -1,   // 越界 / 格式 / 长度非法
    kRectTooLarge = -2,   // 矩形像素超出单个 staging 槽容量
    kQueueFull = -3,      // 背压：TX 队列满（调用方可等待后重试，超时后 dropPendingFrame）
    kFrameBusy = -4,      // 上一帧尚未结束（END 未发出），新帧暂时无法开始
    kFrameAborted = -5,   // 当前帧已作废（本矩形被丢弃；TX 将发送 ABORTED END）
    kNotEnabled = -6,
    kNotConnected = -7,
    kNotSupported = -8,
    kInternal = -9,
};

// 显示配置（DESIGN.md D.1 DisplayConfig；协议只约束 width/height 1..4096）。
struct DisplayConfig {
    int width = 320;
    int height = 240;
    proto::PixelFormat format = proto::PixelFormat::kRgb565;
};

// 只读显示信息（DESIGN.md D.1 DisplayInfo）。
struct DisplayInfo {
    int width = 0;
    int height = 0;
    proto::PixelFormat format = proto::PixelFormat::kRgb565;
    bool supportsDirtyRect = true;
};

// 统一显示后端抽象（DESIGN.md D.1）。Application 只面向本接口，
// 不出现 if (pc_mode) / if (lcd_mode) 分支；具体后端经 DisplayManager 注入。
class IDisplay {
public:
    virtual ~IDisplay() = default;
    virtual DisplayStatus init(const DisplayConfig& cfg) = 0;
    virtual const DisplayInfo& info() const = 0;
    // 汇入一个矩形（内存中已有数据）。调用方保证 pixels 在调用期间有效；
    // 实现必须同步完成必要 staging，不得持有 pixels 指针。
    virtual DisplayStatus writeRect(int x, int y, int w, int h,
                                    const uint8_t* pixels) = 0;
    // 提交当前帧（LVGL is_last / rendering cycle 结束）。
    virtual DisplayStatus flush() = 0;
    virtual DisplayStatus setEnabled(bool enabled) = 0;
};

}  // namespace display
}  // namespace espview
