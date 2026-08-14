// ESPView M2 — DisplayFrame：Qt 边界的显示快照结构（纯数据，非协议实现）。
//
// 规范来源：docs/DESIGN.md M2 节（Qt 只接收 CommittedFrame / DisplayFrame）。
// DisplayFrame 是 FrameAssembler 提交帧在 PC 侧的镜像快照（PC framebuffer 只是
// 「最后一次成功提交的镜像」，不是第二个权威状态）。
//
// 数据流：SerialWorker（Worker 线程）在 onFrameCommit 回调里把 CommittedFrame
// 拷贝成 DisplayFrame（含每个 RECT 的像素副本），通过 Qt queued signal 投递给
// GUI 线程；VirtualScreenWidget 只消费本结构。
//
// 字段故意使用 plain uint8_t / uint16_t / uint32_t，不透出协议枚举：
//   frameType:   0 = FULL，1 = PARTIAL（与 shared/protocol FrameType 数值一致）
//   pixelFormat: 0 = RGB565（v0.1 唯一格式）
// 每个 RECT 的像素为 RGB565 little-endian（低字节在前），w*h*2 字节。
// 本文件不依赖 shared/protocol 实现（Widget 不知道 Packet/CRC/CHUNKED/SEQ）。

#pragma once

#include <cstdint>
#include <vector>

#include <QMetaType>

namespace espview {
namespace pc {

struct DisplayRect {
    uint16_t x = 0;  // 矩形左上角逻辑坐标（framebuffer 像素）
    uint16_t y = 0;
    uint16_t w = 0;  // 矩形宽（像素）
    uint16_t h = 0;
    std::vector<uint8_t> pixels;  // RGB565 LE，w*h*2 字节
};

struct DisplayFrame {
    uint16_t frameId = 0;
    uint8_t frameType = 0;    // 0 = FULL，1 = PARTIAL
    uint8_t pixelFormat = 0;  // 0 = RGB565
    uint16_t width = 0;       // 逻辑分辨率宽（ESP32 TestPattern = 320）
    uint16_t height = 0;      // 逻辑分辨率高（240）
    uint16_t rectCount = 0;   // 实际 FRAME_RECT 数
    uint32_t byteCount = 0;   // 像素总字节数
    std::vector<DisplayRect> rects;
};

}  // namespace pc
}  // namespace espview

Q_DECLARE_METATYPE(espview::pc::DisplayRect)
Q_DECLARE_METATYPE(espview::pc::DisplayFrame)
