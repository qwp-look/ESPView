// ESPView M8-C (C1) — IRenderStage: 渲染 stage 契约（pure C++17）。
//
// 定位（task book §七）：每个 stage 明确 input format / output format /
// geometry / 内存契约。pipeline 在 build 期做格式链校验与 scratch 规划。
//
// 内存契约：
//   - StageInput.data 为行主序紧凑缓冲，总字节 >= formatRowStrideBytes(width)*height
//     （kMono1 页式 = width * ceil(height/8)）。
//   - StageOutput.capacityBytes 由调用方/pipeline 提供；stage 写入超过容量
//     必须返回 ok=false（不允许越界写）。
//   - stage 不得持有输入/输出缓冲指针（run 结束后不得引用）。
//   - 增量窗口（fast path）：width/height = 紧凑缓冲像素尺寸（= 窗口尺寸），
//     offsetX/offsetY = 该缓冲在逻辑帧中的原点（逻辑坐标）。通用 stage
//     （亮度/缩放/裁剪/dither）忽略 offset（视为独立图像）。
//   - run 错误路径不使用异常。
#pragma once

#include <cstddef>
#include <cstdint>

#include "render_format.h"

namespace espview {
namespace render {

struct StageInput {
    RenderFormat format = RenderFormat::kRgb565;
    int width = 0;          // 缓冲像素宽（fast path = 窗口宽）
    int height = 0;         // 缓冲像素高（fast path = 窗口高）
    const uint8_t* data = nullptr;
    size_t bytes = 0;       // 可用字节数（>= 本格式所需）
    int offsetX = 0;        // 缓冲在逻辑帧中的原点 X（fast path 用）
    int offsetY = 0;        // 缓冲在逻辑帧中的原点 Y
};

struct StageOutput {
    RenderFormat format = RenderFormat::kMono1;
    int width = 0;
    int height = 0;
    uint8_t* data = nullptr;
    size_t capacityBytes = 0;
};

struct StageResult {
    bool ok = false;
    size_t bytesWritten = 0;  // 本 stage 实际写入的字节数
};

class IRenderStage {
public:
    virtual ~IRenderStage() = default;

    virtual const char* name() const = 0;
    virtual RenderFormat inputFormat() const = 0;
    virtual RenderFormat outputFormat() const = 0;

    // 给定输入几何，计算输出几何；false = 该几何不受支持（build 期发现）。
    virtual bool outputSize(int inW, int inH, int& outW, int& outH) const = 0;

    // 执行本 stage。in.format/out.format 必须与 inputFormat/outputFormat 一致
    // （pipeline build 校验 + run 校验）；输出越界必须返回 ok=false。
    virtual StageResult run(const StageInput& in, StageOutput& out) = 0;
};

}  // namespace render
}  // namespace espview
