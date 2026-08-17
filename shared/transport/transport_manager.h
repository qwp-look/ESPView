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

// M7-B：Transport 诊断快照（OLED provider / statsLoop 专用，值语义）。
struct TransportDiagSnapshot {
    TransportType type = TransportType::kUart;
    bool connected = false;
    uint64_t reconnectCount = 0;
    uint64_t txBytes = 0;
    uint64_t rxBytes = 0;
    int8_t rssi = -128;
    uint8_t channel = 0;
};


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
    // M8-A5：析构专用收尾 —— 先 detach 上层回调再关闭 Transport，
    // 保证析构路径绝不触发 state/data 回调（上层可能已先销毁）。
    void shutdown();
    // 运行时切换（§五）：close 旧 → 工厂创建新 → open 新。
    // 失败时保持 Disconnected（current() == nullptr），返回 false。
    bool switchTo(TransportType type);
    // 重新 open 当前已选类型（open 失败/切换失败后的重试入口）。
    bool reopen();
    // M8-A3（PC TCP Server accept 路径）：adopt 一个已激活的 Transport。
    // 与 open()/switchTo() 不同：transport 已由调用方 open（HostTcpTransport
    // 已 attach accepted socket），本方法只挂回调并设为当前 Transport，
    // 不调用 open()。要求当前未 open（已 open 返回 false，不做隐式替换），
    // 且 t->isConnected() 必须为 true（未连接 → 拒绝；M8-A3 C/E）。
    // 失败时不改变当前状态；成功则 isOpen()==true、current()==type。
    bool adopt(TransportType type, std::shared_ptr<ITransport> t);

    // ---- 查询 ----
    TransportType current() const;
    bool isOpen() const;
    // M8-A5：切换窗口标志 —— 门忙但不代表会话永久关闭（trySend 用它区分
    // kBackpressure 与 kNotConnected；switching_ 为 atomic，无锁读取）。
    bool isSwitching() const { return switching_.load(); }
    // 当前 Transport 指针（nullptr = 未 open/失败）。仅供统计/日志读取，
    // 发送必须经 lockTransport()/tryLockTransport()。
    ITransport* transport() const;
    // 当前 Transport 能力快照（按值返回：锁内拷贝，消除锁释放后引用悬垂窗口）。
    TransportCapabilities capabilities() const;
    uint32_t switchCount() const { return switchCount_; }
    uint32_t switchFailures() const { return switchFailures_; }
    // 最近一次 open/switch 失败的简短描述（诊断用）。
    const char* lastError() const { return lastError_; }

    // M7-B：线程安全诊断快照（值语义）。在 stateMutex_ 保护下读取当前
    // Transport 的统计/连接/AP 信息；对端 switchTo/断线重建并发安全
    // （内部经 shared_ptr 持引用，绝不暴露裸指针给调用方）。
    TransportDiagSnapshot diagSnapshot() const;

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
    void clearPending(bool hadOldTransport);            // 丢弃 stale 状态；hadOldTransport 时确保恰好一次会话结束 Disconnected
    void takePending(std::vector<ITransport::State>& out);
    void flushStates(const std::vector<ITransport::State>& states);
    void onTransportData(const uint8_t* data, size_t len, uint32_t gen);
    void onTransportState(ITransport::State s, uint32_t gen);
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
    // M8-A5（TM-01）：会话代际（wire-free 实现字段）。attachCallbacks 时递增；
    // 回调 lambda 按值捕获当前代际，onTransportState/Data 入口校验，不匹配即丢弃。
    // 旧 Transport 的迟到状态/数据（post-clearPending 窗口、in-flight 竞态、
    // zombie 泄漏路径）绝不进入新会话。
    std::atomic<uint32_t> generation_{0};
    std::mutex pendingMutex_;             // 保护 pendingStates_
    std::vector<ITransport::State> pendingStates_;

    bool open_ = false;
    uint32_t switchCount_ = 0;
    uint32_t switchFailures_ = 0;
    char lastError_[96] = "ok";
};

class TransportManager;  // M8-A5：TransportLockGuard 前置声明

// M8-A5：发送门 RAII 守卫 —— 禁止裸 lockTransport/unlockTransport 配对泄漏
// （一次忘记 unlock 即全链路死锁）。析构自动 release；可移动不可拷贝。
class TransportLockGuard {
public:
    explicit TransportLockGuard(ITransport* t, TransportManager& mgr) noexcept
        : t_(t), mgr_(mgr) {}
    ~TransportLockGuard() noexcept { release(); }
    TransportLockGuard(const TransportLockGuard&) = delete;
    TransportLockGuard& operator=(const TransportLockGuard&) = delete;
    TransportLockGuard(TransportLockGuard&& o) noexcept : t_(o.t_), mgr_(o.mgr_) {
        o.t_ = nullptr;
    }
    TransportLockGuard& operator=(TransportLockGuard&& o) noexcept {
        if (this != &o) {
            release();
            t_ = o.t_;
            o.t_ = nullptr;
        }
        return *this;
    }
    ITransport* get() const noexcept { return t_; }
    explicit operator bool() const noexcept { return t_ != nullptr; }
    void release() noexcept {
        if (t_ != nullptr) {
            mgr_.unlockTransport();
            t_ = nullptr;
        }
    }

private:
    ITransport* t_;
    TransportManager& mgr_;
};

// M8-A5：RAII 获取发送门（阻塞/非阻塞各一）。
inline TransportLockGuard lockTransport(TransportManager& mgr) {
    return TransportLockGuard(mgr.lockTransport(), mgr);
}
inline TransportLockGuard tryLockTransport(TransportManager& mgr) {
    return TransportLockGuard(mgr.tryLockTransport(), mgr);
}

}  // namespace transport
}  // namespace espview
