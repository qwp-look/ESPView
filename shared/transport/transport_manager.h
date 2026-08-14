// ESPView M6-C — TransportManager（shared/transport，纯 C++17）。
//
// 规范来源：M6-C 任务书 §三/§五/§七/§十/§十三。
//
// 职责：
//   - 持有当前 ITransport 实例（经 TransportFactory 创建，工厂注入配置）；
//   - select/switchTo：运行时在 UART/TCP 之间安全切换；
//   - open/close/state forwarding：把当前 Transport 的 data/state 回调转发给上层；
//   - 发送门（txMutex_）：serialize send 与 switch，杜绝切换期间 use-after-free；
//     上层 sink 必须经 lockTransport()/tryLockTransport() 访问当前 Transport。
//
// 不管理 Packet/Message/Frame（§三）；不实现协议会话重置——会话重置由上层在
//   收到 kDisconnected 状态回调时完成（ProtocolEndpoint.onTransportDisconnected()，
//   已含 decoder/frame/ACK/seq 清零；InputManager.resetState() 由应用层完成）。
//
// 切换语义（§五）：
//   switchTo() 全程持有 txMutex_：
//     1) 摘除旧 Transport 的 data/state 回调（防 stale）；
//     2) close 旧 Transport（其 Disconnected 状态被缓冲，不立即上抛）；
//     3) 经工厂创建新 Transport，挂回调，open（其 Connected 状态被缓冲）；
//     4) 释放锁后按顺序冲刷状态回调 → 上层按
//        Disconnected → Connected 顺序完成会话重置 + HELLO + FULL resync。
//   切换期间到达的 data 字节直接丢弃（毫秒级窗口；协议握手会恢复）。
//
// 线程模型：switchTo/open/close 由单一管理任务调用；send 由 TX/RX 任务经
//   lockTransport 访问；state/data 回调在 Transport 自己的线程触发。
// 错误路径不使用异常。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "transport.h"

namespace espview {
namespace transport {

class TransportManager {
public:
    // 创建具体 Transport（工厂持有配置；例如 ESP32 侧创建 UART/TCP adapter）。
    using TransportFactory = std::function<std::shared_ptr<ITransport>(TransportType type)>;

    TransportManager(TransportFactory factory, TransportType initial);
    ~TransportManager();

    TransportManager(const TransportManager&) = delete;
    TransportManager& operator=(const TransportManager&) = delete;
    // 不可移动/不可拷贝：内含 std::mutex（移动会 UB），且无处需要移动——
    // 构造全部就地完成（C++17 保证 prvalue 复制消除）。
    TransportManager(TransportManager&&) = delete;
    TransportManager& operator=(TransportManager&&) = delete;

    // ---- 生命周期 ----
    // open 当前已选 Transport；失败返回 false（状态回调 kError 已缓冲并冲刷）。
    bool open();
    // 关闭当前 Transport（幂等）；不改变已选类型。
    void close();
    // 运行时切换（§五）：close 旧 → 工厂创建新 → open 新。
    // 失败时保持 Disconnected（current() == nullptr），返回 false。
    bool switchTo(TransportType type);
    // 重新 open 当前已选类型（open 失败/切换失败后的重试入口）。
    bool reopen();

    // ---- 查询 ----
    TransportType current() const;
    bool isOpen() const;
    // 当前 Transport 指针（nullptr = 未 open/失败）。仅供统计/日志读取，
    // 发送必须经 lockTransport()/tryLockTransport()。
    ITransport* transport() const;
    const TransportCapabilities& capabilities() const;
    uint32_t switchCount() const { return switchCount_; }
    uint32_t switchFailures() const { return switchFailures_; }
    // 最近一次 open/switch 失败的简短描述（诊断用）。
    const char* lastError() const { return lastError_; }

    // ---- 回调 ----
    void setDataCallback(ITransport::DataCallback cb);
    void setStateCallback(ITransport::StateCallback cb);

    // ---- 发送门（上层 sink 专用）----
    // 阻塞获取当前 Transport（等待切换完成）；返回 nullptr = 未 open/失败。
    ITransport* lockTransport();
    // 非阻塞获取：切换中/门忙 → nullptr（调用方按背压处理，绝不阻塞）。
    ITransport* tryLockTransport();
    void unlockTransport();

private:
    void attachCallbacks(ITransport& t);
    void detachCallbacks(ITransport& t);
    bool createAndOpenLocked();                        // 已持 txMutex_：工厂创建 + open + 状态缓冲
    void closeLocked();                               // 已持 txMutex_：close + 状态缓冲
    void clearPending();                                // 丢弃旧 Transport 的 stale 状态（保留会话结束 Disconnected）
    void takePending(std::vector<ITransport::State>& out);
    void flushStates(const std::vector<ITransport::State>& states);
    void onTransportData(const uint8_t* data, size_t len);
    void onTransportState(ITransport::State s);
    void setLastError(const char* msg);

    TransportFactory factory_;
    TransportType initialType_;
    TransportType currentType_;
    std::shared_ptr<ITransport> current_;

    // txMutex_：发送门（sink 发送期间持有；switchTo 排他）。
    mutable std::mutex txMutex_;
    // stateMutex_：保护 currentType_/current_/isOpen_/回调。
    mutable std::mutex stateMutex_;

    ITransport::DataCallback dataCb_;
    ITransport::StateCallback stateCb_;
    std::atomic<bool> switching_{false};  // 切换窗口：丢弃 data / 缓冲 state
    std::mutex pendingMutex_;             // 保护 pendingStates_
    std::vector<ITransport::State> pendingStates_;

    bool open_ = false;
    uint32_t switchCount_ = 0;
    uint32_t switchFailures_ = 0;
    char lastError_[96] = "ok";
};

}  // namespace transport
}  // namespace espview
