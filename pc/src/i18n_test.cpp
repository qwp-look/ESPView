// ESPView M7-C3 — i18n host tests（独立可执行 main，纯 C++17，零 Qt / 零 COM3 依赖）。
//
// 规范来源：任务书 §二十二 11-14：
//   11. English 语言返回英文文案；
//   12. Chinese 语言返回中文文案（任务书 §十四 词汇，中文自然通顺）；
//   13. 语言切换是纯函数：切换只改变 lookup 结果、不持有任何状态——断言
//       任意调用顺序 / 交错调用结果一致、重复调用幂等、uiKeys() 稳定；
//       模块没有"当前语言"全局状态（语言始终作为参数传入）；
//   14. 全部必需 key 两种语言均有非空翻译（遍历 key 清单断言，并钉死
//       §十四 必需词条的中英对照）。
//
//   g++ -std=c++17 -Wall -Wextra -Wpedantic -I pc/src pc/src/i18n.cpp pc/src/i18n_test.cpp -o build/i18n_test

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "i18n.h"

namespace {

using espview::pc::UiLang;
using espview::pc::trText;
using espview::pc::uiKeys;

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

// §十四 必需词条 + M7-C 模式名：钉死中英对照（若目录与此表不一致，测试失败）。
struct RequiredEntry {
    const char* en;
    const char* zh;
};

const RequiredEntry kRequired[] = {
    // 任务书 §十四 核心词汇
    {"Main Title", "主标题"},
    {"Transport", "传输"},
    {"Display", "显示"},
    {"Connection", "连接"},
    {"Session", "会话"},
    {"Virtual", "虚拟"},
    {"Physical", "物理"},
    {"Mirror", "镜像"},
    {"Split", "分屏"},
    {"Application", "应用"},
    {"Diagnostics", "诊断"},
    {"Connected", "已连接"},
    {"Disconnected", "已断开"},
    {"Reconnecting", "重连中"},
    {"Switching", "切换中"},
    {"Ready", "就绪"},
    {"Degraded", "降级"},
    {"Error", "错误"},
    {"Apply", "应用"},
    {"Cancel", "取消"},
    {"Settings", "设置"},
    {"Language", "语言"},
    {"Wi-Fi", "Wi-Fi"},
    {"Wizard", "向导"},
    {"Save", "保存"},
    {"Close", "关闭"},
    {"Retry", "重试"},
    // M7-C 显示模式名
    {"Virtual Only", "仅虚拟显示"},
    {"Physical Only", "仅物理显示"},
    // M7-C3 状态面板/抽屉扩展词条（钉死防目录缺词）
    {"RSSI", "RSSI"},
    {"Channel", "信道"},
    {"Heap", "堆"},
    {"Frame", "帧"},
    {"Errors", "错误"},
    {"Success", "成功"},
    {"Failure", "失败"},
    {"Active", "正常"},
    {"Disabled", "已禁用"},
    {"Unavailable", "不可用"},
    {"Router", "路由"},
    {"FULL resync", "全帧重同步"},
    {"ESP32 Physical / Diagnostics", "ESP32 物理 / 诊断"},
    {"Waiting for connection", "等待连接"},
};

// 11. English 返回英文（key 即英文原文；未命中回退英文原文）。
void test11English() {
    std::printf("[11] English returns English\n");
    for (const RequiredEntry& e : kRequired) {
        CHECK(std::strcmp(trText(UiLang::kEnglish, e.en), e.en) == 0);
    }
    // 遍历全部 key：English 恒等于 key 本身
    for (const std::string& k : uiKeys()) {
        CHECK(std::strcmp(trText(UiLang::kEnglish, k.c_str()), k.c_str()) == 0);
    }
    // 未命中 key → 英文原文（返回 key 本身）
    CHECK(std::strcmp(trText(UiLang::kEnglish, "no.such.key"), "no.such.key") == 0);
    CHECK(std::strcmp(trText(UiLang::kChinese, "no.such.key"), "no.such.key") == 0);
}

// 12. Chinese 返回中文（§十四 词条全部钉死对照）。
void test12Chinese() {
    std::printf("[12] Chinese returns Chinese\n");
    for (const RequiredEntry& e : kRequired) {
        CHECK(std::strcmp(trText(UiLang::kChinese, e.en), e.zh) == 0);
    }
    // 中英混写词条仍须有译文（与本表一致）
    CHECK(std::strcmp(trText(UiLang::kChinese, "Wi-Fi"), "Wi-Fi") == 0);
    // 模式名抽查（自然中文）
    CHECK(std::strcmp(trText(UiLang::kChinese, "Mirror"), "镜像") == 0);
    CHECK(std::strcmp(trText(UiLang::kChinese, "Split"), "分屏") == 0);
}

// 13. 语言切换是纯函数：无全局状态、只改变 lookup 结果。
//     本模块不持有 transport / display / framebuffer；此处断言：
//       a) 交替 / 任意顺序调用结果一致（无顺序依赖）；
//       b) 重复调用幂等（无状态累积）；
//       c) uiKeys() 多次调用稳定（目录不被调用副作用修改）；
//       d) "切换语言"可观测效果仅剩返回值差异（同 key 两种语言结果对比）。
void test13LanguageSwitchIsPure() {
    std::printf("[13] language switch is a pure function (no state)\n");

    // a) 交错调用：先 zh 后 en 与先 en 后 zh 结果一致
    for (const RequiredEntry& e : kRequired) {
        const char* zhFirst = trText(UiLang::kChinese, e.en);
        const char* enAfter = trText(UiLang::kEnglish, e.en);
        const char* enFirst = trText(UiLang::kEnglish, e.en);
        const char* zhAfter = trText(UiLang::kChinese, e.en);
        CHECK(std::strcmp(zhFirst, e.zh) == 0);
        CHECK(std::strcmp(zhAfter, e.zh) == 0);
        CHECK(std::strcmp(enFirst, e.en) == 0);
        CHECK(std::strcmp(enAfter, e.en) == 0);
    }

    // b) 重复调用幂等：连续 64 次同一语言结果不变
    for (const RequiredEntry& e : kRequired) {
        const char* first = trText(UiLang::kChinese, e.en);
        bool stable = true;
        for (int i = 0; i < 64; ++i) {
            if (std::strcmp(trText(UiLang::kChinese, e.en), first) != 0) {
                stable = false;
            }
        }
        CHECK(stable);
    }

    // c) uiKeys() 稳定：重复调用返回相同键集（大小与内容不变）
    const std::vector<std::string>& keys1 = uiKeys();
    const std::vector<std::string>& keys2 = uiKeys();
    CHECK(&keys1 == &keys2);  // 返回同一 static 实例
    CHECK(keys1.size() == keys2.size());
    bool sameContents = true;
    for (size_t i = 0; i < keys1.size(); ++i) {
        if (keys1[i] != keys2[i]) {
            sameContents = false;
        }
    }
    CHECK(sameContents);

    // d) 语言只改变 lookup 结果：同一 key 的 zh 结果稳定且可复现；
    //    语言作为参数传入，模块无"当前语言"可变状态。
    const char* zhA = trText(UiLang::kChinese, "Transport");
    const char* zhB = trText(UiLang::kChinese, "Transport");
    const char* enA = trText(UiLang::kEnglish, "Transport");
    CHECK(std::strcmp(zhA, zhB) == 0);
    CHECK(std::strcmp(zhA, "传输") == 0);
    CHECK(std::strcmp(enA, "Transport") == 0);
    CHECK(std::strcmp(zhA, enA) != 0);  // 同一 key 两种语言结果不同（可翻译词条）
}

// 14. 全部必需 key 两种语言均有非空翻译（遍历 key 清单断言）。
void test14AllKeysBothLanguages() {
    std::printf("[14] all keys have non-empty translations in both languages\n");
    const std::vector<std::string>& keys = uiKeys();
    CHECK(keys.size() >= 30u);  // 任务书：至少 30 个 key
    for (const std::string& k : keys) {
        const char* en = trText(UiLang::kEnglish, k.c_str());
        const char* zh = trText(UiLang::kChinese, k.c_str());
        CHECK(en != nullptr && en[0] != '\0');
        CHECK(zh != nullptr && zh[0] != '\0');
    }
    // 必需词条必须全部在 key 清单里（防目录缺词）
    for (const RequiredEntry& e : kRequired) {
        bool found = false;
        for (const std::string& k : keys) {
            if (k == e.en) {
                found = true;
                break;
            }
        }
        CHECK(found);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== ESPView pc i18n host tests ==\n");
    test11English();
    test12Chinese();
    test13LanguageSwitchIsPure();
    test14AllKeysBothLanguages();
    std::printf("i18n_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
