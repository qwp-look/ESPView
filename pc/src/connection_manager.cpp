// ESPView M2 — ConnectionManager 实现（见 connection_manager.h）。

#include "connection_manager.h"

namespace espview {
namespace pc {

ConnectionManager::ConnectionManager(QObject* parent) : QObject(parent) {
    // Worker 的 queued signals 直接转发；发射线程（Worker）与接收线程（GUI）
    // 不同，Qt 自动使用 QueuedConnection。
    connect(&worker_, &SerialWorker::frameReady, this, &ConnectionManager::frameReady);
    connect(&worker_, &SerialWorker::statusChanged, this, &ConnectionManager::statusChanged);
    connect(&worker_, &SerialWorker::statsChanged, this, &ConnectionManager::statsChanged);
    connect(&worker_, &SerialWorker::diagAdded, this, &ConnectionManager::diagAdded);
    connect(&worker_, &SerialWorker::displayModeAck, this, &ConnectionManager::displayModeAck);
    // M7-D1：CAPABILITIES 能力快照转发（与其它 Worker signal 同模式，queued）。
    connect(&worker_, &SerialWorker::capabilitiesReceived, this,
            &ConnectionManager::capabilitiesReceived);
    // M7-D2：PHYSICAL_PREVIEW 帧快照转发（与其它 Worker signal 同模式，queued）。
    connect(&worker_, &SerialWorker::previewFrame, this,
            &ConnectionManager::previewFrame);
    // M7-D3：Wi-Fi 消息/ACK 转发（同模式，queued；D4 向导消费）。
    connect(&worker_, &SerialWorker::wifiScanResult, this,
            &ConnectionManager::wifiScanResult);
    connect(&worker_, &SerialWorker::wifiStatus, this,
            &ConnectionManager::wifiStatus);
    connect(&worker_, &SerialWorker::wifiScanReqAck, this,
            &ConnectionManager::wifiScanReqAck);
    connect(&worker_, &SerialWorker::wifiConfigAck, this,
            &ConnectionManager::wifiConfigAck);
}

ConnectionManager::~ConnectionManager() {
    stop();
}

void ConnectionManager::start(const QString& port, quint32 baud, bool noReset) {
    worker_.start(port, baud, noReset);
}

void ConnectionManager::startTcp(uint16_t port, const QString& bind) {
    worker_.startTcp(port, bind);
}

bool ConnectionManager::switchTransport(const TransportConfig& cfg) {
    return worker_.switchTransport(cfg);
}

void ConnectionManager::stop() {
    worker_.stop();
}

bool ConnectionManager::isRunning() const {
    return worker_.isRunning();
}

void ConnectionManager::sendInput(const espview::input::InputEvent& ev) {
    worker_.sendInput(ev);
}

void ConnectionManager::sendDisplayMode(uint8_t mode) {
    worker_.sendDisplayMode(mode);
}

void ConnectionManager::sendWifiScanRequest(uint8_t maxEntries) {
    worker_.sendWifiScanRequest(maxEntries);
}

void ConnectionManager::sendWifiConfig(const std::string& ssid, const std::string& password,
                                       uint32_t serverIp, uint16_t serverPort) {
    worker_.sendWifiConfig(ssid, password, serverIp, serverPort);
}

void ConnectionManager::sendWifiClear() {
    worker_.sendWifiClear();
}

}  // namespace pc
}  // namespace espview
