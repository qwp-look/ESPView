// ESPView M7-A — 控制器解析与命令分发（调用 shared/oled 的 oled_cmd 序列）。
//
// AUTO 语义（无回读）：地址探测由 OledI2c::probe() 完成；控制器无法从
// 总线上可靠区分，默认 SSD1306，Kconfig 可强制 SH1106（132x64 GDDRAM）。
#pragma once

#include <cstddef>
#include <functional>

#include "esp_err.h"

#include "oled_cmd.h"  // ControllerType / 分段生成
#include "oled_i2c.h"

namespace espview {
namespace oled {

// AUTO → SSD1306；显式选择原样返回。
ControllerType resolveController(ControllerType configured);

// 执行初始化：init 命令段逐段 transmit + 清屏上传。全部成功返回 true；
// M7-B：stopRequested 为停止谓词 —— 每段 transmit 前调用，返回 true 立即放弃
// （不再发送下一段，快速退出）；默认空谓词 = 不检查。
// firstError（可选）返回第一个非 OK 的 esp_err_t（ESP_OK = 全成功）。
bool executeInitSequence(OledI2c& i2c, ControllerType ctrl, size_t maxSegmentBytes,
                        const std::function<bool()>& stopRequested = {},
                        esp_err_t* firstError = nullptr);

}  // namespace oled
}  // namespace espview
