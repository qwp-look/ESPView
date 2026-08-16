// ESPView M2/M4 — Qt 6 Virtual Display 入口（Windows / MSYS2 MinGW64 / Qt 6.11.1）。
//
// 规范来源：docs/DESIGN.md M2/M4 节 + spec §4/§5/§6/§7/§19/§20。
//
// 启动流程：
//   QApplication → MainWindow → ConnectionManager.start(COM4, 115200) / startTcp
//   → SerialWorker（独立线程）打开串口/TCP 监听 → 等 HELLO → CONNECTED
//   → 等 FULL → FrameAssembler commit → DisplayFrame (queued) → VirtualScreenWidget。
// M6-D：正式 Transport 选择 UI（TCP/UART + Apply）→ ConnectionManager.switchTransport
//   → TRANSPORT SWITCHING → DISCONNECTED → CONNECTING → HANDSHAKE → FULL RESYNC →
//   CONNECTED；QSettings 持久化（transport / uart port+baud / tcp port / 窗口大小，
//   绝不保存 Wi-Fi 凭据）。
//
// M4 状态面板（四域 + 诊断列表，spec §三/§四/§十九/§二十）：
//   Connection：Transport 状态 + Protocol Session 状态分层显示、重连计数；
//   Display：分辨率/格式/提交与作废帧/帧率/最近作废原因；
//   Protocol：RX/TX 字节与吞吐、Packet/Message 计数、错误分类（CRC/seq/session）；
//   Heartbeat：PING/PONG 计数、超时、RTT（无测量显示 N/A，不显示 0ms）；
//   Input：sent / dropped / unsupported / ignoredAutoRepeat；
//   诊断列表：最近 50 条（timestamp + severity + source + message），分级着色。
//
// CLI（显式参数覆盖 QSettings；无 QSettings 时默认 TCP / COM4 / 115200 / 0.0.0.0:8765）：
//   --transport uart|tcp  传输类型（默认：QSettings，否则 tcp）
//   --port COM4           串口名（默认：QSettings，否则 COM4；UART 模式）
//   --baud 115200         波特率（默认 115200；UART 模式）
//   --tcp-bind <ip>       TCP Server 监听地址（默认 0.0.0.0；TCP 模式）
//   --tcp-port 8765       TCP Server 端口（默认 8765；TCP 模式）
//   --dump-png <dir> 调试：每个新 FULL commit 保存 full_<frameId>.png 到目录
//   --autoclose-ms N 调试：N ms 后自动 close（clean-exit 测试）

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QTextStream>
#include <QFormLayout>
#include <QGridLayout>
#include <QKeySequence>
#include <QComboBox>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "connection_manager.h"
#include "display_mode_widget.h"
#include "display_router.h"  // DisplayRouteMode（M7-C3 模式 UI 状态）
#include "display_status_panel.h"
#include "display_frame.h"
#include "frame_assembler.h"  // proto::toString(FrameDiscardReason)（状态面板）
#include "i18n.h"             // trText / UiLang（M7-C3 双语）
#include "input_controller.h"
#include "input_event.h"       // makeKeyEvent（M6-D：F12 远端切换协助）
#include "language_selector.h"
#include "physical_capability_snapshot.h"  // M7-C4：能力/健康分离收敛点
#include "physical_status.h"   // parsePhysicalStatusLine（M7-C3 遥测）
#include "serial_worker.h"
#include "split_drawer.h"
#include "physical_preview_widget.h"  // M7-D2：物理预览 widget
#include "wifi_wizard_dialog.h"  // M7-D4：Wi-Fi 配网向导
#include "transport_config.h"  // TransportConfig / validateTransportConfig（M6-D）
#include "virtual_screen_widget.h"

namespace {

void printUsage(const char* argv0) {
    std::printf(
        "ESPView Virtual Display (M2/M4/M6-A/M6-D)\n"
        "Usage: %s [options]\n"
        "  --transport uart|tcp   transport type (default: QSettings, else tcp)\n"
        "  --port COM4            COM port (UART; default: QSettings, else COM4)\n"
        "  --baud 115200          baud rate (UART; default 115200)\n"
        "  --tcp-bind <ip>        TCP server bind address (default 0.0.0.0)\n"
        "  --tcp-port 8765        TCP server port (default 8765)\n"
        "  --dump-png <dir>       debug: save each new FULL frame to <dir>/full_<frameId>.png\n"
        "  --diag-log <file>       debug: append peer/session diag lines to file\n"
        "  --autoclose-ms N       debug: auto-close the window after N ms (clean-exit test)\n"
        "  --no-reset             M6-C test-only: skip UART DTR/RTS reset pulse\n",
        argv0);
}

// M4：诊断 severity → 显示名称/颜色（spec §十九：INFO/WARNING/ERROR/CRITICAL）。
QString severityName(int severity) {
    switch (severity) {
        case 0:
            return QStringLiteral("Info");
        case 1:
            return QStringLiteral("Warning");
        case 2:
            return QStringLiteral("Error");
        case 3:
            return QStringLiteral("Critical");
    }
    return QStringLiteral("Unknown");
}

QString severityColor(int severity) {
    switch (severity) {
        case 0:
            return QStringLiteral("#37474f");  // INFO 深灰
        case 1:
            return QStringLiteral("#e65100");  // WARNING 橙
        case 2:
            return QStringLiteral("#b71c1c");  // ERROR 红
        case 3:
            return QStringLiteral("#880e4f");  // CRITICAL 深红
    }
    return QStringLiteral("#000000");
}

// M6-D：切换耗时统计用单调时钟（与 Worker 的 steady clock 同一基）。
uint64_t steadyMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace

namespace espview {
namespace pc {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(const TransportConfig& initialCfg, const QString& pngDumpDir,
               QWidget* parent = nullptr)
        : QMainWindow(parent), currentCfg_(initialCfg) {
        setWindowTitle(tr_("ESPView") + QStringLiteral(" ") + tr_("Virtual Display") +
                       QStringLiteral(" — 320x240"));
        resize(1160, 720);
        // M6-D §二十一：恢复上次窗口大小（QSettings）。
        const QSize savedSize =
            settings_.value(QStringLiteral("window/size"), QSize(1160, 720)).toSize();
        if (savedSize.width() >= 480 && savedSize.height() >= 400) {
            resize(savedSize);
        }

        screen_ = new VirtualScreenWidget(this);
        buildTransportPanel();  // M6-D §二：正式 Transport 选择 UI（TCP/UART）
        buildModePanel();       // M7-C3：Display Mode UI + 语言选择
        buildSplitDrawer();     // M7-C3：Split Drawer（screen_ 移入 QSplitter）
        buildStatusPanel();
        statusPanel_ = new DisplayStatusPanel(this);  // M7-C3：显示状态面板

        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);
        layout->addLayout(transportGrid_);  // M6-D：Transport UI 置于虚拟屏上方
        layout->addLayout(modeGrid_);        // M7-C3：Display Mode UI + Language
        layout->addWidget(screenSplitter_, 1);  // M7-C3：screen_ | drawer_
        auto* bottomRow = new QHBoxLayout;   // M7-C3：状态面板 | M4 状态面板
        bottomRow->setSpacing(8);
        bottomRow->addWidget(statusPanel_, 3);
        bottomRow->addLayout(statusGrid_);
        layout->addLayout(bottomRow);
        layout->addWidget(diagList_, 1);
        setCentralWidget(central);

        buildMenu();

        // M3：InputController（GUI 线程）— 接 widget 事件 + 转发到 Worker 队列
        inputController_ = new InputController(this);
        screen_->setInputController(inputController_);
        inputController_->setSendFn([this](const espview::input::InputEvent& e) {
            manager_.sendInput(e);
        });

