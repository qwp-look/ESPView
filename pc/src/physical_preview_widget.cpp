// ESPView M7-D2 — PhysicalPreviewWidget 实现（Qt Widgets；GUI 线程独占）。
//
// 渲染：页式 1bpp（OledFb 布局：fb[page*width + x]，bit0=该页顶行，
// y=page*8+bit）→ 手动 Format_ARGB32 2x 放大（亮像素白、背景黑，OLED
// 观感）。只消费 PhysicalPreviewState 快照，不解析 wire。
//
// 性能：位图只在 setFrame 时重建（≤10Hz 协议上限，默认 2Hz）；stale 由
// 1Hz 计时器驱动，只刷新状态文本（值变化才改 label），无 50Hz 重绘。

#include "physical_preview_widget.h"

#include <QColor>
#include <QFormLayout>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <map>
#include <string>

namespace espview {
namespace pc {

namespace {

// 状态配色（与 SplitDrawer 一致：RGB，浅色/深色主题均可读）。
const QColor kColorOk(0x2E, 0x7D, 0x32);    // Live（绿）
const QColor kColorWarn(0xE6, 0x51, 0x00);  // Stale（橙）
const QColor kColorMuted(0x75, 0x75, 0x75); // No Preview / Disabled / 占位（灰）

const char* kPlaceholder = "—";

// 值变化才更新 label（避免 1Hz stale tick 无谓重排）。
void setLabelValue(QLabel* label, const QString& text) {
    if (label != nullptr && label->text() != text) {
        label->setText(text);
    }
}

}  // namespace

QString PhysicalPreviewWidget::t(const char* key) const {
    return QString::fromUtf8(trText(lang_, key));
}

PhysicalPreviewWidget::PhysicalPreviewWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("physicalPreviewWidget"));
    buildUi();

    staleTimer_ = new QTimer(this);
    staleTimer_->setInterval(kStaleCheckMs);
    connect(staleTimer_, &QTimer::timeout, this, &PhysicalPreviewWidget::onStaleTick);
    staleTimer_->start();

    frameClock_.start();
    refreshStatus();
}

void PhysicalPreviewWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    titleLabel_ = new QLabel(QStringLiteral("<b>%1</b>").arg(t("Physical Preview")), this);
    root->addWidget(titleLabel_);

    statusLabel_ = new QLabel(this);
    root->addWidget(statusLabel_);

    // 2x 位图区域：无帧时隐藏（不占空间，不伪造画面）。
    imageLabel_ = new QLabel(this);
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setFrameShape(QFrame::StyledPanel);
    imageLabel_->setMinimumSize(256, 128);  // 128x64 的 2x；更大帧自动扩张
    imageLabel_->hide();
    root->addWidget(imageLabel_, /*stretch*/ 1);

    // 只读元数据：Controller / Resolution / Format（值来自帧或外部，占位 "—"）。
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(2);
    controllerLabel_ = new QLabel(t("Controller"), this);
    resolutionLabel_ = new QLabel(t("Resolution"), this);
    formatLabel_ = new QLabel(t("Format"), this);
    controllerValue_ = new QLabel(kPlaceholder, this);
    resolutionValue_ = new QLabel(kPlaceholder, this);
    formatValue_ = new QLabel(kPlaceholder, this);
    form->addRow(controllerLabel_, controllerValue_);
    form->addRow(resolutionLabel_, resolutionValue_);
    form->addRow(formatLabel_, formatValue_);
    root->addLayout(form);

    refreshStatus();
}

void PhysicalPreviewWidget::setFrame(const PhysicalPreviewState& snapshot) {
    state_ = snapshot;
    // 可用性由快照自述（有像素且 sessionConnected）；disconnected 快照 → 无数据。
    haveFrame_ = snapshot.isAvailable();
    if (haveFrame_) {
        lastFrameAtMs_ = frameClock_.elapsed();
        renderFrame();  // 只有新帧才重建位图（无 50Hz 重绘）
    } else {
        lastFrameAtMs_ = -1;
        if (imageLabel_ != nullptr) {
            imageLabel_->clear();
            imageLabel_->hide();
        }
    }
    refreshStatus();
}

void PhysicalPreviewWidget::clear() {
    // 只清帧与能力元数据：不动使能/语言（断线语义 AE.3：PC 断线清空预览位图；
    // M7-G3：Controller 属会话能力，跨会话不得残留 → 一并复位为占位）。
    const bool enabled = previewEnabled_;
    state_ = PhysicalPreviewState();
    previewEnabled_ = enabled;
    controllerName_.clear();
    haveFrame_ = false;
    lastFrameAtMs_ = -1;
    if (imageLabel_ != nullptr) {
        imageLabel_->clear();
        imageLabel_->hide();
    }
    refreshStatus();
}

