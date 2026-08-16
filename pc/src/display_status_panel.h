// ESPView M7-C3 — DisplayStatusPanel：RuntimeStats + Worker 状态 + ESP32 ERROR
// 文本遥测 的 GUI 显示面板（Qt Widgets）。
//
// 数据源（均为 GUI 线程 queued 投递，纯展示、只读复用，不改变底层结构）：
//   - SerialWorker::statusChanged  → setWorkerStatus(status, text)
//   - SerialWorker::statsChanged   → setStats(WorkerStats)
//   - SerialWorker::diagAdded      → parsePhysicalStatusLine → merge →
//                                     setPhysicalStatus(PhysicalStatus)
//   - mod 行 / SET_MODE ACK        → setModeState(mode, routerState, fullResync)
//
// 分组（任务书 M7-C3）：Transport / Session / Display Mode / Virtual /
// Physical / Router / Errors / FULL resync。文案全部走 tr()（Agent D 的
// i18n 接入占位；本文件不引入翻译基础设施）。
//
// 状态区分（任务书 §十一 Unavailable / Degraded / Disabled / Active）：
//   PanelState 枚举 + 着色（Active 绿 / Degraded 橙 / Disabled 灰 /
//   Unavailable 红）；各分组内部映射见 refresh*() 注释。
//
// 本文件为 Agent E 独占新增；由主代理接入 espview_virtual_display 构建
// （需把 shared/display 加入目标 include path 或链接 espview_display）。

#pragma once

#include <QString>
#include <QWidget>

#include <cstdint>

#include "i18n.h"
#include "physical_status.h"  // espview::display::PhysicalStatus
#include "serial_worker.h"    // espview::pc::WorkerStatus / WorkerStats

class QGridLayout;  // 全局作用域前向声明（Qt 类，勿放入命名空间）
class QGroupBox;
class QLabel;

namespace espview {
namespace pc {

// 单项展示状态（任务书 §十一）。面板着色：Active 绿 / Degraded 橙 /
// Disabled 灰 / Unavailable 红。
enum class PanelState : uint8_t {
    kUnavailable = 0,  // 无数据 / 断线 / 不可用
    kDegraded = 1,     // 部分可用（降级）
    kDisabled = 2,     // 路由/功能关闭（正常非错误）
    kActive = 3,       // 正常
};


class DisplayStatusPanel : public QWidget {
    Q_OBJECT
public:
    explicit DisplayStatusPanel(QWidget* parent = nullptr);

public slots:
    // SerialWorker::statusChanged（WorkerStatus 数值见 serial_worker.h）。
    void setWorkerStatus(espview::pc::WorkerStatus status, const QString& text);
    // SerialWorker::statsChanged（约每 500ms 一次快照）。
    void setStats(const espview::pc::WorkerStats& stats);
    // diagAdded 行解析合并后的 ESP32 遥测快照。
    void setPhysicalStatus(const espview::display::PhysicalStatus& ps);
    // DisplayRouteMode 数值 0..3 + UiRouterState 数值 0..4（4=kUnavailable；0xFF=未知）
    // + FULL resync 挂起标志（mod 行 / SET_MODE ACK 通道）。
    void setModeState(int mode, int routerState, bool fullResyncPending);
    // 语言切换：只重刷文案，不触碰数据源 / 连接。
    void setUiLanguage(int lang);

private:
    struct Row {
        QLabel* key = nullptr;
        QLabel* value = nullptr;
    };

    QGroupBox* addGroup(const QString& title);
    Row addRow(QGridLayout* grid, int row, const QString& key);
    void setValue(const Row& row, const QString& value, PanelState state);

    void refreshAll();
    void refreshTransport();
    void refreshSession();
    void refreshMode();
    void refreshVirtual();
    void refreshPhysical();
    void refreshRouter();
    void refreshErrors();
    void refreshFullResync();
    QString t(const char* key) const;  // trText(lang_, key) 的 Qt 封装

    QString modeName(int mode) const;
    QString sceneName(uint8_t scene) const;
    QString routerStateText(uint8_t state) const;
    QString sessionStateText(uint8_t state, uint64_t reconnectCount) const;
    QString workerStateText(int status) const;
    QString stateText(PanelState s) const;
    PanelState virtualState() const;
    PanelState physicalState() const;

    // ---- 状态源（GUI 线程独占）----
    int workerStatus_ = 0;  // espview::pc::WorkerStatus 数值
    QString workerText_;
    bool hasStats_ = false;
    espview::pc::WorkerStats stats_;
    espview::display::PhysicalStatus phys_;
    int mode_ = 0;            // DisplayRouteMode 数值；默认 VirtualOnly（Router 默认）
    int routerState_ = 0xFF;  // RouterState 数值；0xFF = 尚无 mod 行
    bool fullResyncPending_ = false;
    UiLang lang_ = UiLang::kEnglish;

    // ---- 分组行 ----
    struct TransportRows {
        Row kind;
        Row state;
        Row detail;
        Row reconnect;
        Row rssiCh;
    };
    TransportRows transport_;

    struct SessionRows {
        Row state;
        Row hello;
        Row ping;
        Row rtt;
    };
    SessionRows session_;

    struct ModeRows {
        Row mode;
    };
    ModeRows displayMode_;

    struct VirtualRows {
        Row app;
        Row resolution;
        Row fps;
        Row frames;
    };
    VirtualRows virtual_;

    struct PhysicalRows {
        Row scene;
        Row state;
        Row oled;
        Row errors;
        Row heap;
    };
    PhysicalRows physical_;

    struct RouterRows {
        Row state;
    };
    RouterRows router_;

    struct ErrorsRows {
        Row oled;
        Row protocol;
        Row dropped;
        Row queue;
    };
    ErrorsRows errors_;

    struct ResyncRows {
        Row state;
    };
    ResyncRows resync_;
};

}  // namespace pc
}  // namespace espview
