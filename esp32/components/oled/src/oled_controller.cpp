// ESPView M7-A — 控制器解析与命令分发实现。
#include "oled_controller.h"

namespace espview {
namespace oled {

ControllerType resolveController(ControllerType configured) {
    return configured == ControllerType::kAuto ? ControllerType::kSsd1306 : configured;
}

bool executeInitSequence(OledI2c& i2c, ControllerType ctrl, size_t maxSegmentBytes,
                         const std::function<bool()>& stopRequested,
                         esp_err_t* firstError) {
    if (firstError != nullptr) {
        *firstError = ESP_OK;
    }
    const std::vector<uint8_t> cmds = initCommands(ctrl);
    const auto cmdSegs = segmentCommands(cmds, maxSegmentBytes);
    for (const auto& seg : cmdSegs) {
        // M7-B：每段 transmit 前检查停止谓词 —— stop() 后不再发送下一段。
        if (stopRequested && stopRequested()) {
            return false;
        }
        const esp_err_t err = i2c.transmit(seg.data(), seg.size());
        if (err != ESP_OK) {
            if (firstError != nullptr) {
                *firstError = err;
            }
            return false;
        }
    }
    // 清屏：GDDRAM 随机内容 → 全 0。
    OledFb clearFb;
    const auto dataSegs = segmentFrameUpload(clearFb, ctrl, maxSegmentBytes);
    for (const auto& seg : dataSegs) {
        // M7-B：清屏段同样逐段检查停止谓词。
        if (stopRequested && stopRequested()) {
            return false;
        }
        const esp_err_t err = i2c.transmit(seg.data(), seg.size());
        if (err != ESP_OK) {
            if (firstError != nullptr) {
                *firstError = err;
            }
            return false;
        }
    }
    return true;
}

}  // namespace oled
}  // namespace espview
