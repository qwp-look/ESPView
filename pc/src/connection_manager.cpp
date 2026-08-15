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

}  // namespace pc
}  // namespace espview
