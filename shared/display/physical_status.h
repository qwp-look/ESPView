// ESPView M7-C3 — PhysicalStatus：ESP32 物理显示/系统遥测快照（纯 C++17，零依赖）。
//
// 定位：把 ESP32 经 GUI diagAdded（ERROR 文本通道）上报的遥测行映射成 GUI
// 显示面板可读的字段快照。非 wire 格式：本文件不定义/解析任何协议字段，
// 只解析下面 6 类 ERROR 文本行（M7-B/C2 实机已产出，格式与 esp32/main/main.cpp
// 及 lvgl_port reportStats() 的 snprintf 输出逐字段对齐）：
//   oled a=0x3C c=SSD1306 err=0 ok=1       （OLED 地址/控制器/错误数/健康）
//   trx tr=0 st=2 sw=0 rc=0 tx=0 rx=0 rssi=-24 ch=6 （传输诊断；只取 rssi/ch）
//   mem h=167008 lg=110592 mn=155160       （free heap / largest / min free）
//   disp id=4 t=0 r=2 b=30720 e=2405 f=0.00 d=1 q=334 （帧统计，fps 定点 0.01）
//   sess st=3 h=1/1 p=9/10                 （会话态 + HELLO/PING 计数）
//   mod sw=3 st=3 scene=0                  （路由模式/路由状态/物理场景）
//
// 解析原则：
//   - 首 token 匹配 6 类前缀之一 → true，并按行语义合并进 out（未知 k=v 键
//     静默忽略，容错）；首 token 不匹配 → false，out 保持不变、不报错；
//   - 数字全部防溢出 clamp（无符号解析溢出 → UINT64_MAX，再按目标字段窄化）；
//   - 语义枚举（mode/routerState/scene）越界值映射为 0xFF（未知），避免把
//     非法值当作正常状态显示。
//
// 线程/所有权：纯值类型，无锁无分配；GUI 线程独占消费，不改变底层
// WorkerStats / shared/protocol 结构（只读复用）。

#pragma once

#include <cstdint>
#include <string_view>

namespace espview {
namespace display {

// OLED 控制器代码（数值对齐 shared/oled ControllerType：0=AUTO 1=SSD1306
// 2=SH1106；0xFF=未知）。不引入 shared/oled 依赖（本文件零依赖）。
enum class OledControllerCode : uint8_t {
    kAuto = 0,
    kSsd1306 = 1,
    kSh1106 = 2,
    kUnknown = 0xFF,
};

// 控制器代码 → 名称（"SSD1306"/"SH1106"/"AUTO"/"UNKNOWN"，供 GUI 显示）。
const char* controllerCodeName(OledControllerCode c);

// 物理遥测快照（值语义；每组一个 valid 标志，GUI 据此区分「无数据」与
// 「真实值」——任务书 §十一 Unavailable 判定）。字段数值语义与来源行一致：
//   - sessionState：proto::SessionState 数值（0..3，对端会话态；0=Disconnected
//     1=Connecting 2=Handshake 3=Connected）；
//   - helloOk：双向 HELLO 已交换（txHello>0 && rxHello>0，来自 sess h=tx/rx）；
//   - pingOk：收到对端 PING（rxPing>0，来自 sess p=tx/rx；对端存活）；
//   - mode：DisplayRouteMode 数值 0..3（越界 clamp 到 3）；
//   - routerState：RouterState 数值 0..3；0xFF = 无 router / 未知（st=-1 或越界）；
//   - physicalScene：PhysicalScene 数值 0=Diagnostics 1=Application；0xFF = 未知。
struct PhysicalStatus {
    // ---- oled（"oled a= c= err= ok="）----
    uint8_t oledAddress = 0;                 // I2C 7-bit 地址（0x3C 等）
    OledControllerCode oledController = OledControllerCode::kUnknown;
    uint64_t oledErrCount = 0;               // OLED 错误计数（clamp 防溢出）
    bool oledOk = false;                     // 1 = OLED 健康

    // ---- trx（"trx tr= st= sw= rc= tx= rx= rssi= ch="）----
    int8_t rssiDbm = -128;                   // RSSI（-128 = 无信号/无测量）
    uint8_t channel = 0;                     // Wi-Fi 信道（0 = 无）

    // ---- mem（"mem h= lg= mn="）----
    uint64_t heapFree = 0;                   // 空闲堆
    uint64_t heapLargest = 0;                // 最大连续块
    uint64_t heapMinFree = 0;                // 历史最小空闲堆

    // ---- disp（"disp id= t= r= b= e= f= d= q="）----
    uint16_t lastFrameId = 0;
    uint8_t lastFrameType = 0;               // 0=FULL 1=PARTIAL
    uint32_t lastRectCount = 0;              // 最近一帧 RECT 数
    uint32_t lastFrameBytes = 0;             // 最近一帧像素字节
    uint64_t lastFrameElapsedMs = 0;         // 最近一帧 BEGIN→END 发送耗时
    uint32_t fpsHundredths = 0;              // 帧率定点 0.01fps（f=0.00 → 0）
    uint64_t framesDropped = 0;
    uint64_t queueFullEvents = 0;

    // ---- sess（"sess st= h= p="）----
    uint8_t sessionState = 0;                // 对端会话态（proto::SessionState 数值）
    bool helloOk = false;
    bool pingOk = false;

    // ---- mod（"mod sw= st= scene="）----
    uint8_t mode = 0;                        // DisplayRouteMode 0..3（clamp）
    uint8_t routerState = 0;                 // RouterState 0..3；0xFF=无/未知
    uint8_t physicalScene = 0;               // PhysicalScene 0/1；0xFF=未知

    // ---- 行源 valid 标志（false = 该行类型尚未收到任何数据）----
    bool oledValid = false;
    bool transportValid = false;
    bool memValid = false;
    bool displayValid = false;
    bool sessionValid = false;
    bool modeValid = false;

    bool anyValid() const {
        return oledValid || transportValid || memValid || displayValid ||
               sessionValid || modeValid;
    }
};

// 解析一行 ERROR 文本遥测（见文件头 6 类格式）并合并进 out（仅更新该行所属
// 字段组与 valid 标志；不触碰其它组）。首 token 不匹配返回 false 且 out 不变；
// 匹配但字段缺失/非法 → 该字段保持原值（容错）。数字 clamp 防溢出。
bool parsePhysicalStatusLine(std::string_view line, PhysicalStatus& out);

// 快照合并：把 src 中 valid 的字段组整体覆盖到 dst（同组逐字段拷贝）；
// src 中未生效的组不改变 dst。用于多行解析后累积到同一快照。
void mergePhysicalStatus(const PhysicalStatus& src, PhysicalStatus& dst);

}  // namespace display
}  // namespace espview
