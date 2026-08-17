// ESPView M7-A — SSD1306/SH1106 命令序列与帧上传分段实现。
#include "oled_cmd.h"

#include <algorithm>
#include <cstdint>

namespace espview {
namespace oled {

namespace {

// 公共初始化命令体（SSD1306 与 SH1106 共享；控制器差异在内存模式/charge pump）。
// 顺序见 M7-A spec：charge pump → memory mode → segment remap → COM scan →
// contrast → precharge → VCOMH → display on。
void appendCommonInit(std::vector<uint8_t>& cmds) {
    cmds.push_back(0xAE);            // display off
    cmds.push_back(0xD5); cmds.push_back(0x80);  // display clock divide/osc freq
    cmds.push_back(0xA8); cmds.push_back(0x3F);  // multiplex ratio 1/64
    cmds.push_back(0xD3); cmds.push_back(0x00);  // display offset 0
    cmds.push_back(0x40);            // start line 0
}

void appendTail(std::vector<uint8_t>& cmds) {
    cmds.push_back(0xA0);            // segment remap OFF（无镜像；0xA1 会水平镜像整屏）
    cmds.push_back(0xC8);            // COM scan direction：dec
    cmds.push_back(0xDA); cmds.push_back(0x12);  // COM pins（1/64）
    cmds.push_back(0x81); cmds.push_back(0x7F);  // contrast
    cmds.push_back(0xD9); cmds.push_back(0xF1);  // precharge period
    cmds.push_back(0xDB); cmds.push_back(0x30);  // VCOMH deselect level
    cmds.push_back(0xA4);            // resume to RAM content
    cmds.push_back(0xA6);            // normal（非反显）
    cmds.push_back(0xAF);            // display on
}

// 构建上传数据：**按页反转列序**（页顺序 0..7 保持不变，仅页内 x 反转）。
// 实机修正（2026-08-15）：本模块在 0xA0/0xA1 segment remap 下均呈现水平镜像
// （控制器疑似忽略 remap 命令），按页反转列序可确定性消除镜像——地址列 C
// 接收 fb 列 (127-C)，无论 SEG0 物理位于哪一端，fb 左列都落在屏幕左侧。
//
// M8-A4：页内列序反转收敛为单一 helper（writeReversedPage），SSD1306 整块
// 构建与 SH1106 分段上传共用，消除两份镜像实现。
void writeReversedPage(uint8_t* dst, const uint8_t* page, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        dst[i] = page[width - 1 - i];
    }
}

std::vector<uint8_t> buildFbUploadData(const OledFb& fb) {
    std::vector<uint8_t> out;
    out.resize(OledFb::kSizeBytes);
    for (int p = 0; p < OledFb::kPageCount; ++p) {
        writeReversedPage(out.data() + static_cast<size_t>(p) * static_cast<size_t>(OledFb::kWidth),
                          fb.page(p), static_cast<size_t>(OledFb::kWidth));
    }
    return out;
}

void appendDataSegments(std::vector<WireSegment>& out, const uint8_t* data,
                        size_t len, size_t maxSegmentBytes) {
    const size_t maxPayload = maxSegmentBytes > 1 ? maxSegmentBytes - 1 : 1;
    size_t off = 0;
    while (off < len) {
        const size_t n = std::min(maxPayload, len - off);
        WireSegment seg;
        seg.reserve(n + 1);
        seg.push_back(kCtrlData);
        seg.insert(seg.end(), data + off, data + off + n);
        out.push_back(std::move(seg));
        off += n;
    }
}

}  // namespace

std::vector<uint8_t> initCommands(ControllerType c) {
    std::vector<uint8_t> cmds;
    appendCommonInit(cmds);
    if (c == ControllerType::kSh1106) {
        // SH1106：内部 DC-DC，无 0x8D charge pump 命令；page 寻址
        // （无 0x21/0x22 列/页范围命令，靠 0xB0|page + 0x00/0x10 列起始）。
        cmds.push_back(0x20); cmds.push_back(0x02);  // memory mode: page
    } else {
        cmds.push_back(0x8D); cmds.push_back(0x14);  // charge pump on
        cmds.push_back(0x20); cmds.push_back(0x00);  // memory mode: horizontal
    }
    appendTail(cmds);
    return cmds;
}

std::vector<uint8_t> displayOnCommands() {
    return {0xAF};
}

std::vector<uint8_t> displayOffCommands() {
    return {0xAE};
}

std::vector<uint8_t> setContrastCommands(uint8_t contrast) {
    return {0x81, contrast};
}

std::vector<WireSegment> segmentCommands(const std::vector<uint8_t>& commands,
                                         size_t maxSegmentBytes) {
    std::vector<WireSegment> out;
    const size_t maxPayload = maxSegmentBytes > 1 ? maxSegmentBytes - 1 : 1;
    size_t off = 0;
    while (off < commands.size()) {
        const size_t n = std::min(maxPayload, commands.size() - off);
        WireSegment seg;
        seg.reserve(n + 1);
        seg.push_back(kCtrlCommand);
        seg.insert(seg.end(), commands.begin() + static_cast<long>(off),
                   commands.begin() + static_cast<long>(off + n));
        out.push_back(std::move(seg));
        off += n;
    }
    return out;
}

std::vector<WireSegment> segmentFrameUpload(const OledFb& fb, ControllerType c,
                                            size_t maxSegmentBytes) {
    std::vector<WireSegment> out;
    const size_t maxPayload = maxSegmentBytes > 1 ? maxSegmentBytes - 1 : 1;

    if (c == ControllerType::kSh1106) {
        // SH1106：132x64 GDDRAM，page 寻址。每页：设页 + 设列起始（偏移 2）
        // （0x00|low / 0x10|high），再 0x40 写 128 字节。
        for (int p = 0; p < OledFb::kPageCount; ++p) {
            const uint8_t colStart = static_cast<uint8_t>(kSh1106ColumnOffset);
            WireSegment cmd;
            cmd.push_back(kCtrlCommand);
            cmd.push_back(static_cast<uint8_t>(0xB0 | p));
            cmd.push_back(static_cast<uint8_t>(colStart & 0x0F));
            cmd.push_back(static_cast<uint8_t>(0x10 | ((colStart >> 4) & 0x0F)));
            out.push_back(std::move(cmd));

            // M8-A4：列序反转统一走 writeReversedPage（与 SSD1306 整块同
            // helper）；128B 栈缓冲，无堆分配。
            uint8_t reversedPage[OledFb::kWidth];
            writeReversedPage(reversedPage, fb.page(p),
                              static_cast<size_t>(OledFb::kWidth));
            size_t off = 0;
            while (off < static_cast<size_t>(OledFb::kWidth)) {
                const size_t n = std::min(maxPayload,
                                          static_cast<size_t>(OledFb::kWidth) - off);
                WireSegment seg;
                seg.reserve(n + 1);
                seg.push_back(kCtrlData);
                seg.insert(seg.end(), reversedPage + off, reversedPage + off + n);
                out.push_back(std::move(seg));
                off += n;
            }
        }
        return out;
    }

    // SSD1306：horizontal 模式。先设列范围 0..127 与页范围 0..7，
    // 再 0x40 流式上传整块（写完指针自动回绕）。
    WireSegment range;
    range.push_back(kCtrlCommand);
    range.push_back(0x21); range.push_back(0x00); range.push_back(0x7F);
    range.push_back(0x22); range.push_back(0x00); range.push_back(0x07);
    out.push_back(std::move(range));

    const std::vector<uint8_t> data = buildFbUploadData(fb);
    appendDataSegments(out, data.data(), data.size(), maxSegmentBytes);
    return out;
}

const char* controllerName(ControllerType c) {
    switch (c) {
        case ControllerType::kSsd1306: return "SSD1306";
        case ControllerType::kSh1106: return "SH1106";
        case ControllerType::kAuto: return "AUTO";
    }
    return "UNKNOWN";
}

}  // namespace oled
}  // namespace espview
