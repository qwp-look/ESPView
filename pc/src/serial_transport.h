// ESPView — HostUartTransport（PC 侧，M1-3B / M8-A3 收敛到 canonical ITransport）。
//
// 规范来源：docs/DESIGN.md E/F/I 节 + M8-A3（§三十五：直接实现 shared/transport
//   espview::transport::ITransport，删除 IPcTransport/pc_transport.h）。
// 职责：open / close / send / isConnected / dataCallback / stateCallback /
//   capabilities()。不理解 Packet/Message/Frame，HELLO/会话/帧组装全部属于上层
//   （ProtocolEndpoint）。
//
// 平台：Windows + Win32 COM API（CreateFile/DCB/COMMTIMEOUTS），C++17，无 Qt。
//
// 线程模型：
//   - RX worker 线程：ReadFile（短超时 50ms）→ dataCallback（回调仅在调用期间有效）。
//     每次 ReadFile 返回的字节数即为真实 UART read size（允许半包/多包任意切分）。
//   - send()：任意调用线程同步 WriteFile（M8-A3：整个 WriteFile 循环持锁，close
//     在锁内置 INVALID —— 消除句柄竞争）。
//   - close()：置停止标志 → 等待 RX worker 退出（ReadFile 50ms 超时保证及时退出）
//     → 锁内置句柄 INVALID 后 CloseHandle。
//   - 回调生命周期：dataCallback/stateCallback 由 setXxxCallback 设置，仅在
//     open 期间有效；close() 完成后不会再有回调触发。
//
// 状态语义：open 成功 = Connected（仅串口/driver 可用）；open 失败 = Error；
//   close = Disconnected。PC 已连接/握手完成由协议层判定。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "transport.h"  // shared/transport：唯一 canonical ITransport

namespace espview {
namespace pc {

class HostUartTransport : public espview::transport::ITransport {
public:
    using State = espview::transport::ITransport::State;
    using DataCallback = espview::transport::ITransport::DataCallback;
    using StateCallback = espview::transport::ITransport::StateCallback;

    struct Config {
        std::string port = "COM3";
        uint32_t baud = 115200;
        uint8_t data_bits = 8;
        uint8_t stop_bits = 1;  // 1 或 2
        char parity = 'N';      // 'N' / 'E' / 'O'
        // read：单次 ReadFile 的最长等待（含帧间空闲间隔）。短值保证 close 及时。
        uint32_t read_timeout_ms = 50;
        // write：单次 WriteFile 超时（ms）。
        uint32_t write_timeout_ms = 1000;
        // ESP32 复位脉冲（DTR=False 保持 GPIO0 高，RTS 脉冲 EN），与
        // scripts/pc_com3_session_test.py 的 reset 序列一致。
        bool reset_on_open = false;
    };

    HostUartTransport();  // 默认配置（COM3/115200）
    explicit HostUartTransport(Config cfg);
    ~HostUartTransport() override;

    HostUartTransport(const HostUartTransport&) = delete;
    HostUartTransport& operator=(const HostUartTransport&) = delete;

    // 打开并配置串口（配置在构造时注入）。失败返回 false 并进入 Error 状态。
    // 已 open 未 close 时返回 false（明确失败，不做隐式重开）；close() 后可重开。
    bool open() override;
    // M8-A3（测试/诊断工具专用）：close() 后、再次 open() 前更新配置
    // （com3_frame_test 的 reconnect 场景需在重开时跳过复位脉冲）。
    // 生产路径（SerialWorker 工厂）每次构造新实例注入配置，无需本方法；
    // open 期间调用行为未定义（调用方必须已 close）。
    void setConfig(const Config& cfg) {
        if (!opened_) {  // M8-A3：open 期间禁止改配置（避免与 driver 配置竞态）
            cfg_ = cfg;
        }
    }
    // 幂等关闭；close() 返回后不再触发任何回调。
    void close() override;
    bool isConnected() const override;
    // 完整发送 len 字节（Transport 内部处理 short write）。
    // kBackpressure = 写入超时（would-block）；kNotConnected = 未连接；
    // kError = 参数/写入失败。
    espview::transport::SendStatus send(const uint8_t* data, size_t len) override;
    const espview::transport::TransportCapabilities& capabilities() const override {
        return caps_;
    }

    void setDataCallback(DataCallback cb) override;
    void setStateCallback(StateCallback cb) override;

    // ---- 诊断统计（M1-3B 验收：partial read / sticky packet 证据）----
    uint64_t rxBytes() const override;
    uint64_t txBytes() const override;
    uint64_t readCount() const;                 // ReadFile 成功次数
    std::vector<size_t> lastReadSizes(size_t max) const;  // 最近 max 次 read size

private:
    void rxLoop();
    void setState(State s);
    void applyResetPulse(HANDLE h);

    Config cfg_;
    espview::transport::TransportCapabilities caps_;

    // sendMutex_：M8-A3 —— 保护"句柄有效性 + 整个 WriteFile 循环"，close 在锁内
    // 置 INVALID，彻底消除 send/close 句柄竞争（原实现锁外 WriteFile 与 close 竞态）。
    std::mutex sendMutex_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::thread rxThread_;
    std::atomic<bool> stopRequested_{false};
    bool opened_ = false;

    // 回调与状态（open/close/set 回调时访问，RX 线程只读副本）。
    std::mutex cbMutex_;
    DataCallback dataCb_;
    StateCallback stateCb_;

    // 统计（RX 线程写，主线程读；用原子+互斥）。
    std::atomic<uint64_t> rxBytes_{0};
    std::atomic<uint64_t> txBytes_{0};
    std::atomic<uint64_t> readCount_{0};
    mutable std::mutex readSizesMutex_;
    std::vector<size_t> readSizes_;

    State state_ = State::kDisconnected;
    mutable std::mutex stateMutex_;
};

}  // namespace pc
}  // namespace espview
