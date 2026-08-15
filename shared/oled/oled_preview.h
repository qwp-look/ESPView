// ESPView M7-D2 — OledPreviewSlot：PHYSICAL_PREVIEW（TYPE 0x13）预览槽。
//
// 职责（DESIGN.md AE 节，M7-D2 已冻结）：
//   - 持有 1KB 最新帧快照槽（128x64 1bpp 页式，与 OledFb::data() 逐字节一致）
//     + frameId 计数器 + 有效标志；
//   - OLED 任务在内容确定点调用 store() 做 1KB 短拷贝（最新帧合并，覆盖旧槽）；
//     发送任务调用 snapshot() / makePhysicalPreviewPayload() 取帧并递增 frameId；
//     reset() 在握手/断线时清零。OLED 任务绝不触碰 transport（本模块只产出
//     字节，不含任何协议 builder/endpoint 依赖）。
//
// 线程模型（无锁，写者/读者/复位端分工）：
//   - 写侧：OLED 任务（唯一 store 调用者）；复位侧：发送任务（唯一 reset 调用者）；
//   - 读侧：发送任务（唯一 snapshot / makePhysicalPreviewPayload 调用者，因此
//     frameId_ 无需原子）；
//   - 同步：seqlock —— seq_ 偶数 = 槽稳定可读，奇数 = 写者进行中。写者先 seq++
//     （奇数，release）再逐字节 release 写槽，完成后 seq++（偶数，release 发布）；
//     读者 acquire 读 seq，奇数则重试，偶数则逐字节 acquire 复制，再读 seq 比对，
//     不一致（并发覆盖）则重试（有界 1000 次）。
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
#include <vector>

namespace espview {
namespace oled {

class OledPreviewSlot {
public:
    // AE.2 固定几何：128x64 Mono1。
    static constexpr uint16_t kWidth = 128;
    static constexpr uint16_t kHeight = 64;
    // pixelFormat 取值 1 = Mono1（局部常量；不新增协议枚举，protocol.h 不动）。
    static constexpr uint8_t kMono1 = 1;
    static constexpr size_t kSizeBytes = 1024;        // 槽/像素区字节数
    static constexpr size_t kPayloadSizeBytes = 1032; // 8B 头 + 1024B 像素

    OledPreviewSlot();

    // OLED 任务（内容确定点）调用：把最新 1KB 页式帧拷贝进槽（覆盖旧槽 =
    // 最新帧合并）。非阻塞：1024 次 release 原子写 + 2 次 seq 递增，无自旋、
    // 无锁、不触碰 transport。src1024 == nullptr 时 no-op。
    void store(const uint8_t* src1024);

    // 发送任务调用：把当前槽稳定副本拷到 out1024（1024B），成功返回 true 并
    // 递增 frameId（outFrameId = 递增前值；0..65535 回绕，见 AE.2）。槽无效
    // （从未 store 或已 reset）返回 false 且不拷贝、不递增。有界自旋（≤1000
    // 次重试），写者并发覆盖时自动重取最新稳定帧。
    bool snapshot(uint8_t* out1024, uint16_t& outFrameId);

    // 握手/断线时调用（发送任务）：清槽（全零）、有效标志清除、frameId 归零。
    // 与 store() 并发安全：以同一 seqlock 发布协议写入全零，读者不会读到半清
    // 状态；store() 紧随其后到达时槽回到有效（OLED 持续产帧，属预期）。
    void reset();

    // 按 AE.2 布局编码 1032B PHYSICAL_PREVIEW payload（多字节 LE）：
    //   [0..1] frameId u16 LE（取最新：内部稳定读后递增）
    //   [2..3] width u16 LE     [4..5] height u16 LE
    //   [6]    pixelFormat u8   [7] flags u8
    //   [8..]  pixels 1024B（与 OledFb::data() 逐字节一致）
    // 槽无效时返回空向量（调用方跳过发送；本模块无失败路径影响 Virtual
    // display —— 空 payload 即“无本帧”，不抛异常、不改任何其它状态）。
    std::vector<uint8_t> makePhysicalPreviewPayload(
        uint16_t width = kWidth, uint16_t height = kHeight,
        uint8_t pixelFormat = kMono1, uint8_t flags = 0);

    // 槽是否已至少发布过一次（发送侧可据此跳过空帧）。
    bool valid() const;

private:
    // seqlock 稳定读：复制当前槽到 out1024；返回 false = 槽无效或重试超限。
    bool copySlot(uint8_t* out1024) const;
    // seqlock 发布写：seq 置奇（release）→ 逐字节 release 写槽（src==nullptr
    // 时全零）→ seq 置偶（release）。写者路径无自旋。
    void publish(const uint8_t* src1024);

    std::atomic<uint32_t> seq_{0};   // 偶数 = 稳定，奇数 = 写者进行中
    std::atomic<bool> valid_{false}; // 至少一次 store 已发布
    // 发送任务独占（snapshot/payload/reset 同任务；OLED 任务不触碰）：
    uint16_t frameId_ = 0;
    std::atomic<uint8_t> slot_[kSizeBytes]{}; // 1KB 页式 1bpp 快照（原子字节防撕裂）
};

}  // namespace oled
}  // namespace espview