        // 信号接线（Worker → Manager → GUI，全部 queued，GUI 线程独占 widget）
        connect(&manager_, &ConnectionManager::frameReady, this,
                [this](const DisplayFrame& f) { onFrameReady(f); });
        connect(&manager_, &ConnectionManager::statusChanged, this,
                [this](WorkerStatus s, const QString& text) { onStatusChanged(s, text); });
        connect(&manager_, &ConnectionManager::statsChanged, this,
                [this](const WorkerStats& st) { onStatsChanged(st); });
        connect(&manager_, &ConnectionManager::diagAdded, this,
                [this](quint64 ts, int sev, const QString& src, const QString& msg) {
                    onDiagAdded(ts, sev, src, msg);
                });
        // M7-C3：Display Mode 发送与 ACK（独立于 Transport 切换，保留会话）。
        connect(modeWidget_, &DisplayModeWidget::applyRequested, this,
                [this](int mode) {
                    screen_->clearDisplay();  // §五：切换先清陈旧镜像（旧模式帧不残留）
                    manager_.sendDisplayMode(static_cast<uint8_t>(mode));
                    modeSwitchStartMs_ = steadyMs();
                    modeWatchdog_->start();
                    syncModeStateToUi();
                });
        connect(&manager_, &ConnectionManager::displayModeAck, this,
                [this](bool ok) {
                    modeWatchdog_->stop();
                    modeWidget_->onAck(ok);
                    syncModeStateToUi();
                    statusBar()->showMessage(
                        ok ? tr_("Success: SET_MODE") : tr_("Failure: SET_MODE"),
                        3000);
                });
        // M7-D1：CAPABILITIES 能力上行 → snapshot 更新 + capability 门控。
        connect(&manager_, &ConnectionManager::capabilitiesReceived, this,
                [this](const espview::proto::CapabilitiesInfo& caps) {
                    onCapabilitiesReceived(caps);
                });
        // M7-D2：PHYSICAL_PREVIEW 帧快照 → 预览 widget（widget 在 buildSplitDrawer 创建；
        // 信号仅在连接后到达，届时已非空）。
        connect(&manager_, &ConnectionManager::previewFrame, this,
                [this](const espview::pc::PhysicalPreviewState& state) {
                    if (previewWidget_ != nullptr) {
                        previewWidget_->setFrame(state);
                    }
                });
        // M7-C3：语言切换（只改文案，不触碰 transport / display / framebuffer）。
        connect(langSel_, &LanguageSelector::languageChanged, this,
                &MainWindow::onLanguageChanged);

        // M6-D：切换看门狗 —— FULL 未在时限内提交 → Switch failed（§五，不无限循环）。
        switchWatchdog_ = new QTimer(this);
        switchWatchdog_->setInterval(500);
        connect(switchWatchdog_, &QTimer::timeout, this, &MainWindow::onSwitchWatchdog);

        // M7-C3：Display Mode ACK 看门狗 —— ACK 丢失/发送被丢弃时回退模型，
        // 避免 Apply 永久禁用（Agent F §S2.4）。
        modeWatchdog_ = new QTimer(this);
        modeWatchdog_->setInterval(500);
        connect(modeWatchdog_, &QTimer::timeout, this, &MainWindow::onModeWatchdog);

        if (!pngDumpDir.isEmpty()) {
            if (QDir().mkpath(pngDumpDir)) {
                screen_->setPngDumpDir(pngDumpDir);
                statusBar()->showMessage(tr_("PNG dump dir: %1").arg(pngDumpDir), 5000);
            } else {
                statusBar()->showMessage(
                    tr_("Cannot create dump dir: %1").arg(pngDumpDir), 5000);
            }
        }

        // M6-D：应用初始配置（CLI / QSettings 合并结果）并启动 Worker。
        applyConfigToUi(currentCfg_);
        updatePortLabels();
        startTransport(currentCfg_);

        // M7-C3：恢复持久化 UI 状态（语言恢复在最后，触发一次全量重翻译）。
        restoreDisplayModeFromSettings();
        langSel_->setLanguage(settings_.value(QStringLiteral("ui/language"), 0).toInt());
    }

    ~MainWindow() override {
        manager_.stop();  // join Worker 线程；确保无 COM 句柄泄漏
    }

    // M6-D 调试：--diag-log 目标文件（peer/session 诊断追加）
    void setDiagLogPath(const QString& p) { diagLogPath_ = p; }

protected:

    void closeEvent(QCloseEvent* event) override {
        saveSettings();  // M6-D §二十一：退出时持久化配置
        manager_.stop();
        event->accept();
    }

private slots:
    void onFrameReady(const DisplayFrame& frame) {
        // P1-2：只处理当前传输会话的帧（transport switch 后旧会话 stale 帧丢弃）。
        if (frame.sessionId != 0 && frame.sessionId != currentSessionId_) {
            return;
        }
        // M7-C3：模式切换 / 重连后的 FULL resync 门控 ——
        //   - switchingInProgress：模式切换中，旧模式帧不得上屏（只记录到达）；
        //   - fullResyncPending：等待新 FULL 期间 PARTIAL 不显示（重同步语义，
        //     PhysicalOnly 下虚拟输出永久保持清屏占位）。
        const auto& ms = modeWidget_->state();
        if (ms.switchingInProgress) {
            return;  // 切换中：不写屏、不更新 res/format（首个新 FULL 才恢复）
        }
        if (ms.fullResyncPending && frame.frameType != 0) {
            return;  // 重同步未完成：只接受 FULL
        }
        screen_->setFrame(frame);
        // M3：鼠标坐标 clamp 上界跟随对端分辨率
        inputController_->setDisplaySize(frame.width, frame.height);
        resLabel_->setText(QStringLiteral("%1x%2").arg(frame.width).arg(frame.height));
        formatLabel_->setText(frame.pixelFormat == 0 ? tr_("RGB565")
                                                     : QStringLiteral("0x%1")
                                                           .arg(frame.pixelFormat, 2, 16, QLatin1Char('0')));
        // M6-D §五：切换完成判定 —— 新 Transport 收到 FULL commit 才算 CONNECTED。
        if (frame.frameType == 0 && switchPendingFull_) {
            lastSwitchMs_ = steadyMs() - switchStartMs_;
            switchPendingFull_ = false;
            switchWatchdog_->stop();
            switching_ = false;
            applyBtn_->setEnabled(true);
            switchStateLabel_->setText(tr_("CONNECTED (FULL resync done)"));
            switchStateLabel_->setStyleSheet(QStringLiteral("color:#1b5e20;font-weight:bold;"));
            ++switchSuccesses_;
            updateSwitchStatsLabel();
            statusBar()->showMessage(
                tr_("Transport switch OK (%1 ms)").arg(lastSwitchMs_), 5000);
        }
        // M7-C3：FULL 帧提交 → Display Mode 模型 FULL resync 收敛。
        if (frame.frameType == 0) {
            auto s = modeWidget_->state();
            s.onFullCommit();
            modeWidget_->setUiState(s);
            syncModeStateToUi();
        }
    }

