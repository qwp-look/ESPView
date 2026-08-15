// ESPView M7-C3 — SplitState 实现（纯 C++17，零平台依赖）。
//
// 持久化键值对与 Qt 解耦：值一律为 std::string，由 Qt 层
// （pc/src/split_drawer.cpp）做 QSettings 桥接，不在此引入 Qt。

#include "split_state.h"

#include <algorithm>
#include <cctype>

namespace espview {
namespace display {

namespace {

// 宽松布尔解析："1"/"true"/"yes"/"on" -> true；"0"/"false"/"no"/"off" -> false；
// 其余（空串/未知）-> false 且不修改 out。
bool parseBool(const std::string& s, bool& out) {
    std::string lower;
    lower.reserve(s.size());
    for (const char c : s) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
        out = true;
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
        out = false;
        return true;
    }
    return false;
}

// 无异常十进制整数解析（允许前导 +/-）；溢出/非数字/空串 -> false。
bool parseWidth(const std::string& s, int& out) {
    if (s.empty()) {
        return false;
    }
    std::size_t i = 0;
    bool negative = false;
    if (s[i] == '+') {
        ++i;
    } else if (s[i] == '-') {
        negative = true;
        ++i;
    }
    if (i == s.size()) {
        return false;
    }
    long long value = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
        if (value > 1000000) {  // 远超合法范围，直接拒绝（防溢出）
            return false;
        }
    }
    out = negative ? static_cast<int>(-value) : static_cast<int>(value);
    return true;
}

}  // namespace

int SplitState::clampWidth(int width) {
    return std::max(kMinDrawerWidth, std::min(kMaxDrawerWidth, width));
}

SplitState::SettingsMap SplitState::toSettingsMap() const {
    return {
        {kKeyDrawerVisible, visible_ ? "1" : "0"},
        {kKeyDrawerWidth, std::to_string(width_)},
    };
}

bool SplitState::fromSettingsMap(const SettingsMap& map) {
    bool applied = false;
    for (const auto& kv : map) {
        if (kv.first == kKeyDrawerVisible) {
            bool v = false;
            if (parseBool(kv.second, v)) {
                visible_ = v;
                applied = true;
            }
        } else if (kv.first == kKeyDrawerWidth) {
            int w = 0;
            if (parseWidth(kv.second, w)) {
                width_ = clampWidth(w);
                applied = true;
            }
        }
        // 未知键忽略（与其它配置键共存，不做全量覆盖）。
    }
    return applied;
}

}  // namespace display
}  // namespace espview