void PhysicalPreviewWidget::renderFrame() {
    const int w = static_cast<int>(state_.width());
    const int h = static_cast<int>(state_.height());
    if (w <= 0 || h <= 0) {
        return;  // 防御（模型已拒绝非法几何）
    }
    const size_t needed = (static_cast<size_t>(w) * h + 7u) / 8u;
    if (state_.pixels().size() < needed) {
        return;  // 防御（模型已校验像素长度）
    }
    // 手动 Format_ARGB32 2x 放大：亮像素 2x2 白、背景黑（OLED 观感）。
    // 位图只在 setFrame 时重建（协议上限 10Hz，默认 2Hz，无 50Hz 重绘）。
    QImage image(w * 2, h * 2, QImage::Format_ARGB32);
    image.fill(QColor(Qt::black));
    QPainter painter(&image);
    const auto& pixels = state_.pixels();
    for (int y = 0; y < h; ++y) {
        const int page = y / 8;   // SSD1306 页（8 行一条）
        const int bit = y % 8;    // bit0 = 该页顶行
        for (int x = 0; x < w; ++x) {
            const size_t idx = static_cast<size_t>(page) * static_cast<size_t>(w) + x;
            if (idx < pixels.size() && ((pixels[idx] >> bit) & 1u) != 0u) {
                painter.fillRect(x * 2, y * 2, 2, 2, QColor(Qt::white));
            }
        }
    }
    painter.end();
    imageLabel_->setPixmap(QPixmap::fromImage(image));
    imageLabel_->setVisible(previewEnabled_);
    imageLabel_->update();  // 只有 setFrame 才触发位图区域重绘
}

void PhysicalPreviewWidget::refreshStatus() {
    const char* key = "No Preview";
    QColor color = kColorMuted;
    bool showImage = false;
    if (!previewEnabled_) {
        key = "Preview Disabled";
    } else if (haveFrame_) {
        const bool stale =
            lastFrameAtMs_ >= 0 &&
            (frameClock_.elapsed() - lastFrameAtMs_) > kStaleThresholdMs;  // >1s
        if (stale) {
            key = "Stale";
            color = kColorWarn;
        } else {
            key = "Live";
            color = kColorOk;
        }
        showImage = true;
    }
    // 值变化才改 label（stale tick 1Hz，无 50Hz 无谓重排）。
    if (statusKey_ != QLatin1String(key)) {
        statusLabel_->setText(t(key));
        statusLabel_->setStyleSheet(
            QStringLiteral("color: rgb(%1,%2,%3);")
                .arg(color.red()).arg(color.green()).arg(color.blue()));
        statusKey_ = QString::fromUtf8(key);
    }
    if (imageLabel_ != nullptr) {
        imageLabel_->setVisible(showImage && previewEnabled_);
    }
    // 行名重刷（语言切换；值变化才改 label）。
    setLabelValue(controllerLabel_, t("Controller"));
    setLabelValue(resolutionLabel_, t("Resolution"));
    setLabelValue(formatLabel_, t("Format"));
    // 只读元数据值（Controller 外部传入或占位；Resolution/Format 来自帧）。
    setLabelValue(controllerValue_,
                  controllerName_.isEmpty() ? QString::fromUtf8(kPlaceholder)
                                            : controllerName_);
    if (haveFrame_) {
        setLabelValue(resolutionValue_,
                      QStringLiteral("%1 x %2").arg(state_.width()).arg(state_.height()));
        setLabelValue(formatValue_,
                      state_.pixelFormat() == kPixelFormatMono1
                          ? QStringLiteral("Mono1")
                          : t("Unknown"));
    } else {
        setLabelValue(resolutionValue_, QString::fromUtf8(kPlaceholder));
        setLabelValue(formatValue_, QString::fromUtf8(kPlaceholder));
    }
}

void PhysicalPreviewWidget::onStaleTick() {
    refreshStatus();  // 只刷新状态文本，不重建位图
}

void PhysicalPreviewWidget::setUiLanguage(int lang) {
    lang_ = static_cast<UiLang>(lang);
    if (titleLabel_ != nullptr) {
        titleLabel_->setText(
            QStringLiteral("<b>%1</b>").arg(t("Physical Preview")));
    }
    // 值区域/状态文案重刷；不触碰连接与帧。
    statusKey_.clear();
    refreshStatus();
}

void PhysicalPreviewWidget::setControllerName(const QString& name) {
    controllerName_ = name;
    refreshStatus();
}

void PhysicalPreviewWidget::setEnabled(bool enabled) {
    if (previewEnabled_ == enabled) {
        return;
    }
    previewEnabled_ = enabled;
    if (imageLabel_ != nullptr && !previewEnabled_) {
        imageLabel_->hide();
    }
    refreshStatus();
}

void PhysicalPreviewWidget::loadSettings(QSettings& settings) {
    std::map<std::string, std::string> map;
    const QString key = QString::fromUtf8(kPreviewEnabledSettingsKey);
    if (settings.contains(key)) {
        map[kPreviewEnabledSettingsKey] =
            settings.value(key).toString().toStdString();
    }
    state_.fromSettingsMap(map);  // 缺键/未知值保持当前值；仅此一个键
    setEnabled(state_.previewEnabled());
}

void PhysicalPreviewWidget::saveSettings(QSettings& settings) {
    state_.setPreviewEnabled(previewEnabled_);
    const auto map = state_.toSettingsMap();  // 恰 {ui/previewEnabled: "1"/"0"}
    for (const auto& kv : map) {
        settings.setValue(QString::fromUtf8(kv.first.c_str()),
                          QVariant(kv.second == "1"));
    }
}

QSize PhysicalPreviewWidget::sizeHint() const {
    return QSize(280, 240);
}

}  // namespace pc
}  // namespace espview
