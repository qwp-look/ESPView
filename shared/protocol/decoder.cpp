#include "decoder.h"

#include <cstddef>
#include <utility>

#include "crc32.h"

namespace espview {
namespace proto {

namespace {

// 缓冲批量压实阈值：start_ 超过该值时把已消费前缀从 buf_ 中擦除，
// 保证整体处理复杂度近似线性（每个字节最多被移动常数次）。
constexpr size_t kCompactionThreshold = 65536;

constexpr char kMagic0 = 'E';
constexpr char kMagic1 = 'S';
constexpr char kMagic2 = 'P';
constexpr char kMagic3 = 'V';

}  // namespace

const char* toString(DecoderError e) {
    switch (e) {
        case DecoderError::kNeedMoreData:
            return "kNeedMoreData";
        case DecoderError::kBadMagic:
            return "kBadMagic";
        case DecoderError::kBadVersion:
            return "kBadVersion";
        case DecoderError::kBadType:
            return "kBadType";
        case DecoderError::kBadLength:
            return "kBadLength";
        case DecoderError::kCrcMismatch:
            return "kCrcMismatch";
        case DecoderError::kSequenceGap:
            return "kSequenceGap";
        case DecoderError::kChunkTypeMismatch:
            return "kChunkTypeMismatch";
        case DecoderError::kChunkViolation:
            return "kChunkViolation";
        case DecoderError::kMessageTooLarge:
            return "kMessageTooLarge";
    }
    return "kUnknown";
}

StreamDecoder::StreamDecoder(MessageCallback onMessage, PacketCallback onPacket,
                             ErrorCallback onError, size_t maxMessagePayload)
    : onMessage_(std::move(onMessage)),
      onPacket_(std::move(onPacket)),
      onError_(std::move(onError)),
      maxMessagePayload_(maxMessagePayload) {}

void StreamDecoder::feed(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    buf_.insert(buf_.end(), data, data + size);
    process();
}

void StreamDecoder::reset() {
    buf_.clear();
    start_ = 0;
    state_ = State::kSync;
    expectedSeq_ = 0;
    abortMessage();
    header_ = PacketHeader{};
}

void StreamDecoder::resetSeqBaseline() {
    expectedSeq_ = 0;
}

void StreamDecoder::onTimeout() {
    // 半包滞留超时：滞留字节全部不可信，丢弃后回 SYNC；seq 基线保持不变。
    buf_.clear();
    start_ = 0;
    state_ = State::kSync;
    abortMessage();
    header_ = PacketHeader{};
}

size_t StreamDecoder::bufferedBytes() const { return buf_.size() - start_; }
bool StreamDecoder::assemblingMessage() const { return assembling_; }
uint16_t StreamDecoder::expectedSeq() const { return expectedSeq_; }

void StreamDecoder::consume(size_t n) {
    const size_t avail = buf_.size() - start_;
    if (n > avail) {
        n = avail;
    }
    start_ += n;
    if (start_ >= buf_.size()) {
        buf_.clear();
        start_ = 0;
    } else if (start_ >= kCompactionThreshold) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(start_));
        start_ = 0;
    }
}

void StreamDecoder::report(DecoderError e) {
    if (onError_) {
        onError_(e);
    }
}

void StreamDecoder::abortMessage() {
    assembling_ = false;
    currentType_ = 0;
    currentFlags_ = 0;
    currentPayload_.clear();
}

void StreamDecoder::startAssembly(const uint8_t* payload) {
    if (header_.length > maxMessagePayload_) {
        report(DecoderError::kMessageTooLarge);
        assembling_ = false;
        currentType_ = 0;
        currentFlags_ = 0;
        currentPayload_.clear();
        return;
    }
    assembling_ = true;
    currentType_ = header_.type;
    currentFlags_ = header_.flags & ~kFlagChunked;
    currentPayload_.assign(payload, payload + header_.length);
}

void StreamDecoder::handleVerifiedPacket() {
    const uint8_t* payload = buf_.data() + start_ + kPacketHeaderSize;
    const bool chunked = (header_.flags & kFlagChunked) != 0;
    const uint8_t logicalFlags = header_.flags & ~kFlagChunked;

    if (!assembling_) {
        // IDLE：CHUNKED=0 → 普通消息立即派发；CHUNKED=1 → 开始组装。
        if (!chunked) {
            Message msg;
            msg.type = header_.type;
            msg.flags = logicalFlags;
            msg.payload.assign(payload, payload + header_.length);
            if (onMessage_) {
                onMessage_(msg);
            }
        } else {
            startAssembly(payload);
        }
        return;
    }

    // ---- 正在组装 CHUNKED Message ----
    if (header_.type != currentType_ || logicalFlags != currentFlags_) {
        // 组装期间出现类型/标志不一致：当前消息作废（chunked 消息必须连续）。
        if (header_.type != currentType_ && chunked) {
            report(DecoderError::kChunkTypeMismatch);
        } else {
            report(DecoderError::kChunkViolation);
        }
        abortMessage();
        // 新包按消息边界处理：CHUNKED=0 派发为新消息，CHUNKED=1 以新类型重新组装。
        if (!chunked) {
            Message msg;
            msg.type = header_.type;
            msg.flags = logicalFlags;
            msg.payload.assign(payload, payload + header_.length);
            if (onMessage_) {
                onMessage_(msg);
            }
        } else {
            startAssembly(payload);
        }
        return;
    }

    // 类型与标志一致：追加载荷。
    if (currentPayload_.size() + header_.length > maxMessagePayload_) {
        report(DecoderError::kMessageTooLarge);
        abortMessage();
        // 本包作废、不派发；后续包从消息边界重新开始。
        return;
    }
    currentPayload_.insert(currentPayload_.end(), payload, payload + header_.length);

    if (!chunked) {
        // 末包（CHUNKED=0）：消息完成并派发。
        Message msg;
        msg.type = currentType_;
        msg.flags = currentFlags_;
        msg.payload = std::move(currentPayload_);
        if (onMessage_) {
            onMessage_(msg);
        }
        abortMessage();
    }
}

