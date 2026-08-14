// ESPView — HostUartTransport（PC 侧，M1-3B）
//
// 规范来源：docs/DESIGN.md E/F/I 节（ITransport 同构接口 + PC 线程模型）。
// 职责（与 ESP32 UartTransport 同构）：open / close / send / isConnected /
//   dataCallback / stateCallback / mtu()。不理解 Packet/Message/Frame，
//   HELLO/会话/帧组装全部属于上层（ProtocolEndpoint）。
//
// 平台：Windows + Win32 COM API（CreateFile/DCB/COMMTIMEOUTS），C++17，无 Qt。
//
// 线程模型：
//   - RX worker 线程：ReadFile（短超时 50ms）→ dataCallback（回调仅在调用期间有效）。
//     每次 ReadFile 返回的字节数即为真实 UART read size（允许半包/多包任意切分）。
//   - send()：任意调用线程同步 WriteFile（互斥保护），发送短控制包为主。
//   - close()：置停止标志 → 等待 RX worker 退出（ReadFile 50ms 超时保证及时退出）
//     → CloseHandle。避免 CancelIo/关闭竞态（ERROR_OPERATION_ABORTED 只在
//     显式 CancelIoEx 或句柄被强制关闭时出现；本实现通过短超时 + join 规避）。
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

#include "pc_transport.h"

namespace espview {
namespace pc {

class HostUartTransport : public IPcTransport {
public:
    // 与 IPcTransport::State 同一类型（枚举值对齐；别名保持既有调用兼容）。
    using State = IPcTransport::State;
    using DataCallback = IPcTransport::DataCallback;
    using StateCallback = IPcTransport::StateCallback;

    struct Config : public PcTransportConfig {
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

    HostUartTransport() = default;
    ~HostUartTransport();

    HostUartTransport(const HostUartTransport&) = delete;
    HostUartTransport& operator=(const HostUartTransport&) = delete;

    // 打开并配置串口（cfg 必须是 HostUartTransport::Config）。失败返回 false 并进入 Error 状态。
    bool open(const PcTransportConfig& cfg) override;
    // 幂等关闭；close() 返回后不再触发任何回调。
    void close() override;
    bool isConnected() const override;
    // 完整发送 len 字节。false = 未连接 / 写入失败 / 超时。
    bool send(const uint8_t* data, size_t len) override;
    size_t mtu() const override;  // 20B header + 4096 payload

    void setDataCallback(DataCallback cb) override;
    void setStateCallback(StateCallback cb) override;

    // ---- 诊断统计（M1-3B 验收：partial read / sticky packet 证据）----
    uint64_t rxBytes() const;
    uint64_t txBytes() const;
    uint64_t readCount() const;                 // ReadFile 成功次数
    std::vector<size_t> lastReadSizes(size_t max) const;  // 最近 max 次 read size

private:
    void rxLoop();
    void setState(State s);
    void applyResetPulse(HANDLE h);

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

    State state_ = State::Disconnected;
    mutable std::mutex stateMutex_;
    size_t mtu_ = 0;
};

}  // namespace pc
}  // namespace espview
