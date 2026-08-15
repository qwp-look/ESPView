// ESPView M7-C3 — SplitState：Split Drawer 的纯 C++17 状态模型（零依赖）。
//
// 职责（任务书 §二十二 15–18）：
//   - drawer 开/合（drawerVisible）+ 宽度（合理范围 200..560）；
//   - 转移：open() / close() / toggle() / setDrawerVisible() /
//     setDrawerWidth()（范围夹取）/ resize()（setDrawerWidth 别名）；
//   - 持久化：toSettingsMap() / fromSettingsMap() 返回纯数据键值对
//     （std::string 值，不依赖 Qt / QSettings），键名固定为
//     "split/drawerVisible"、"split/drawerWidth"。
//
// Qt 层（pc/src/split_drawer.*）只负责 QSettings 桥接；本模型不接触 Qt，
// ESP32 侧（如需要）也可直接复用。默认状态：收起 + 默认宽度（320）；
// 进入 Split 模式由主窗口显式 open()（避免 VirtualOnly 下意外遮挡）。

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace espview {
namespace display {

class SplitState {
public:
    // Drawer 宽度合理范围（任务书 §二十二：200..560）。
    static constexpr int kMinDrawerWidth = 200;
    static constexpr int kMaxDrawerWidth = 560;
    static constexpr int kDefaultDrawerWidth = 320;

    // QSettings 键名（与 toSettingsMap() / fromSettingsMap() 保持一致；
    // 这两个键均不含凭据，可安全持久化）。
    static constexpr const char* kKeyDrawerVisible = "split/drawerVisible";
    static constexpr const char* kKeyDrawerWidth = "split/drawerWidth";

    // 纯数据键值对：key -> string 值（"1"/"0" 与十进制宽度）。
    using SettingsMap = std::vector<std::pair<std::string, std::string>>;

    bool drawerVisible() const { return visible_; }
    int drawerWidth() const { return width_; }

    // ---- 开/合转移（§22.15）：宽度跨开合保留 ----
    void open() { setDrawerVisible(true); }
    void close() { setDrawerVisible(false); }
    void toggle() { setDrawerVisible(!visible_); }
    void setDrawerVisible(bool visible) { visible_ = visible; }

    // ---- resize 转移（§22.16）：按 [kMinDrawerWidth, kMaxDrawerWidth] 夹取 ----
    void setDrawerWidth(int width) { width_ = clampWidth(width); }
    void resize(int width) { setDrawerWidth(width); }

    // ---- 持久化（§22.17–18）：纯数据键值对，未知键忽略 ----
    // 序列化：
    //   "split/drawerVisible" -> "1" / "0"
    //   "split/drawerWidth"   -> 十进制字符串（已夹取）
    SettingsMap toSettingsMap() const;

    // 反序列化：未知键忽略；非法值忽略（保持当前状态）；可解析但越界的宽度
    // 按范围夹取。返回是否应用了任何键（空 map / 全未知键 -> false）。
    bool fromSettingsMap(const SettingsMap& map);

    // Qt 层共用：宽度夹取。
    static int clampWidth(int width);

private:
    bool visible_ = false;  // 保守默认：收起；Split 模式进入由主窗口显式 open()
    int width_ = kDefaultDrawerWidth;
};

}  // namespace display
}  // namespace espview