    void onStatusChanged(WorkerStatus status, const QString& text) {
        switch (status) {
            case WorkerStatus::Connected:
                connLabel_->setText(tr_("Transport ✓ / Session CONNECTED"));
                connLabel_->setStyleSheet(QStringLiteral("color:#1b5e20;font-weight:bold;"));
                break;
            case WorkerStatus::Connecting:
                connLabel_->setText(tr_("Transport ✓ / Session CONNECTING"));
                connLabel_->setStyleSheet(QStringLiteral("color:#e65100;font-weight:bold;"));
                break;
            case WorkerStatus::Disconnected:
                connLabel_->setText(tr_("DISCONNECTED"));
                connLabel_->setStyleSheet(QStringLiteral("color:#b71c1c;font-weight:bold;"));
                screen_->clearDisplay();  // 断线：清空/隐藏旧画面
                resLabel_->setText(QStringLiteral("—"));
                formatLabel_->setText(QStringLiteral("—"));
                break;
            case WorkerStatus::Error:
                connLabel_->setText(tr_("ERROR"));
                connLabel_->setStyleSheet(QStringLiteral("color:#b71c1c;font-weight:bold;"));
                screen_->clearDisplay();
                if (switching_) {
                    // M6-D §五：切换期间 Worker 报错（如 TCP bind 失败 / COM 占用）
                    // → 立即判失败，不等待看门狗。
                    abortSwitch(text);
                }
                break;
        }
        // M7-C3：状态面板 + Display Mode 模型跟随会话状态。
        statusPanel_->setWorkerStatus(status, text);
        auto s = modeWidget_->state();
        if (status == WorkerStatus::Connected) {
            // 断线期间 Apply 过（waitingForConnection）或 Apply 在飞被断线打断
            // （pendingInterruptedApply，P1-1）且选择未应用 → 自动补发一次
            // SET_MODE（单飞行 + 看门狗，失败回退选择，不重试循环）。
            const bool needsAutoSend =
                (s.waitingForConnection || s.pendingInterruptedApply) &&
                s.selectedMode != s.appliedMode;
            s.onConnected();  // 新会话 → fullResyncPending
            currentSessionId_ = manager_.sessionId();  // P1-2：会话 epoch 更新
            if (needsAutoSend) {
                s.onSwitchStart();  // 锁定本次实际发送模式
            }
            modeWidget_->setUiState(s);
            if (needsAutoSend) {
                manager_.sendDisplayMode(static_cast<uint8_t>(s.selectedMode));
                modeSwitchStartMs_ = steadyMs();
                modeWatchdog_->start();
            }
        } else if (status == WorkerStatus::Disconnected || status == WorkerStatus::Error) {
            s.onDisconnected();
            // M7-C4：会话断开 → 能力学习结果撤销（修复跨会话残留闩锁 bug：
            // 重连到无 OLED 设备时门控不得沿用上一会话的 physicalAvailable）。
            physCap_ = espview::display::resetPhysicalCapability();
            s.onPhysicalAvailable(false);
            s.onPhysicalDegraded(false);
            modeWidget_->setUiState(s);
            drawer_->clearStatus();  // 会话失联 → 抽屉物理状态清空（不伪造）
            // M7-G3: transport-level disconnect clears preview bitmap and capability
            // metadata too (do not rely solely on worker session-level snapshot).
            if (previewWidget_ != nullptr) {
                previewWidget_->clear();
                previewWidget_->setControllerName(QString());
            }
        }
        syncModeStateToUi();
        statusBar()->showMessage(tr_(text.toUtf8().constData()));
    }

    // Port/Baud 显示（M6-D：来自当前配置；TCP 显示监听地址，UART 显示 COM/波特率）。
    void updatePortLabels() {
        if (currentCfg_.kind == TransportKind::kUart) {
            portLabel_->setText(QString::fromStdString(currentCfg_.uartPort));
            baudLabel_->setText(QString::number(currentCfg_.uartBaud));
        } else {
            portLabel_->setText(tr_("TCP %1:%2")
                                    .arg(QString::fromStdString(currentCfg_.tcpBind))
                                    .arg(currentCfg_.tcpPort));
            baudLabel_->setText(tr_("TCP"));
        }
    }

    void onStatsChanged(const WorkerStats& st) {
        // Connection?Transport ??????????Session ??????
        const char* ssName = "Unknown";
        switch (st.sessionState) {
            case 0: ssName = "Disconnected"; break;
            case 1: ssName = "Connecting"; break;
            case 2: ssName = "Handshake"; break;
            case 3: ssName = "Connected"; break;
        }
        // M6-C：显示 Transport 类型（UART/TCP）；TCP 连接后追加对端地址。
        const QString kindName = st.transportKind == 1 ? QStringLiteral("TCP") : QStringLiteral("UART");
        QString detail = tr_("Transport %1  ·  Session %2  ·  Reconnects %3")
                             .arg(kindName)
                             .arg(tr_(ssName))
                             .arg(st.reconnectCount);
        if (st.transportKind == 1 && st.transportPeer[0] != '\0') {
            detail += tr_("  ·  Peer %1").arg(QString::fromUtf8(st.transportPeer));
        }
        connDetailLabel_->setText(detail);
        updatePortLabels();  // 切换后配置可能变化（M6-D）

        // M6-D §十/§十六：Peer 与 Client 显示（TCP 单客户端；UART 无 peer）。
        // 不把「server 监听地址」与「对端 peer 地址」混淆（§二）。
        if (st.transportKind == 1) {
            peerLabel_->setText(st.transportPeer[0] != '\0'
                                    ? tr_("Peer: %1")
                                          .arg(QString::fromUtf8(st.transportPeer))
                                    : tr_("Peer: —"));
            clientLabel_->setText(tr_("Client: 1 / 1 (single)"));
        } else {
            peerLabel_->setText(tr_("Peer: —"));
            clientLabel_->setText(tr_("Client: —"));
        }

        // Display
        framesLabel_->setText(tr_("%1 / %2")
                                  .arg(st.committedFrames)
                                  .arg(st.discardedFrames));
        fpsLabel_->setText(tr_("%1 fps · last id=%2 %3")
                               .arg(st.effectiveFps, 0, 'f', 1)
                               .arg(st.lastFrameId)
                               .arg(st.lastFrameType == 0 ? tr_("FULL")
                                                          : tr_("PARTIAL")));
        if (st.discardedFrames > 0) {
            discardLabel_->setText(
                tr_("%1 @ %2 ms")
                    .arg(tr_(proto::toString(
                             static_cast<proto::FrameDiscardReason>(st.lastDiscardReason))))
                    .arg(st.lastDiscardTimeMs));
        } else {
            discardLabel_->setText(QStringLiteral("—"));
        }
        if (resLabel_->text() == QStringLiteral("—") && st.peerWidth > 0 &&
            st.peerHeight > 0) {
            resLabel_->setText(QStringLiteral("%1x%2").arg(st.peerWidth).arg(st.peerHeight));
        }
        if (formatLabel_->text() == QStringLiteral("—") && st.peerPixelFormat == 0) {
            formatLabel_->setText(tr_("RGB565"));
        }

        // Protocol
        rxLabel_->setText(tr_("%1 B (↑ %2 B/s)")
                              .arg(st.rxBytes)
                              .arg(st.rxBytesPerSec, 0, 'f', 0));
        txLabel_->setText(tr_("%1 B (↑ %2 B/s)")
                              .arg(st.txBytes)
                              .arg(st.txBytesPerSec, 0, 'f', 0));
        pktLabel_->setText(tr_("pkts %1 · rxMsg %2 · txMsg %3")
                               .arg(st.packetsRx)
                               .arg(st.rxMessages)
                               .arg(st.txMessages));
        errLabel_->setText(tr_("decode %1 · CRC %2 · seqGap %3 · session %4")
                               .arg(st.decoderErrors)
                               .arg(st.crcErrors)
                               .arg(st.seqGaps)
                               .arg(st.sessionErrors));

        // Heartbeat
        hbLabel_->setText(tr_("PING sent %1 / recv %2 · PONG sent %3 / recv %4 · timeouts %5")
                              .arg(st.pingSent)
                              .arg(st.pingReceived)
                              .arg(st.pongSent)
                              .arg(st.pongReceived)
                              .arg(st.heartbeatTimeouts));
        if (st.rttValid) {
            rttLabel_->setText(tr_("%1 ms (min %2 / avg %3 / max %4, n=%5)")
                                   .arg(st.rttMs)
                                   .arg(st.rttMinMs)
                                   .arg(st.rttAvgMs)
                                   .arg(st.rttMaxMs)
                                   .arg(st.rttSamples));
        } else {
            rttLabel_->setText(tr_("N/A"));  // 无测量 ≠ 0ms
        }

        // Input
        const InputController::Stats& is = inputController_->stats();
        inputLabel_->setText(tr_("%1 / %2 / %3 / %4")
                                 .arg(st.inputSent)
                                 .arg(st.inputDropped)
                                 .arg(is.unsupported)
                                 .arg(is.ignoredAutoRepeat));
    }

