// ESPView M7-C3 — i18n：最小 UI 字符串目录（纯 C++17，零 Qt 依赖）。
//
// 规范来源：任务书 §十四（词汇表，English + 简体中文）+ §二十二 11-14（host 测试）。
//
// 设计（最小机制，不用 .ts/.qm / lrelease）：
//   - key 即英文原文（msgid 风格）：English 语言直接返回 key 本身（M7-E 的 camelCase key 例外，返回其英文文案）；
//   - Chinese 语言经 zh-CN 目录（std::map<std::string,std::string>）查找；
//   - 未命中 key 一律回退英文原文（返回 key 本身），永不返回空串/空指针；
//   - 模块零可变状态：trText 是纯函数（语言作为参数传入），切换语言只改变
//     lookup 结果，不触碰 transport / display / framebuffer。
//   - Qt 侧集成：QString::fromUtf8(trText(lang, key))，无需 Qt Linguist 工具链。

#pragma once

#include <string>
#include <vector>

namespace espview {
namespace pc {

// 支持的语言（0 = English，1 = 中文简体）；数值与 LanguageSelector 下拉索引一致。
enum class UiLang : int {
    kEnglish = 0,
    kChinese = 1,
};

// 返回 key 在 lang 下的 UI 文案。
//   - kEnglish：返回 key 本身（key 即英文原文；M7-E 的 camelCase key 例外，返回其英文文案）；
//   - kChinese：查简体中文目录，未命中回退英文原文（key 本身）；
//   - key 为空指针：返回空串（防御，不崩溃）。
// 无状态、幂等、线程安全：任意调用顺序 / 交错调用结果一致。
const char* trText(UiLang lang, const char* key);

// 全部受支持 key（英文原文，std::map 排序序），供测试遍历与 UI 枚举。
const std::vector<std::string>& uiKeys();

}  // namespace pc
}  // namespace espview
