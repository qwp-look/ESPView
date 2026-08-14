// ESPView M3 — InputController（PC Qt 事件 → InputEvent 的适配/策略层）。
//
// 规范来源：spec §9/§11/§13/§14/§15 + docs/DESIGN.md G 节。
// 职责（GUI 线程独占）：
//   - 接收 VirtualScreenWidget 已映射好的逻辑坐标事件（Qt 键码/掩码由
//     qt_key_adapter 转换；鼠标坐标已由 widget 的 CoordinateMapper 映射，
//     letterbox 外的事件不会传入）；
//   - 键盘：Qt::Key → HostKey → USB HID usage（KeyboardMapper）；autoRepeat
//     KeyDown 忽略（InputPolicy，spec §9），KeyUp 始终发送；
//   - 鼠标：按钮状态一致性（控制器维护当前掩码，spec §14）；MouseMove ≤60Hz
//     限频 + coalesce（MouseMoveThrottle，spec §13）；Wheel angleDelta/120 归一化
//     + clamp（spec §15）；
//   - 统一出口：构造 InputEvent（timestampMs=本地单调毫秒，不上 wire）→
//     sendFn（由 MainWindow 接 SerialWorker 跨线程队列，GUI 线程绝不碰串口）。
//
// 边界：不做协议编码（那是 input_codec + MessageEncoder 的事）、不做重传/ACK、
// 不维护 ESP32 侧输入状态（那是 InputManager 的事）。

#pragma once

#include <cstdint>
#include <functional>

#include <QObject>
#include <QTimer>

#include "input_event.h"
#include "input_policy.h"

namespace espview {
namespace pc {

class InputController : public QObject {
    Q_OBJECT
public:
    // 发送回调（GUI 线程调用；实现方保证线程安全，如 SerialWorker 队列）。
    using SendFn = std::function<void(const espview::input::InputEvent&)>;

    explicit InputController(QObject* parent = nullptr);

    void setSendFn(SendFn fn);
    // 显示分辨率（来自 HELLO / committed frame；用于鼠标坐标防御性 clamp）。
    void setDisplaySize(uint16_t width, uint16_t height);

    // ---- VirtualScreenWidget 事件入口（逻辑坐标已映射；无效事件不传入）----
    // x/y 为逻辑显示坐标（0..width-1 / 0..height-1）；buttonsMask 为按住掩码。
    void onMouseMove(int x, int y, uint8_t buttonsMask);
    // buttonBit = kMouseLeft/Right/Middle（本次 press/release 的按钮）。
    void onMousePress(uint8_t buttonBit, int x, int y, uint16_t modifiersMask);
    void onMouseRelease(uint8_t buttonBit, int x, int y, uint16_t modifiersMask);
    // angleDeltaY = Qt wheelEvent angleDelta().y()（120 = 一格）。
    void onWheel(int angleDeltaY, int x, int y, uint8_t buttonsMask, uint16_t modifiersMask);
    // qtKey = Qt::Key 数值（事件源已转换）；autoRepeat 来自 Qt（spec §9）。
    void onKeyPress(int qtKey, uint16_t modifiersMask, bool autoRepeat);
    void onKeyRelease(int qtKey, uint16_t modifiersMask);

    // 统计（状态栏展示；计数器不回绕）。
    struct Stats {
        uint64_t keysSent = 0;
        uint64_t mouseSent = 0;
        uint64_t unsupported = 0;      // 未映射 Qt 键 → 不发送
        uint64_t ignoredAutoRepeat = 0;
        uint64_t moveThrottled = 0;    // 被 60Hz 限频合并的 Move 数
        uint64_t dropped = 0;          // 发送回调层拒绝（如未连接）
    };
    const Stats& stats() const { return stats_; }

    // 供 16ms 定时器与 Qt 测试直接调用：补发最后一批被限频的 Move。
    void flushPendingMoves();

private:
    void send(const espview::input::InputEvent& e);
    uint16_t clampX(int x) const;
    uint16_t clampY(int y) const;

    SendFn sendFn_;
    uint16_t width_ = 320;
    uint16_t height_ = 240;
    uint8_t lastButtons_ = 0;  // 控制器维护的当前按钮掩码（spec §14 状态一致性）
    espview::input::MouseMoveThrottle moveThrottle_;
    QTimer* flushTimer_ = nullptr;
    Stats stats_;
};

}  // namespace pc
}  // namespace espview