    void onDiagAdded(quint64 ts, int severity, const QString& source, const QString& msg) {
        // 最近 50 条（与 Worker 侧 DiagnosticsRing 容量一致）。
        while (diagList_->count() >= 50) {
            delete diagList_->takeItem(0);
        }
        const QString line = QStringLiteral("[%1] %2 %3: %4")
                                 .arg(QDateTime::fromMSecsSinceEpoch(
                                          static_cast<qint64>(ts))
                                          .toLocalTime()
                                          .toString(QStringLiteral("HH:mm:ss.zzz")))
                                 .arg(tr_(severityName(severity).toUtf8().constData()), -8)
                                 .arg(source)
                                 .arg(msg);
        // M6-D 调试：追加到 --diag-log 文件（完整 ERROR 文本通道，不受 50 条 ring 限制）
        if (!diagLogPath_.isEmpty()) {
            QFile df(diagLogPath_);
            if (df.open(QIODevice::Append | QIODevice::Text)) {
                QTextStream tsout(&df);
                tsout << line << Qt::endl;
            }
        }
        auto* item = new QListWidgetItem(line, diagList_);
        item->setForeground(QColor(severityColor(severity)));
        if (severity >= 2) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
        diagList_->addItem(item);
        diagList_->scrollToBottom();

        // M7-C3：ESP32 ERROR 文本遥测 → PhysicalStatus（worker 在 stats 行前
        // 追加了 "stats: " 前缀，此处剥离后再解析）。
        QString text = msg;
        if (text.startsWith(QStringLiteral("stats: "))) {
            text = text.mid(7);
        }
        espview::display::PhysicalStatus parsed = physSnapshot_;
        if (espview::display::parsePhysicalStatusLine(text.toStdString(), parsed)) {
            physSnapshot_ = parsed;
            statusPanel_->setPhysicalStatus(physSnapshot_);
            drawer_->setPhysicalStatus(physSnapshot_);
            // M7-G3: physical preview Controller row uses telemetry fact (oled line c=) -
            // only updated when an oled line arrives; otherwise stays placeholder (no faking).
            if (previewWidget_ != nullptr && physSnapshot_.oledValid) {
                previewWidget_->setControllerName(QString::fromUtf8(
                    espview::display::controllerCodeName(physSnapshot_.oledController)));
            }
            // M7-C4：能力/健康分离 —— 单一收敛点 PhysicalCapabilitySnapshot。
            // capabilityKnown（曾见 ok=1，学习结果只置位、断开才撤销）只管门控；
            // healthy（最近遥测 ok）管降级；provenance 为未来 CAPABILITIES 上行预留。
            const bool wasKnown = physCap_.capabilityKnown;
            physCap_ = espview::display::makePhysicalCapabilitySnapshot(physSnapshot_, physCap_);
            auto s = modeWidget_->state();
            if (!wasKnown && physCap_.capabilityKnown) {
                s.onPhysicalAvailable(true);
            }
            if (physCap_.capabilityKnown) {
                s.onPhysicalDegraded(!physCap_.healthy);
            }
            modeWidget_->setUiState(s);
            syncModeStateToUi();
        }
    }

    // M7-D1：CAPABILITIES（TYPE 0x02）→ 物理能力快照（wire 直接映射）。
    // 遥测推断保留为 graceful fallback：未收到 CAPABILITIES 时维持现有行为
    // （面板 "Capability unavailable"）；收到后以 wire 事实为准。healthy/
    // telemetryFresh 仍属遥测域（能力≠健康，AD.4），由最近 oled 行驱动。
    void onCapabilitiesReceived(const espview::proto::CapabilitiesInfo& caps) {
        espview::display::PhysicalCapabilitySnapshot snap;
        snap.provenance =
            espview::display::PhysicalCapabilityProvenance::kCapabilitiesMessage;
        snap.capabilityKnown = caps.physicalPresent;
        snap.width = caps.physWidth;
        snap.height = caps.physHeight;
        snap.mono = caps.physMono;
        snap.canReadback = caps.physCanReadback;
        snap.controller =
            static_cast<espview::display::OledControllerCode>(caps.physController);
        snap.address = caps.physI2cAddress;
        snap.scene =
            (caps.sceneSupport & espview::proto::kSceneSupportApplication) != 0
                ? espview::display::PhysicalScene::kApplication
                : espview::display::PhysicalScene::kDiagnostics;
        snap.healthy = physCap_.healthy;
        snap.telemetryFresh = physCap_.telemetryFresh;
        physCap_ = snap;

        // M7-G3: physical preview Controller row prefers wire capability fact
        // (CAPABILITIES physController; telemetry inference is the fallback, same source).
        if (previewWidget_ != nullptr) {
            previewWidget_->setControllerName(QString::fromUtf8(
                espview::display::controllerCodeName(snap.controller)));
        }

        auto s = modeWidget_->state();
        s.onPhysicalAvailable(caps.physicalPresent);
        if (physCap_.capabilityKnown) {
            s.onPhysicalDegraded(!physCap_.healthy);
        }
        modeWidget_->setUiState(s);
        syncModeStateToUi();
    }

