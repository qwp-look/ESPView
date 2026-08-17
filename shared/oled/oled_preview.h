// ESPView M7-D2 — OledPreviewSlot：PHYSICAL_PREVIEW（TYPE 0x13）预览槽。
//
// 职责（DESIGN.md AE 节，M7-D2 已冻结）：
//   - 持有 1KB 最新帧快照槽（128x64 1bpp 页式，与 OledFb::data() 逐字节一致）
//     + frameId 计数器 + 有效标志；
//   - OLED 任务在内容确定点调用 store() 做 1KB 短拷贝（最新帧合并，覆盖旧槽）；
//     发送任务调用 snapshot() 取帧并递增 frameId；reset() 在握手/断线时清零。
//     OLED 任务绝不触碰 transport（本模块只产出字节，不含任何协议
//     builder/endpoint 依赖）。
//   - M8-A4 单编码路径：AE.2 payload 布局（1032B：frameId/width/height/
//     pixelFormat/flags + 1024B 像素）的唯一编码器是 shared/protocol
//     makePhysicalPreview（message.h/message.cpp）；本槽不再自编码 AE.2，
//     main.cpp 经 snapshot() 取字节 + frameId 后直接调用 sendPhysicalPreview
//     （消除 encode→parse→re-encode 往返与双编码器漂移）。
//
// 线程模型（M8-A4：写者互斥 + 读者 seqlock）：
//   - 写侧：store() 来自 OLED 任务；reset() 来自发送任务/会话状态回调
//     （reset 可能运行于 transport RX 任务，见 main.cpp onSessionState）。
//     store 与 reset 是双写者：M8-A4 前两者并发会撕裂 seqlock 奇偶序列
//     （Agent C/K 竞态）——现在 store/reset 共用 writeMutex_ 串行化；
//   - 读侧：发送任务 snapshot()（唯一读者；frameId_ 用 relaxed fetch_add
//     计数，无顺序依赖；读者保持无锁）；
//   - 同步：写者先取 writeMutex_，再走 seqlock 发布（seq_ 偶数 = 槽稳定可读，
//     奇数 = 写者进行中）。写者先 seq++（奇数，release）再逐字节 release 写槽，
//     完成后 seq++（偶数，release 发布）；读者 acquire 读 seq，奇数则重试，
//     偶数则逐字节 acquire 复制，再读 seq 比对，不一致（并发覆盖）则重试
//     （有界 1000 次）。
//   - 正确性：槽字节为 std::atomic<uint8_t>，且写侧 release / 读侧 acquire ——
//     若读者观察到某字节来自未完成发布批次，happens-before 链强制其后续 seq
//     读看到写者奇数标记而重试，杜绝撕裂帧；也无 C++ 数据竞争（不依赖 memcpy
//     与普通数组并发的 UB）。
//   - 实时性：写者从不自旋等待（1024 次 release store + 2 次 seq 递增），保持
//     OLED 任务非阻塞；读者最多有界自旋，2–10Hz 发布率下几乎从不重试；重试
//     超限丢弃本帧（fire-and-forget，与 AE.3 背压整帧丢弃一致）。
//
// 纯 C++17、零平台依赖（host 测试与 ESP32 侧共用同一份源码）。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "oled_geometry.h"  // M8-A4：几何唯一事实来源

namespace espview {
namespace oled {

class OledPreviewSlot {
public:
    // AE.2 固定几何：128x64 Mono1（像素区字节数与 protocol 的
    // kPhysicalPreviewPixelBytes 一致；AE.2 布局归 protocol 层，本类不重复）。
    // M8-A4：几何数值来自 shared/oled oled_geometry.h（单一来源）。
    static constexpr uint16_t kWidth = static_cast<uint16_t>(kDefaultOledGeometry.width);
    static constexpr uint16_t kHeight = static_cast<uint16_t>(kDefaultOledGeometry.height);
    static constexpr size_t kSizeBytes = kDefaultOledGeometry.sizeBytes;  // 槽/像素区字节数

    OledPreviewSlot();

    // OLED 任务（内容确定点）调用：把最新 1KB 页式帧拷贝进槽（覆盖旧槽 =
    // 最新帧合并）。非阻塞：1024 次 release 原子写 + 2 次 seq 递增，无自旋、
    // 无锁、不触碰 transport。src1024 == nullptr 时 no-op。
    void store(const uint8_t* src1024);

    // 发送任务调用：把当前槽稳定副本拷到 out1024（1024B），成功返回 true 并
    // 递增 frameId（outFrameId = 递增前值；0..65535 回绕，见 AE.2）。槽无效
    // （从未 store 或已 reset）返回 false 且不拷贝、不递增。有界自旋（≤1000
    // 次重试），写者并发覆盖时自动重取最新稳定帧。像素字节即 AE.2 的 pixels
    // 区；frameId/width/height/format 由调用方经 protocol 层编码（单路径）。
    bool snapshot(uint8_t* out1024, uint16_t& outFrameId);

    // 握手/断线时调用（发送任务）：清槽（全零）、有效标志清除、frameId 归零。
    // 与 store() 并发安全：以同一 seqlock 发布协议写入全零，读者不会读到半清
    // 状态；store() 紧随其后到达时槽回到有效（OLED 持续产帧，属预期）。
    void reset();

    // 槽是否已至少发布过一次（发送侧可据此跳过空帧）。
    bool valid() const;

private:
    // seqlock 稳定读：复制当前槽到 out1024；返回 false = 槽无效或重试超限。
    bool copySlot(uint8_t* out1024) const;
    // seqlock 发布写：seq 置奇（release）→ 逐字节 release 写槽（src==nullptr
    // 时全零）→ seq 置偶（release）。写者路径无自旋。
    void publish(const uint8_t* src1024);

    // M8-A4：store/reset 双写者互斥（OLED 任务 vs 会话状态回调任务）。
    // 读者 snapshot() 仍无锁（seqlock）。持有 writeMutex_ 的路径不得调用
    // snapshot/valid（避免死锁）；写路径绝不触碰 transport。
    mutable std::mutex writeMutex_;
    std::atomic<uint32_t> seq_{0};   // 偶数 = 稳定，奇数 = 写者进行中
    std::atomic<bool> valid_{false}; // 至少一次 store 已发布
    // M7-F：reset 可能经会话状态回调在 transport RX 任务执行，与发送任务
    // （snapshot）并发 → 必须原子（relaxed 计数即可）。
    std::atomic<uint16_t> frameId_{0};
    std::atomic<uint8_t> slot_[kSizeBytes]{}; // 1KB 页式 1bpp 快照（原子字节防撕裂）
};

}  // namespace oled
}  // namespace espview

