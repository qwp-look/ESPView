// ESPView M7-C3 — LanguageSelector：语言选择下拉框（English / 中文）。
//
// 规范来源：任务书 §十四（i18n UI 控件）+ §二十二 11-14。
//
// 职责（最小机制，不用 QTranslator / .ts / .qm）：
//   - 下拉框两项：English（0）、中文（1），索引与 UiLang 数值一致；
//   - 用户选择或 setLanguage() 值变化时发出 languageChanged(int)；
//   - 不保存任何持久化状态——语言持久化由主集成层负责
//     （QSettings key "ui/language"），本控件不接触 QSettings；
//   - 切换语言只触发主窗口 retranslate UI；本控件不触碰
//     transport / display / framebuffer。

#pragma once

#include <QComboBox>

namespace espview {
namespace pc {

class LanguageSelector : public QComboBox {
    Q_OBJECT
public:
    explicit LanguageSelector(QWidget* parent = nullptr);

    // 当前语言索引（0=English，1=中文）；未选择时默认 English。
    int currentLanguage() const { return currentIndex(); }

public slots:
    // 程序化设置语言（如启动时从 QSettings 恢复 "ui/language"）。
    // 值变化时经 languageChanged 通知集成层；值相同则不重发（避免重入循环）。
    void setLanguage(int lang);

signals:
    // 0=English，1=中文（与 UiLang::kEnglish / kChinese 数值一致）。
    void languageChanged(int lang);

private:
    // 把 QComboBox::currentIndexChanged 转发为 languageChanged（只转发 0/1）。
    void forwardLanguage(int index);
};

}  // namespace pc
}  // namespace espview