    void saveSnapshot() {
        if (!screen_->hasImage()) {
            statusBar()->showMessage(tr_("No image to save yet"), 3000);
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this, tr_("Save current display"), QStringLiteral("espview_snapshot.png"),
            tr_("PNG image (*.png)"));
        if (path.isEmpty()) {
            return;
        }
        if (screen_->savePng(path)) {
            statusBar()->showMessage(tr_("Saved %1").arg(path), 5000);
        } else {
            statusBar()->showMessage(tr_("Failed to save %1").arg(path), 5000);
        }
    }

private:
    // ---- M7-C3：Display Mode UI / Split Drawer / 状态面板 / i18n ----
    QString tr_(const char* key) const {
        return QString::fromUtf8(trText(lang_, key));
    }
    // 注册静态文案标签（key = i18n 目录 key；语言切换时统一重刷）。
    void registerLabel(QLabel* label, const char* key) {
        retranslateLabels_.emplace_back(label, key);
    }
    void buildModePanel() {
        modeGrid_ = new QGridLayout(this);
        modeGrid_->setHorizontalSpacing(8);
        modeGrid_->setVerticalSpacing(4);
        modeWidget_ = new DisplayModeWidget(this);
        modeGrid_->addWidget(modeWidget_, 0, 0, 1, 4);
        auto* langLabel = new QLabel(tr_("Language"), this);
        registerLabel(langLabel, "Language");
        modeGrid_->addWidget(langLabel, 0, 4);
        langSel_ = new LanguageSelector(this);
        modeGrid_->addWidget(langSel_, 0, 5);
        modeGrid_->setColumnStretch(6, 1);
    }
    void buildSplitDrawer() {
        screenSplitter_ = new QSplitter(Qt::Horizontal, this);
        drawer_ = new SplitDrawer(&settings_, this);
        // M7-D2：物理预览 widget 插入抽屉顶部（分区标题 + 位图 + 状态）。
        previewWidget_ = new PhysicalPreviewWidget(drawer_);
        drawer_->addExternalWidget("Physical Preview", previewWidget_);
        previewWidget_->loadSettings(settings_);  // ui/previewEnabled（白名单键）
        screenSplitter_->addWidget(screen_);
        screenSplitter_->addWidget(drawer_);
        screenSplitter_->setStretchFactor(0, 1);
        screenSplitter_->setStretchFactor(1, 0);
        drawer_->loadSettings();  // 恢复 split/drawerVisible + split/drawerWidth
    }
    void restoreDisplayModeFromSettings() {
        const int saved = settings_.value(QStringLiteral("display/mode"), 0).toInt();
        if (saved <= 0 || saved > 3) {
            return;  // capability 未知前仅 VirtualOnly 可恢复（物理模式等遥测）。
        }
        auto s = modeWidget_->state();
        if (s.setSelectedMode(static_cast<espview::display::DisplayRouteMode>(saved))) {
            modeWidget_->setUiState(s);
        }
    }
    // 把模式模型的最新状态推给状态面板；Split 选中时自动打开抽屉。
    void syncModeStateToUi() {
        const auto& s = modeWidget_->state();
        statusPanel_->setModeState(static_cast<int>(s.selectedMode),
                                   static_cast<int>(s.routerState),
                                   s.fullResyncPending);
        if (s.selectedMode == espview::display::DisplayRouteMode::kSplit) {
            drawer_->open();
        }
    }
    void onModeWatchdog() {
        if (!modeWidget_->state().switchingInProgress) {
            modeWatchdog_->stop();
            return;
        }
        if (steadyMs() - modeSwitchStartMs_ >= 30000) {
            modeWatchdog_->stop();
            modeWidget_->onAck(false);  // 超时 → 回退选择 + 错误（不无限等待）
            syncModeStateToUi();
            statusBar()->showMessage(
                tr_("Failure: SET_MODE timeout"), 5000);
        }
    }
    void onLanguageChanged(int lang) {
        lang_ = static_cast<UiLang>(lang);
        retranslateUi();
        settings_.setValue(QStringLiteral("ui/language"), lang);
    }
    void retranslateUi() {
        setWindowTitle(tr_("ESPView") + QStringLiteral(" ") + tr_("Virtual Display") +
                       QStringLiteral(" — 320x240"));
        for (const auto& entry : retranslateLabels_) {
            if (entry.first != nullptr && entry.second != nullptr) {
                entry.first->setText(tr_(entry.second));
            }
        }
        if (previewWidget_ != nullptr) {
            previewWidget_->setUiLanguage(static_cast<int>(lang_));
        }
        if (screen_ != nullptr) {
            screen_->setUiLanguage(static_cast<int>(lang_));
            screen_->update();
        }
        transportCombo_->setItemText(0, tr_("TCP"));
        transportCombo_->setItemText(1, tr_("UART"));
        applyBtn_->setText(tr_("Apply"));
        uartPortTitle_->setText(tr_("UART") + QStringLiteral(" ") + tr_("Port") +
                                QStringLiteral(":"));
        uartBaudTitle_->setText(tr_("Baud") + QStringLiteral(":"));
        tcpBindTitle_->setText(tr_("Local server") + QStringLiteral(":"));
        tcpPortTitle_->setText(tr_("Port") + QStringLiteral(":"));
        if (saveAction_ != nullptr) {
            saveAction_->setText(tr_("Save PNG..."));
        }
        if (quitAction_ != nullptr) {
            quitAction_->setText(tr_("Quit"));
        }
        modeWidget_->setUiLanguage(static_cast<int>(lang_));
        statusPanel_->setUiLanguage(static_cast<int>(lang_));
        drawer_->setUiLanguage(static_cast<int>(lang_));
        syncModeStateToUi();
    }

    // ---- M6-D：正式 Transport 选择 UI（§二/§四/§五/§七）----
    void buildTransportPanel() {
        transportGrid_ = new QGridLayout(this);
        transportGrid_->setHorizontalSpacing(8);
        transportGrid_->setVerticalSpacing(4);

        auto* title = new QLabel(tr_("Transport"), this);
        registerLabel(title, "Transport");
        transportGrid_->addWidget(title, 0, 0);
        transportCombo_ = new QComboBox(this);
        transportCombo_->addItem(tr_("TCP"));
        transportCombo_->addItem(tr_("UART"));
        transportGrid_->addWidget(transportCombo_, 0, 1);
        connect(transportCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                &MainWindow::onTransportComboChanged);

        applyBtn_ = new QPushButton(tr_("Apply"), this);
        transportGrid_->addWidget(applyBtn_, 0, 2);
        connect(applyBtn_, &QPushButton::clicked, this, &MainWindow::onApplyTransport);

        switchStateLabel_ = new QLabel(QStringLiteral("—"), this);
        switchStateLabel_->setStyleSheet(QStringLiteral("font-weight:bold;"));
        transportGrid_->addWidget(switchStateLabel_, 0, 3, 1, 3);

        // UART 配置行（§二/§七：COM4 / 115200 默认）。
        uartCfgWidget_ = new QWidget(this);
        auto* uartLay = new QHBoxLayout(uartCfgWidget_);
        uartLay->setContentsMargins(0, 0, 0, 0);
        uartLay->setSpacing(6);
        uartPortTitle_ =
            new QLabel(tr_("UART") + QStringLiteral(" ") + tr_("Port") +
                           QStringLiteral(":"),
                       uartCfgWidget_);
        uartLay->addWidget(uartPortTitle_);
        uartPortEdit_ = new QLineEdit(uartCfgWidget_);
        uartPortEdit_->setMaximumWidth(110);
        uartLay->addWidget(uartPortEdit_);
        uartBaudTitle_ = new QLabel(tr_("Baud") + QStringLiteral(":"), uartCfgWidget_);
        uartLay->addWidget(uartBaudTitle_);
        uartBaudEdit_ = new QLineEdit(uartCfgWidget_);
        uartBaudEdit_->setMaximumWidth(100);
        uartBaudEdit_->setValidator(new QIntValidator(1, 4000000, uartBaudEdit_));
        uartLay->addWidget(uartBaudEdit_);
        uartLay->addStretch(1);
        transportGrid_->addWidget(uartCfgWidget_, 1, 0, 1, 6);

        // TCP 配置行（§二：Local server = 监听地址；Peer 地址单列显示，
        // 绝不把 server 地址与 peer 地址混淆）。
        tcpCfgWidget_ = new QWidget(this);
        auto* tcpLay = new QHBoxLayout(tcpCfgWidget_);
        tcpLay->setContentsMargins(0, 0, 0, 0);
        tcpLay->setSpacing(6);
        tcpBindTitle_ =
            new QLabel(tr_("Local server") + QStringLiteral(":"), tcpCfgWidget_);
        tcpLay->addWidget(tcpBindTitle_);
        tcpBindEdit_ = new QLineEdit(tcpCfgWidget_);
        tcpBindEdit_->setMaximumWidth(150);
        tcpLay->addWidget(tcpBindEdit_);
        tcpPortTitle_ = new QLabel(tr_("Port") + QStringLiteral(":"), tcpCfgWidget_);
        tcpLay->addWidget(tcpPortTitle_);
        tcpPortEdit_ = new QLineEdit(tcpCfgWidget_);
        tcpPortEdit_->setMaximumWidth(80);
        tcpPortEdit_->setValidator(new QIntValidator(1, 65535, tcpPortEdit_));
        tcpLay->addWidget(tcpPortEdit_);
        tcpLay->addStretch(1);
        transportGrid_->addWidget(tcpCfgWidget_, 2, 0, 1, 6);

        peerLabel_ = new QLabel(tr_("Peer: —"), this);
        transportGrid_->addWidget(peerLabel_, 3, 0, 1, 3);
        clientLabel_ = new QLabel(tr_("Client: —"), this);
        transportGrid_->addWidget(clientLabel_, 3, 3, 1, 3);

        switchStatsLabel_ =
            new QLabel(tr_("Switches 0 · OK 0 · Fail 0 · Last —"), this);
        transportGrid_->addWidget(switchStatsLabel_, 4, 0, 1, 6);

        onTransportComboChanged(transportCombo_->currentIndex());
    }

    // 初始启动（构造期）：按合并后的配置启动 Worker（UART 保持复位脉冲语义；
    // TCP 为 Server 监听）。运行时切换走 onApplyTransport → switchTransport。
    void startTransport(const TransportConfig& cfg) {
        if (cfg.kind == TransportKind::kUart) {
            manager_.start(QString::fromStdString(cfg.uartPort), cfg.uartBaud, cfg.uartNoReset);
        } else {
            manager_.startTcp(cfg.tcpPort, QString::fromStdString(cfg.tcpBind));
        }
    }