void StreamDecoder::process() {
    for (;;) {
        switch (state_) {
            case State::kSync: {
                const size_t avail = buf_.size() - start_;
                size_t i = 0;
                size_t keepFrom = avail;  // 默认：全部字节作为垃圾丢弃
                while (i < avail) {
                    if (buf_[start_ + i] != kMagic0) {
                        ++i;
                        continue;
                    }
                    // 候选 'E'：匹配 'ESPV' 前缀。
                    size_t matched = 1;
                    while (matched < 4 && i + matched < avail &&
                           buf_[start_ + i + matched] ==
                               "ESPV"[static_cast<size_t>(matched)]) {
                        ++matched;
                    }
                    if (i + matched == avail && matched < 4) {
                        // 缓冲尾部是可能的不完整 MAGIC：保留并从该处继续等待。
                        keepFrom = i;
                        break;
                    }
                    if (matched == 4) {
                        // 找到完整 MAGIC：丢弃其前的垃圾，进入 HEADER。
                        consume(i);
                        state_ = State::kHeader;
                        break;
                    }
                    // 伪 MAGIC：失配字节若又是 'E'，则跳至该字节，否则越过它。
                    const bool mismatchIsE = (buf_[start_ + i + matched] == kMagic0);
                    i += mismatchIsE ? matched : matched + 1;
                }
                if (state_ == State::kHeader) {
                    break;  // 进入 HEADER 继续外层循环
                }
                consume(keepFrom);
                return;  // 需要更多数据
            }

            case State::kHeader: {
                if (buf_.size() - start_ < kPacketHeaderSize) {
                    return;  // 半包头：等待更多字节
                }
                const PacketError err =
                    decodeHeader(buf_.data() + start_, kPacketHeaderSize, &header_);
                if (err != PacketError::kNone) {
                    // 候选 MAGIC 不可信：丢弃其首字节，从第二个字节继续搜索。
                    switch (err) {
                        case PacketError::kUnsupportedVersion:
                            report(DecoderError::kBadVersion);
                            break;
                        case PacketError::kInvalidType:
                            report(DecoderError::kBadType);
                            break;
                        case PacketError::kInvalidLength:
                            report(DecoderError::kBadLength);
                            break;
                        default:
                            report(DecoderError::kBadMagic);
                            break;
                    }
                    consume(1);
                    state_ = State::kSync;
                    break;  // 回 SYNC 继续外层循环
                }
                state_ = State::kPayload;
                break;
            }

            case State::kPayload: {
                if (buf_.size() - start_ < kPacketHeaderSize + header_.length) {
                    return;  // 半包载荷：等待更多字节
                }
                state_ = State::kVerify;
                break;
            }

            case State::kVerify: {
                const uint8_t* payload = buf_.data() + start_ + kPacketHeaderSize;
                const size_t packetSize = kPacketHeaderSize + header_.length;

                if (verifyPacketCrc(header_, payload, header_.length) != PacketError::kNone) {
                    // CRC 失败：整包不可信，丢弃整包并从包尾之后继续扫描；
                    // 正在组装的 CHUNKED Message 作废；seq 基线重定位为失败包 seq+1
                    // （头部语法已合法；若头部本身损坏，后续包 seq gap 仍可自愈）。
                    report(DecoderError::kCrcMismatch);
                    abortMessage();
                    expectedSeq_ = static_cast<uint16_t>(header_.seq + 1);
                    consume(packetSize);
                    state_ = State::kSync;
                    break;
                }

                if (onPacket_) {
                    onPacket_(header_, payload, header_.length);
                }

                if (header_.seq != expectedSeq_) {
                    // seq 跳变：当前包视为异常并丢弃，消息作废，基线重定位；
                    // 不视为传输断开（DESIGN.md / M0-B2 要求）。
                    report(DecoderError::kSequenceGap);
                    abortMessage();
                    expectedSeq_ = static_cast<uint16_t>(header_.seq + 1);
                    consume(packetSize);
                    state_ = State::kSync;
                    break;
                }
                expectedSeq_ = static_cast<uint16_t>(header_.seq + 1);

                // ---- Message 组装 / 派发 ----
                handleVerifiedPacket();

                consume(packetSize);
                state_ = State::kSync;
                break;
            }
        }
    }
}

}  // namespace proto
}  // namespace espview
