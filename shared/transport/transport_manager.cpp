// ESPView M6-C — TransportManager 实现（见 transport_manager.h）。

#include "transport_manager.h"

#include <utility>

namespace espview {
namespace transport {

TransportManager::TransportManager(TransportFactory factory, TransportType initial)
    : factory_(std::move(factory)), initialType_(initial), currentType_(initial) {}

TransportManager::~TransportManager() {
    close();
}

bool TransportManager::open() {
    std::vector<ITransport::State> pending;
    bool ok = false;
    {
        std::lock_guard<std::mutex> tx(txMutex_);
        {
            std::lock_guard<std::mutex> st(stateMutex_);
            if (open_) {
                return true;  // 已 open：幂等
            }
        }
        switching_.store(true);
        closeLocked();  // Disconnected → pending
        {
            std::lock_guard<std::mutex> st(stateMutex_);
            currentType_ = initialType_;
        }
        ok = createAndOpenLocked();
        switching_.store(false);
    }
    takePending(pending);
    flushStates(pending);
    return ok;
}

void TransportManager::close() {
    std::shared_ptr<ITransport> old;
    {
        std::lock_guard<std::mutex> tx(txMutex_);
        {
            std::lock_guard<std::mutex> st(stateMutex_);
            old = std::move(current_);
            open_ = false;
        }
        // 锁外 close：switching_=true → Disconnected 缓冲，不直接回调（防死锁）
    }
    if (old != nullptr) {
        old->close();
        detachCallbacks(*old);
    }
    switching_.store(false);
    std::vector<ITransport::State> pending;
    takePending(pending);
    flushStates(pending);
}

bool TransportManager::switchTo(TransportType type) {
    std::vector<ITransport::State> pending;
    bool ok = false;
    {
        std::lock_guard<std::mutex> tx(txMutex_);
        {
            std::lock_guard<std::mutex> st(stateMutex_);
            if (currentType_ == type && open_) {
                return true;  // 已在目标类型且已 open：幂等
            }
        }
        switching_.store(true);
        closeLocked();  // Disconnected → pending
        clearPending();  // CS-3：丢弃旧 Transport 的 stale 状态（保留会话结束 Disconnected）
        {
            std::lock_guard<std::mutex> st(stateMutex_);
            currentType_ = type;
        }
        ok = createAndOpenLocked();  // Connecting/Connected → pending
        ++switchCount_;
        if (!ok) {
            ++switchFailures_;
        }
        switching_.store(false);
    }
    takePending(pending);
    flushStates(pending);  // 锁外：上层回调可安全调用 sink
    return ok;
}

bool TransportManager::reopen() {
    std::vector<ITransport::State> pending;
    bool ok = false;
    {
        std::lock_guard<std::mutex> tx(txMutex_);
        switching_.store(true);
        closeLocked();
        clearPending();  // CS-3：丢弃旧 Transport 的 stale 状态（保留会话结束 Disconnected）
        ok = createAndOpenLocked();
        switching_.store(false);
    }
    takePending(pending);
    flushStates(pending);
    return ok;
}

TransportType TransportManager::current() const {
    std::lock_guard<std::mutex> st(stateMutex_);
    return currentType_;
}

bool TransportManager::isOpen() const {
    std::lock_guard<std::mutex> st(stateMutex_);
    return open_ && current_ != nullptr;
}

ITransport* TransportManager::transport() const {
    std::lock_guard<std::mutex> st(stateMutex_);
    return current_.get();
}

const TransportCapabilities& TransportManager::capabilities() const {
    std::lock_guard<std::mutex> st(stateMutex_);
    static const TransportCapabilities kNone{};
    return current_ != nullptr ? current_->capabilities() : kNone;
}

void TransportManager::setDataCallback(ITransport::DataCallback cb) {
    std::lock_guard<std::mutex> st(stateMutex_);
    dataCb_ = std::move(cb);
}

void TransportManager::setStateCallback(ITransport::StateCallback cb) {
    std::lock_guard<std::mutex> st(stateMutex_);
    stateCb_ = std::move(cb);
}

ITransport* TransportManager::lockTransport() {
    txMutex_.lock();
    {
        std::lock_guard<std::mutex> st(stateMutex_);
        if (!open_ || current_ == nullptr) {
            txMutex_.unlock();
            return nullptr;
        }
        return current_.get();
    }
}

ITransport* TransportManager::tryLockTransport() {
    if (!txMutex_.try_lock()) {
        return nullptr;  // 门忙（切换中/大帧发送中）
    }
    {
        std::lock_guard<std::mutex> st(stateMutex_);
        if (!open_ || current_ == nullptr) {
            txMutex_.unlock();
            return nullptr;
        }
        return current_.get();
    }
}

void TransportManager::unlockTransport() {
    txMutex_.unlock();
}

void TransportManager::attachCallbacks(ITransport& t) {
    t.setDataCallback([this](const uint8_t* d, size_t n) { onTransportData(d, n); });
    t.setStateCallback([this](ITransport::State s) { onTransportState(s); });
}

void TransportManager::detachCallbacks(ITransport& t) {
    t.setDataCallback(nullptr);
    t.setStateCallback(nullptr);
}

void TransportManager::onTransportData(const uint8_t* data, size_t len) {
    if (switching_.load()) {
        return;  // 切换窗口：丢弃（毫秒级；协议握手会恢复）
    }
    ITransport::DataCallback cb;
    {
        std::lock_guard<std::mutex> st(stateMutex_);
        cb = dataCb_;
    }
    if (cb != nullptr) {
        cb(data, len);
    }
}

void TransportManager::onTransportState(ITransport::State s) {
    if (switching_.load()) {
        std::lock_guard<std::mutex> pm(pendingMutex_);
        pendingStates_.push_back(s);
        return;
    }
    ITransport::StateCallback cb;
    {
        std::lock_guard<std::mutex> st(stateMutex_);
        cb = stateCb_;
    }
    if (cb != nullptr) {
        cb(s);
    }
}

void TransportManager::flushStates(const std::vector<ITransport::State>& states) {
    if (states.empty()) {
        return;
    }
    ITransport::StateCallback cb;
    {
        std::lock_guard<std::mutex> st(stateMutex_);
        cb = stateCb_;
    }
    if (cb == nullptr) {
        return;
    }
    for (ITransport::State s : states) {
        cb(s);
    }
}

void TransportManager::takePending(std::vector<ITransport::State>& out) {
    std::lock_guard<std::mutex> pm(pendingMutex_);
    out = std::move(pendingStates_);
    pendingStates_.clear();
}

void TransportManager::clearPending() {
    // CS-3：丢弃旧 Transport 在 close 窗口内产生的 stale 状态（迟到的
    // Connecting/Connected 等，不能重放进新会话）。保留 closeLocked() 的正常
    // Disconnected（会话结束信号）：上层依赖它做 input/session 清理
    // （main.cpp onSessionState → InputManager.resetState；§二十八.9）。
    std::lock_guard<std::mutex> pm(pendingMutex_);
    bool keepDisconnected = false;
    for (ITransport::State s : pendingStates_) {
        if (s == ITransport::State::kDisconnected) {
            keepDisconnected = true;
            break;
        }
    }
    if (!keepDisconnected) {
        pendingStates_.clear();
        return;
    }
    std::vector<ITransport::State> keep;
    keep.push_back(ITransport::State::kDisconnected);
    pendingStates_.swap(keep);
}

bool TransportManager::createAndOpenLocked() {
    std::shared_ptr<ITransport> t;
    {
        std::lock_guard<std::mutex> st(stateMutex_);
        if (factory_ == nullptr) {
            setLastError("factory not set");
            return false;
        }
        t = factory_(currentType_);
    }
    if (t == nullptr) {
        setLastError("factory returned null");
        return false;
    }
    attachCallbacks(*t);
    if (!t->open()) {
        setLastError("transport open failed");
        detachCallbacks(*t);
        t->close();
        {
            std::lock_guard<std::mutex> st(stateMutex_);
            current_.reset();
            open_ = false;
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> st(stateMutex_);
        current_ = std::move(t);
        open_ = true;
    }
    return true;
}

void TransportManager::closeLocked() {
    std::shared_ptr<ITransport> old;
    {
        std::lock_guard<std::mutex> st(stateMutex_);
        old = std::move(current_);
        open_ = false;
    }
    if (old != nullptr) {
        old->close();  // Disconnected → pending（switching_=true）
        detachCallbacks(*old);
    }
}

void TransportManager::setLastError(const char* msg) {
    std::lock_guard<std::mutex> st(stateMutex_);
    const char* src = msg != nullptr ? msg : "unknown";
    size_t i = 0;
    while (src[i] != '\0' && i + 1 < sizeof(lastError_)) {
        lastError_[i] = src[i];
        ++i;
    }
    lastError_[i] = '\0';
}

}  // namespace transport
}  // namespace espview