    void applyConfigToUi(const TransportConfig& cfg) {
        transportCombo_->setCurrentIndex(cfg.kind == TransportKind::kUart ? 1 : 0);
        uartPortEdit_->setText(QString::fromStdString(cfg.uartPort));
        uartBaudEdit_->setText(QString::number(cfg.uartBaud));
        tcpBindEdit_->setText(QString::fromStdString(cfg.tcpBind));
        tcpPortEdit_->setText(QString::number(cfg.tcpPort));
    }

    TransportConfig readConfigFromUi() const {
        TransportConfig cfg;
        cfg.kind = transportCombo_->currentIndex() == 1 ? TransportKind::kUart
                                                        : TransportKind::kTcp;
        cfg.uartPort = uartPortEdit_->text().trimmed().toStdString();
        cfg.uartBaud = static_cast<quint32>(uartBaudEdit_->text().toUInt());
        cfg.tcpBind = tcpBindEdit_->text().trimmed().toStdString();
        cfg.tcpPort = static_cast<quint16>(tcpPortEdit_->text().toUInt());
        // 运行时切换：跳过 UART 复位脉冲（M6-C --no-reset 语义 —— 对端会话已由
        // F12 test hook 建立，复位会打断其当前 Transport）。
        cfg.uartNoReset = true;
        return cfg;
    }

    void onTransportComboChanged(int index) {
        uartCfgWidget_->setVisible(index == 1);
        tcpCfgWidget_->setVisible(index == 0);
    }

    // §五：Apply 行为 —— 防重复点击 / 本地校验 / SWITCHING / 等 FULL / CONNECTED。
    void onApplyTransport() {
        if (switching_) {
            return;  // §五.1：disable duplicate clicks
        }
        // §六：Display switching 进行中 → Transport switch Apply 禁用
        // （两套 switch transaction 互斥；等 SET_MODE ACK 收敛后再切换）。
        if (modeWidget_->state().switchingInProgress) {
            statusBar()->showMessage(
                tr_("Display mode switch in progress — wait for ACK"), 3000);
            return;
        }
        const TransportConfig target = readConfigFromUi();
        const std::string verr = validateTransportConfig(target);
        if (!verr.empty()) {
            abortSwitch(tr_(verr.c_str()));  // 本地非法配置（不切换）
            return;
        }
        // 幂等：Worker 存活且配置相同 → 无操作（已运行中）。失败态（lastSwitchFailed_）
        // 允许重新 Apply 重试，不陷入“已运行”死循环。
        if (!lastSwitchFailed_ && manager_.isRunning() && target == currentCfg_) {
            statusBar()->showMessage(
                tr_("Already running this transport configuration"), 3000);
            return;
        }
        beginSwitch(target);
    }

    void beginSwitch(const TransportConfig& target) {
        switching_ = true;
        lastSwitchFailed_ = false;
        applyBtn_->setEnabled(false);
        // §六：Transport switching 与 Display switching 互斥——切换期间禁用
        // Display Mode Apply（防两套 switch transaction 打架；恢复由状态机
        // onFullCommit/onDisconnected 收敛，abortSwitch 显式恢复）。
        {
            auto ms = modeWidget_->state();
            ms.applyEnabled = false;
            modeWidget_->setUiState(ms);
        }
        currentSessionId_ = 0;  // P1-2：等待新会话 Connected 更新 epoch
        ++switchCount_;
        lastSwitchError_.clear();
        switchStateLabel_->setText(tr_("TRANSPORT SWITCHING ..."));
        switchStateLabel_->setStyleSheet(QStringLiteral("color:#e65100;font-weight:bold;"));
        updateSwitchStatsLabel();
        // §十二：切换后不保留旧 Transport 的 stale 画面；等新 FULL 再恢复。
        screen_->clearDisplay();

        // §九：远端切换协助（test-only）—— 经旧 Transport 发送 F12（HID 0x45），
        // 命令 ESP32 CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH 钩子切换 UART↔TCP。
        // 生产固件无该钩子时该按键被忽略，PC 侧切换照常进行（UI 不依赖 F12）。
        const bool kindChange = target.kind != currentCfg_.kind;
        if (kindChange && manager_.isRunning()) {
            sendRemoteSwitchF12();
            // 短暂驻留：等 ESP32 sessionLoop（200ms tick）消费切换请求后再停旧会话。
            QEventLoop loop;
            QTimer::singleShot(450, &loop, &QEventLoop::quit);
            loop.exec();
        }

        const uint64_t t0 = steadyMs();
        const bool ok = manager_.switchTransport(target);
        lastSwitchMs_ = steadyMs() - t0;
        currentCfg_ = target;
        if (!ok) {
            abortSwitch(tr_("invalid transport config (switch not performed)"));
            return;
        }
        // 异步完成判定：新 Transport 收到 FULL commit（onFrameReady）→ CONNECTED。
        switchPendingFull_ = true;
        switchStartMs_ = steadyMs();
        switchWatchdog_->start();
        updateSwitchStatsLabel();
        saveSettings();
    }

    void abortSwitch(const QString& reason) {
        switching_ = false;
        switchPendingFull_ = false;
        switchWatchdog_->stop();
        applyBtn_->setEnabled(true);
        // §六：Transport/Display 互斥解除——恢复 Display Mode Apply
        // （模式自身 switching 由 DisplayUiState 状态机管理）。
        {
            auto ms = modeWidget_->state();
            ms.applyEnabled = !ms.switchingInProgress;
            modeWidget_->setUiState(ms);
        }
        lastSwitchFailed_ = true;
        ++switchFailures_;
        lastSwitchError_ = reason;
        switchStateLabel_->setText(tr_("Switch failed: %1").arg(reason));
        switchStateLabel_->setStyleSheet(QStringLiteral("color:#b71c1c;font-weight:bold;"));
        updateSwitchStatsLabel();
    }

    void sendRemoteSwitchF12() {
        // F12 = USB HID usage 0x45（KeyboardMapper：Qt::Key_F12 → 0x45，M6-C 钩子）。
        const auto down = espview::input::makeKeyEvent(espview::input::InputType::kKeyDown,
                                                       0x45u, 0u, 0u);
        const auto up = espview::input::makeKeyEvent(espview::input::InputType::kKeyUp,
                                                     0x45u, 0u, 0u);
        manager_.sendInput(down);
        manager_.sendInput(up);
    }

    void onSwitchWatchdog() {
        if (!switchPendingFull_) {
            return;
        }
        const uint64_t elapsed = steadyMs() - switchStartMs_;
        // UART FULL ≈13.2s；TCP ≈250ms；30s 上限足够，避免无限等待（§五）。
        if (elapsed >= 30000) {
            abortSwitch(tr_("timeout: no FULL commit within 30s"));
        }
    }

    void updateSwitchStatsLabel() {
        switchStatsLabel_->setText(
            tr_("Switches %1 · OK %2 · Fail %3 · Last %4 ms · LastErr %5")
                .arg(switchCount_)
                .arg(switchSuccesses_)
                .arg(switchFailures_)
                .arg(lastSwitchMs_)
                .arg(lastSwitchError_.isEmpty() ? QStringLiteral("—") : lastSwitchError_));
    }

