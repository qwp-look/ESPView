// ESPView — Stream Decoder（M0-B2）
//
// 规范来源：docs/DESIGN.md E 节「字节流解码状态机 / 三层概念」。
// Decoder 负责两层工作：
//   Layer 1：原始 byte stream → Packet（SYNC/HEADER/PAYLOAD/VERIFY 四态机）
//   Layer 2：Packet sequence → Message（IDLE / ASSEMBLING_CHUNKED）
// Decoder 不负责 Frame 语义：BEGIN/RECT/END 之间的完整性、丢帧、ABORTED 等
// 由后续 FrameAssembler（M0-C）处理，本文件一律不实现。
//
// 错误路径不使用异常；协议错误通过 ErrorCallback（可选）上报，供统计/诊断。
// 纯 C++17，零平台依赖（无 ESP-IDF / Qt / Windows API / 第三方库）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "message.h"
#include "packet.h"
#include "protocol.h"

namespace espview {
namespace proto {

// Message 级累积 payload 上限 = 正式协议常量 MAX_MESSAGE_PAYLOAD（DESIGN.md E 节，M0-C 冻结）。
// 1 MiB 仅是 wire-level 上限；Decoder 只累积（不预分配）payload，不产生固定 1 MiB 缓冲。

// 解码错误码（ErrorCallback 上报；kNeedMoreData 为正常等待态，不上报）。
enum class DecoderError : uint8_t {
    kNeedMoreData = 0,   // 数据不足，等待更多输入（正常状态，不通过回调上报）
    kBadMagic,           // 候选 MAGIC 不可信（HEADER 校验失败归类前的兜底）
    kBadVersion,         // HEADER.VERSION != 0x01
    kBadType,            // HEADER.TYPE 超出 0x01..0x51
    kBadLength,          // HEADER.LENGTH > 4096
    kCrcMismatch,        // CRC32 校验失败（整包丢弃，消息作废）
    kSequenceGap,        // packet.seq != expectedSeq（包丢弃，seq 基线重定位）
    kChunkTypeMismatch,  // 组装中收到 CHUNKED=1 但 TYPE 不同的续片
    kChunkViolation,     // 组装中收到 CHUNKED=0 的不同 TYPE 消息，或 FLAGS 与首包不一致
    kMessageTooLarge,    // 组装中的 Message payload 超过 maxMessagePayload
};

const char* toString(DecoderError e);

// 字节流 → Message 流解码器。
//
// 字节流状态机（DESIGN.md）：
//   SYNC（逐字节扫 'ESPV'）→ HEADER（20B，校验 VERSION/TYPE/LENGTH）
//   → PAYLOAD（读满 LENGTH 字节）→ VERIFY（CRC）→ 按 CHUNKED 组装 Message → SYNC
//
// 已固定的行为（与 DESIGN.md 一致，供测试与上层依赖）：
//   - 伪 MAGIC：HEADER 校验失败时丢弃候选 MAGIC 的首字节，从第二个字节继续搜索；
//   - 半包：滞留缓冲等待；M8-A1 起由 ProtocolEndpoint::tick() 驱动（上层每 100–200ms 调 tick；ESP32 sessionLoop / PC serial_worker），满足条件时调用 onTimeout() 强制回 SYNC；
//   - 粘包：单次 feed 内循环消费所有完整包；
//   - CRC 错误：整包（20B 头 + payload）丢弃，从包尾之后继续扫描；正在组装的
//     CHUNKED Message 作废；seq 基线重定位为失败包 seq+1（其头部语法已合法，
//     若头部本身被破坏，后续包的 seq gap 仍能自愈）；
//   - seq 跳变：当前包丢弃（不派发），正在组装的 Message 作废，seq 基线重定位为
//     收到的 seq+1（uint16 回绕）；不视为传输断开；
//   - HELLO 例外（M8-C C8）：HELLO 是会话起点，跳过 seq 检查并以本包 seq 重建基线
//     （DESIGN.md 握手双方 seq 清零）；保证断线后残留字节污染基线时，新会话 HELLO
//     仍能进入被动恢复路径；
//   - CHUNKED 组装：CHUNKED=1 开始/续接组装，CHUNKED=0 结束并派发；组装期间
//     TYPE 或 FLAGS（非 CHUNKED 位）不一致 → 当前消息作废，新包按消息边界处理
//     （CHUNKED=0 直接派发为新消息，CHUNKED=1 以新类型重新开始组装）；
//   - 派发的 Message.flags 恒为各包 FLAGS 去掉 CHUNKED 位（与 Encoder 对称，
//     保证 encode→decode 逐字节一致）。
class StreamDecoder {
public:
    using MessageCallback = std::function<void(const Message& message)>;
    using PacketCallback =
        std::function<void(const PacketHeader& header, const uint8_t* payload, size_t payloadLen)>;
    using ErrorCallback = std::function<void(DecoderError error)>;

