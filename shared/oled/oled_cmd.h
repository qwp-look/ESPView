// ESPView M7-A — SSD1306/SH1106 控制器命令序列与帧上传分段生成（纯 C++17）。
//
// 零平台依赖：只生成字节序列；I2C 传输由 ESP32 组件层（esp32/components/oled）
// 执行。host 测试直接验证 golden bytes / 分段边界。
//
// 传输约定（SSD1306/SH1106 I2C 协议）：
//   每条 I2C 消息以控制字节开头——0x00 = 后续为命令，0x40 = 后续为 GDDRAM 数据。
//   帧上传可分段：每段 ≤ maxSegmentBytes（含控制字节），≤ I2C 单次 transmit 限制。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "oled_fb.h"

namespace espview {
namespace oled {

// 控制器类型枚举（Kconfig 选择 + 运行时探测结果）。
enum class ControllerType : uint8_t {
    kAuto = 0,     // 运行时探测地址 + 默认 SSD1306（无回读，无法可靠区分）
    kSsd1306 = 1,
    kSh1106 = 2,
};

// 单个 I2C 传输块：首字节为控制字节（0x00=命令 / 0x40=数据），其后为载荷。
using WireSegment = std::vector<uint8_t>;

constexpr uint8_t kCtrlCommand = 0x00;
constexpr uint8_t kCtrlData = 0x40;

// SH1106 的 132 列 GDDRAM：数据起始列偏移（左右各留 2 列，居中 128 列数据）。
constexpr int kSh1106ColumnOffset = 2;

// ---- 命令字节序列（不含控制字节；由 segmentCommands 加 0x00 前缀）----
std::vector<uint8_t> initCommands(ControllerType c);          // 初始化序列（含 display on）
std::vector<uint8_t> displayOnCommands();
std::vector<uint8_t> displayOffCommands();
std::vector<uint8_t> setContrastCommands(uint8_t contrast);

// 把命令字节序列切成 I2C 传输段（每段 ≤ maxSegmentBytes，段首为 0x00）。
std::vector<WireSegment> segmentCommands(const std::vector<uint8_t>& commands,
                                         size_t maxSegmentBytes);

// fb → 帧上传传输段：
//   SSD1306：horizontal 模式，先设列/页地址范围（0x21/0x22），再 0x40 流式上传；
//   SH1106：page 寻址模式，每页 0xB0|page + 0x00|low + 0x10|high（列偏移 2）+ 数据。
//   页顺序保持 0..7；**页内列序反转**（地址列 C ← fb 列 127-C，实机修正镜像）。
// 每段总长 ≤ maxSegmentBytes（含控制字节）。
std::vector<WireSegment> segmentFrameUpload(const OledFb& fb, ControllerType c,
                                            size_t maxSegmentBytes);

// 控制器名（诊断/测试用）。
const char* controllerName(ControllerType c);

}  // namespace oled
}  // namespace espview