    // M6-D §二十一：QSettings 持久化（transport / uart port+baud / tcp port / 窗口大小；
    // 绝不保存 Wi-Fi 密码或任何 ESP32 凭据）。
    void saveSettings() {
        // M6-E §24.11-12：只写 persistedSettingsKeys() 白名单键（绝不保存凭据）。
        for (const std::string& k : espview::pc::persistedSettingsKeys()) {
            const QString key = QString::fromStdString(k);
            if (key == QStringLiteral("transport/type")) {
                settings_.setValue(key, transportCombo_->currentIndex() == 1
                                            ? QStringLiteral("uart")
                                            : QStringLiteral("tcp"));
            } else if (key == QStringLiteral("uart/port")) {
                settings_.setValue(key, uartPortEdit_->text());
            } else if (key == QStringLiteral("uart/baud")) {
                settings_.setValue(key, uartBaudEdit_->text().toUInt());
            } else if (key == QStringLiteral("tcp/port")) {
                settings_.setValue(key, tcpPortEdit_->text().toUInt());
            } else if (key == QStringLiteral("window/size")) {
                settings_.setValue(key, size());
            } else if (key == QStringLiteral("display/mode")) {
                settings_.setValue(key,
                                   static_cast<int>(modeWidget_->state().selectedMode));
            } else if (key == QStringLiteral("ui/language")) {
                settings_.setValue(key, static_cast<int>(lang_));
            }
            // split/drawerVisible + split/drawerWidth：由 SplitDrawer::saveSettings
            // 统一管理（去抖 + 显式保存），此处不重复写。
        }
        drawer_->saveSettings();
        if (previewWidget_ != nullptr) {
            previewWidget_->saveSettings(settings_);  // ui/previewEnabled（白名单键）
        }
    }

    // M4：四域状态面板（Connection / Display / Protocol / Heartbeat / Input）。
    void buildStatusPanel() {
        statusGrid_ = new QGridLayout(this);
        statusGrid_->setHorizontalSpacing(14);
        statusGrid_->setVerticalSpacing(2);

        int row = 0;
        auto* connHeader = new QLabel(tr_("Connection"), this);
        registerLabel(connHeader, "Connection");
        statusGrid_->addWidget(connHeader, row, 0, 1, 2);
        ++row;
        registerLabel(new QLabel(tr_("State"), this), "State");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        connLabel_ = new QLabel(QStringLiteral("—"), this);
        statusGrid_->addWidget(connLabel_, row, 1);
        ++row;
        registerLabel(new QLabel(tr_("Detail"), this), "Detail");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        connDetailLabel_ = new QLabel(QStringLiteral("—"), this);
        statusGrid_->addWidget(connDetailLabel_, row, 1);
        ++row;
        registerLabel(new QLabel(tr_("Port / Baud"), this), "Port / Baud");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        portLabel_ = new QLabel(QStringLiteral("—"), this);
        baudLabel_ = new QLabel(QStringLiteral("—"), this);
        statusGrid_->addWidget(portLabel_, row, 1);
        statusGrid_->addWidget(baudLabel_, row, 2);

        ++row;
        registerLabel(new QLabel(tr_("Display"), this), "Display");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0, 1, 2);
        ++row;
        registerLabel(new QLabel(tr_("Resolution"), this), "Resolution");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        resLabel_ = new QLabel(QStringLiteral("—"), this);
        statusGrid_->addWidget(resLabel_, row, 1);
        ++row;
        registerLabel(new QLabel(tr_("Format"), this), "Format");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        formatLabel_ = new QLabel(QStringLiteral("—"), this);
        statusGrid_->addWidget(formatLabel_, row, 1);
        ++row;
        registerLabel(new QLabel(tr_("Frames (commit/discard)"), this),
                      "Frames (commit/discard)");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        framesLabel_ = new QLabel(QStringLiteral("0 / 0"), this);
        statusGrid_->addWidget(framesLabel_, row, 1);
        ++row;
        registerLabel(new QLabel(tr_("FPS / last"), this), "FPS / last");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        fpsLabel_ = new QLabel(QStringLiteral("—"), this);
        statusGrid_->addWidget(fpsLabel_, row, 1);
        ++row;
        registerLabel(new QLabel(tr_("Last discard"), this), "Last discard");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        discardLabel_ = new QLabel(QStringLiteral("—"), this);
        statusGrid_->addWidget(discardLabel_, row, 1);

        ++row;
        registerLabel(new QLabel(tr_("Protocol"), this), "Protocol");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0, 1, 2);
        ++row;
        registerLabel(new QLabel(tr_("RX"), this), "RX");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        rxLabel_ = new QLabel(QStringLiteral("0 B"), this);
        statusGrid_->addWidget(rxLabel_, row, 1);
        ++row;
        registerLabel(new QLabel(tr_("TX"), this), "TX");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        txLabel_ = new QLabel(QStringLiteral("0 B"), this);
        statusGrid_->addWidget(txLabel_, row, 1);
        ++row;
        registerLabel(new QLabel(tr_("Packets/Messages"), this), "Packets/Messages");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        pktLabel_ = new QLabel(QStringLiteral("—"), this);
        statusGrid_->addWidget(pktLabel_, row, 1);
        ++row;
        registerLabel(new QLabel(tr_("Errors (decode/CRC/seq/session)"), this),
                      "Errors (decode/CRC/seq/session)");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        errLabel_ = new QLabel(QStringLiteral("0  0  0  0"), this);
        statusGrid_->addWidget(errLabel_, row, 1);