    // onMessage：核心输出，每条完整 Message 组装完成后调用（含 CHUNKED=0 的普通消息）。
    // onPacket（可选）：每个通过 CRC 校验的 Packet 回调（含随后被 seq/组装规则丢弃的包）。
    // onError（可选）：协议错误诊断（kNeedMoreData 除外）。
    // maxMessagePayload：组装中的 Message 累计 payload 上限（默认 1 MiB）。
    explicit StreamDecoder(MessageCallback onMessage, PacketCallback onPacket = {},
                           ErrorCallback onError = {},
                           size_t maxMessagePayload = kMaxMessagePayload);

    // 输入任意长度字节：1 byte、半包、完整包、多包拼接均可，结果一致。
    // 注意：回调内不得重入调用 feed()/reset()/onTimeout()。
    void feed(const uint8_t* data, size_t size);
    void feed(const std::vector<uint8_t>& data) { feed(data.data(), data.size()); }

    // 复位：清空全部缓冲、回 SYNC、作废组装中的 Message，expectedSeq 回到 0。
    // 用于断线/重连/握手等场景（DESIGN.md：双方 packet.seq 清零）。
    void reset();

    // 仅把 seq 基线重定位为 0，不清缓冲、不回 SYNC、不中断组装。
    // 会话层在 HANDSHAKE 完成时调用（DESIGN.md：双方 packet.seq 清零），
    // 使对端握手后从 seq=0 开始的数据包不因基线错位被误判为 seq gap。
    // 与 reset() 的区别：可在 decoder 消息回调内安全调用（不触碰缓冲/状态机）。
    void resetSeqBaseline();

    // 半包滞留超时（M8-A1 起由 ProtocolEndpoint::tick() 驱动：上层每 100–200ms 调 tick，满足
    // bufferedBytes()>0 || assemblingMessage() 且距最近一次喂数据 ≥ 500ms 时调用）：
    // 丢弃全部滞留字节、回 SYNC、作废组装中的 Message；expectedSeq 保持不变。
    void onTimeout();

    // ---- 状态查询（测试/诊断用） ----
    size_t bufferedBytes() const;    // 当前未消费的滞留字节数
    bool assemblingMessage() const;  // 是否正在组装 CHUNKED Message
    uint16_t expectedSeq() const;    // 当前期望的下一包 SEQ

private:
    enum class State : uint8_t { kSync, kHeader, kPayload, kVerify };

    void process();               // 循环消费缓冲，直到需要更多数据
    void consume(size_t n);       // 丢弃缓冲前 n 字节（逻辑偏移，带批量压实）
    void report(DecoderError e);  // 经 onError 上报
    void abortMessage();          // 作废组装中的 Message
    void startAssembly(const uint8_t* payload);
    void handleVerifiedPacket();  // CRC/seq 通过后：组装或派发 Message

    MessageCallback onMessage_;
    PacketCallback onPacket_;
    ErrorCallback onError_;
    size_t maxMessagePayload_;

    std::vector<uint8_t> buf_;  // 未消费字节缓冲
    size_t start_ = 0;          // buf_ 中第一个未消费字节的下标
    State state_ = State::kSync;
    PacketHeader header_;       // 当前候选包的解析结果

    bool assembling_ = false;   // 正在组装 CHUNKED Message
    uint8_t currentType_ = 0;   // 组装中消息的 TYPE（首包锁定）
    uint8_t currentFlags_ = 0;  // 组装中消息的非 CHUNKED FLAGS（首包锁定）
    std::vector<uint8_t> currentPayload_;

    uint16_t expectedSeq_ = 0;  // 每方向独立；DESIGN.md：握手/重连后清零
};

}  // namespace proto
}  // namespace espview
