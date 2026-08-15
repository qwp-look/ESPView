// ESPView M7-C3 — LanguageSelector 实现（Qt Widgets）。

#include "language_selector.h"

#include <QString>

namespace espview {
namespace pc {

LanguageSelector::LanguageSelector(QWidget* parent)
    : QComboBox(parent) {
    // 两项语言各用其母语显示（通用 i18n 惯例）。
    addItem(QStringLiteral("English"));
    addItem(QStringLiteral("中文"));
    // 先加项后连接：首项加入时（-1→0）的内部 currentIndexChanged 不转发，
    // 避免构造期发出伪 languageChanged。
    connect(this, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &LanguageSelector::forwardLanguage);
}

void LanguageSelector::setLanguage(int lang) {
    if (lang < 0 || lang > 1) {
        return;  // 只接受 English(0) / 中文(1)
    }
    if (lang != currentIndex()) {
        setCurrentIndex(lang);  // 值变化 → currentIndexChanged → languageChanged
    }
}

void LanguageSelector::forwardLanguage(int index) {
    if (index == 0 || index == 1) {
        emit languageChanged(index);
    }
}

}  // namespace pc
}  // namespace espview