        ++row;
        registerLabel(new QLabel(tr_("Heartbeat"), this), "Heartbeat");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0, 1, 2);
        ++row;
        registerLabel(new QLabel(tr_("PING/PONG"), this), "PING/PONG");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        hbLabel_ = new QLabel(QStringLiteral("—"), this);
        statusGrid_->addWidget(hbLabel_, row, 1, 1, 2);
        ++row;
        registerLabel(new QLabel(tr_("RTT"), this), "RTT");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0);
        rttLabel_ = new QLabel(QStringLiteral("N/A"), this);
        statusGrid_->addWidget(rttLabel_, row, 1);

        ++row;
        registerLabel(new QLabel(tr_("Input (sent/dropped/unsup/ignored)"), this),
                      "Input (sent/dropped/unsup/ignored)");
        statusGrid_->addWidget(retranslateLabels_.back().first, row, 0, 1, 3);
        ++row;
        inputLabel_ = new QLabel(QStringLiteral("0 / 0 / 0 / 0"), this);
        statusGrid_->addWidget(inputLabel_, row, 0, 1, 3);

        // M4：诊断列表（最近 50 条，分级着色）
        diagList_ = new QListWidget(this);
        diagList_->setMaximumHeight(140);
        QFont mono(QStringLiteral("Consolas"), 8);
        mono.setStyleHint(QFont::Monospace);
        diagList_->setFont(mono);
        diagList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }

    void buildMenu() {
        QMenu* fileMenu = menuBar()->addMenu(tr_("File"));
        saveAction_ = fileMenu->addAction(tr_("Save PNG..."), this,
                                          &MainWindow::saveSnapshot);
        saveAction_->setShortcut(QKeySequence::Save);
        fileMenu->addSeparator();
        quitAction_ = fileMenu->addAction(tr_("Quit"), QKeySequence::Quit, this,
                                          &QWidget::close);
        // M7-D4：Wi-Fi 配网向导（模态；内部消费 M7-D3 信号，不切换 Transport）。
        QMenu* toolsMenu = menuBar()->addMenu(tr_("Tools"));
        toolsMenu->addAction(tr_("Wi-Fi Wizard"), this, &MainWindow::openWifiWizard);
    }

    void openWifiWizard() {
        WifiWizardDialog dlg(manager_, this);
        dlg.setUiLanguage(static_cast<int>(lang_));
        dlg.exec();
    }

    VirtualScreenWidget* screen_ = nullptr;
    QGridLayout* statusGrid_ = nullptr;
    QListWidget* diagList_ = nullptr;
    QLabel* connLabel_ = nullptr;
    QLabel* connDetailLabel_ = nullptr;
    QLabel* portLabel_ = nullptr;
    QLabel* baudLabel_ = nullptr;
    QLabel* resLabel_ = nullptr;
    QLabel* formatLabel_ = nullptr;
    QLabel* framesLabel_ = nullptr;
    QLabel* fpsLabel_ = nullptr;
    QLabel* discardLabel_ = nullptr;
    QLabel* rxLabel_ = nullptr;
    QLabel* txLabel_ = nullptr;
    QLabel* pktLabel_ = nullptr;
    QLabel* errLabel_ = nullptr;
    QLabel* hbLabel_ = nullptr;
    QLabel* rttLabel_ = nullptr;
    QLabel* inputLabel_ = nullptr;
    InputController* inputController_ = nullptr;  // GUI 线程；MainWindow 拥有
    ConnectionManager manager_;

    // ---- M6-D：Transport UI / 切换状态 ----
    QGridLayout* transportGrid_ = nullptr;
    QComboBox* transportCombo_ = nullptr;
    QPushButton* applyBtn_ = nullptr;
    QLineEdit* uartPortEdit_ = nullptr;
    QLineEdit* uartBaudEdit_ = nullptr;
    QLineEdit* tcpBindEdit_ = nullptr;
    QLineEdit* tcpPortEdit_ = nullptr;
    QWidget* uartCfgWidget_ = nullptr;
    QWidget* tcpCfgWidget_ = nullptr;
    QLabel* switchStateLabel_ = nullptr;
    QLabel* switchStatsLabel_ = nullptr;
    QLabel* peerLabel_ = nullptr;
    QLabel* clientLabel_ = nullptr;
    QTimer* switchWatchdog_ = nullptr;

    TransportConfig currentCfg_;      // 当前已应用配置（Worker 实际运行）
    bool switching_ = false;          // Apply 处理中（§五.1：防重复点击）
    bool switchPendingFull_ = false;  // 等待新 Transport 的 FULL commit
    bool lastSwitchFailed_ = false;   // 上一次切换失败（允许重新 Apply 重试）
    uint64_t switchStartMs_ = 0;
    uint64_t switchCount_ = 0;
    uint64_t switchSuccesses_ = 0;
    uint64_t switchFailures_ = 0;
    uint64_t lastSwitchMs_ = 0;
    QString lastSwitchError_;
    QSettings settings_;  // org/app name 在 main() 设置（M6-D §二十一）
    QString diagLogPath_;  // --diag-log：peer/session 诊断追加到文件（M6-D 调试）

    // ---- M7-C3：Display Mode UI / Split Drawer / 状态面板 / i18n ----
    QGridLayout* modeGrid_ = nullptr;
    DisplayModeWidget* modeWidget_ = nullptr;
    LanguageSelector* langSel_ = nullptr;
    DisplayStatusPanel* statusPanel_ = nullptr;
    SplitDrawer* drawer_ = nullptr;
    QSplitter* screenSplitter_ = nullptr;
    QTimer* modeWatchdog_ = nullptr;
    PhysicalPreviewWidget* previewWidget_ = nullptr;  // M7-D2：物理预览 widget
    UiLang lang_ = UiLang::kEnglish;
    espview::display::PhysicalStatus physSnapshot_;    // 最近遥测快照（GUI 线程）
    espview::display::PhysicalCapabilitySnapshot physCap_;  // M7-C4：能力/健康收敛点
    uint64_t currentSessionId_ = 0;  // P1-2：当前传输会话 epoch（0 = 无会话）
    uint64_t modeSwitchStartMs_ = 0;
    std::vector<std::pair<QLabel*, const char*>> retranslateLabels_;  // i18n 重刷
    QLabel* uartPortTitle_ = nullptr;
    QLabel* uartBaudTitle_ = nullptr;
    QLabel* tcpBindTitle_ = nullptr;
    QLabel* tcpPortTitle_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* quitAction_ = nullptr;
};

}  // namespace pc
}  // namespace espview

int main(int argc, char** argv) {
    // queued signal 需要运行时注册的自定义类型（Q_DECLARE_METATYPE 之上）
    qRegisterMetaType<espview::pc::DisplayFrame>("espview::pc::DisplayFrame");
    // M7-D2：PhysicalPreviewState（queued 连接需要运行时注册）。
    qRegisterMetaType<espview::pc::PhysicalPreviewState>("espview::pc::PhysicalPreviewState");
    // M7-D3：Wi-Fi 消息解析结果（queued 连接需要运行时注册；无 Qt 依赖类型）。
    qRegisterMetaType<espview::proto::WifiScanResultInfo>("espview::proto::WifiScanResultInfo");
    qRegisterMetaType<espview::proto::WifiStatusInfo>("espview::proto::WifiStatusInfo");
    qRegisterMetaType<espview::pc::DisplayRect>("espview::pc::DisplayRect");
    qRegisterMetaType<espview::pc::WorkerStats>("espview::pc::WorkerStats");
    qRegisterMetaType<espview::pc::WorkerStatus>("espview::pc::WorkerStatus");

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("espview_virtual_display"));
    app.setOrganizationName(QStringLiteral("espview"));

    // M6-D §六/§七/§二十一：默认值 = QSettings（有则用之）→ 建议默认
    //（Transport TCP / UART COM4 115200 / TCP bind 0.0.0.0:8765）；
    // CLI 显式参数覆盖 QSettings。绝不保存 Wi-Fi 密码 / ESP32 凭据。
    espview::pc::TransportConfig cfg;
    {
        // M6-E §24.11-12：只读 persistedSettingsKeys() 白名单键（绝不加载凭据）。
        QSettings settings;
        for (const std::string& k : espview::pc::persistedSettingsKeys()) {
            const QString key = QString::fromStdString(k);
            if (key == QStringLiteral("transport/type")) {
                if (settings.value(key, QStringLiteral("tcp")).toString() ==
                    QStringLiteral("uart")) {
                    cfg.kind = espview::pc::TransportKind::kUart;
                }
            } else if (key == QStringLiteral("uart/port")) {
                cfg.uartPort = settings.value(key, QStringLiteral("COM4"))
                                   .toString()
                                   .toStdString();
            } else if (key == QStringLiteral("uart/baud")) {
                cfg.uartBaud = settings.value(key, 115200).toUInt();
            } else if (key == QStringLiteral("tcp/port")) {
                cfg.tcpPort = static_cast<quint16>(
                    settings.value(key, 8765).toUInt());
            }
            // window/size 由 MainWindow 构造读取（保持现状）。
        }
    }

    QString pngDumpDir;
    QString diagLogPath;
    int autocloseMs = 0;

    const QStringList args = app.arguments();
    // M6-E §18：transport 参数统一交给 applyCliOverrides（CLI > QSettings > default）；
    // 非 transport 参数仍在下方原样解析（--dump-png/--diag-log/--no-reset/
    // --autoclose-ms/--help）。
    {
        std::vector<std::string> cliArgs;
        for (int i = 1; i < args.size(); ++i) {
            cliArgs.emplace_back(args[i].toStdString());
        }
        std::string cliErr;
        if (!espview::pc::applyCliOverrides(cfg, cliArgs, &cliErr)) {
            std::fprintf(stderr, "%s\n", cliErr.c_str());
            return 1;
        }
    }
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--dump-png") && i + 1 < args.size()) {
            pngDumpDir = args[++i];
        } else if (args[i] == QStringLiteral("--diag-log") && i + 1 < args.size()) {
            diagLogPath = args[++i];
        } else if (args[i] == QStringLiteral("--no-reset")) {
            cfg.uartNoReset = true;
        } else if (args[i] == QStringLiteral("--autoclose-ms") && i + 1 < args.size()) {
            bool ok = false;
            const int v = args[++i].toInt(&ok);
            if (ok && v > 0) {
                autocloseMs = v;
            }
        } else if (args[i] == QStringLiteral("--help") || args[i] == QStringLiteral("-h")) {
            printUsage(argv[0]);
            return 0;
        }
    }

    espview::pc::MainWindow window(cfg, pngDumpDir);
    window.setDiagLogPath(diagLogPath);
    window.show();
    if (autocloseMs > 0) {
        // 调试：走真实 closeEvent → ConnectionManager.stop() → join Worker 的路径
        QTimer::singleShot(autocloseMs, &window, &QWidget::close);
    }
    return app.exec();
}

#include "main.moc"
