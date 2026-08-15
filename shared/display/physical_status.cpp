// ESPView M7-C3 — PhysicalStatus 解析/合并实现（见 physical_status.h）。
// 纯 C++17，零平台依赖；错误路径不抛异常、不输出错误日志。

#include "physical_status.h"

#include <cstdint>
#include <limits>

namespace espview {
namespace display {

namespace {

constexpr uint64_t kU64Max = std::numeric_limits<uint64_t>::max();

// 去除前导空白（space/tab/CR/LF）。
std::string_view trimLeft(std::string_view s) {
    size_t i = 0;
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
        ++i;
    }
    return s.substr(i);
}

// 取下一个空白分隔 token；剩余部分写入 rest。空串时返回空 token。
std::string_view nextToken(std::string_view s, std::string_view& rest) {
    s = trimLeft(s);
    size_t i = 0;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' &&
           s[i] != '\n') {
        ++i;
    }
    rest = s.substr(i);
    return s.substr(0, i);
}

// 无符号十进制解析，溢出 clamp 到 UINT64_MAX（防溢出）。非法字符 → false。
bool parseU64Clamp(std::string_view s, uint64_t& out) {
    if (s.empty()) {
        return false;
    }
    uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (kU64Max - d) / 10u) {
            v = kU64Max;  // 溢出：clamp，继续吃掉剩余数字
            continue;
        }
        v = v * 10u + d;
    }
    out = v;
    return true;
}

// 有符号十进制解析（可带 +/-），clamp 到 INT64_MIN..INT64_MAX。
bool parseI64Clamp(std::string_view s, int64_t& out) {
    bool neg = false;
    if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
        neg = (s.front() == '-');
        s.remove_prefix(1);
    }
    uint64_t mag = 0;
    if (!parseU64Clamp(s, mag)) {
        return false;
    }
    if (neg) {
        constexpr uint64_t kMinMag = 0x8000000000000000ULL;  // |INT64_MIN|
        if (mag > kMinMag) {
            mag = kMinMag;
        }
        out = (mag == kMinMag) ? std::numeric_limits<int64_t>::min()
                               : -static_cast<int64_t>(mag);
    } else {
        if (mag > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            mag = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        }
        out = static_cast<int64_t>(mag);
    }
    return true;
}

// 十六进制解析（可选 0x/0X 前缀，大小写不敏感），clamp 到 UINT64_MAX。
bool parseHexClamp(std::string_view s, uint64_t& out) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
    }
    if (s.empty()) {
        return false;
    }
    uint64_t v = 0;
    for (const char c : s) {
        uint64_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint64_t>(c - 'a' + 10u);
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint64_t>(c - 'A' + 10u);
        } else {
            return false;
        }
        if (v > (kU64Max - d) / 16u) {
            v = kU64Max;
            continue;
        }
        v = v * 16u + d;
    }
    out = v;
    return true;
}

// 无符号窄化 clamp（uint64 → 目标类型，防溢出）。
template <typename T>
T clampToU(uint64_t v) {
    const uint64_t max = static_cast<uint64_t>(std::numeric_limits<T>::max());
    return static_cast<T>(v > max ? max : v);
}

// 有符号窄化 clamp（int64 → 目标有符号类型，防溢出，负向正确 clamp）。
template <typename T>
T clampToI(int64_t v) {
    const int64_t min = static_cast<int64_t>(std::numeric_limits<T>::min());
    const int64_t max = static_cast<int64_t>(std::numeric_limits<T>::max());
    if (v < min) {
        return static_cast<T>(min);
    }
    if (v > max) {
        return static_cast<T>(max);
    }
    return static_cast<T>(v);
}

// 定点小数 "X.YY" → 百分位整数（0.00→0；1.25→125；0.5→50）。
// 溢出 clamp 到 uint32 max（fpsHundredths 字段类型）。
bool parseFpsHundredths(std::string_view s, uint32_t& out) {
    const uint64_t limit = std::numeric_limits<uint32_t>::max();
    const size_t dot = s.find('.');
    std::string_view ip = (dot == std::string_view::npos) ? s : s.substr(0, dot);
    uint64_t intPart = 0;
    if (!parseU64Clamp(ip, intPart)) {
        return false;
    }
    uint64_t frac = 0;
    if (dot != std::string_view::npos) {
        const std::string_view fp = s.substr(dot + 1);
        uint64_t place = 10;  // 第一位 = 十分位
        for (size_t i = 0; i < fp.size(); ++i) {
            const char c = fp[i];
            if (c < '0' || c > '9') {
                return false;
            }
            if (i >= 2) {
                break;  // 只取前两位（snprintf "%u.%02u" 保证两位）
            }
            frac += static_cast<uint64_t>(c - '0') * place;
            place = place / 10u;
        }
    }
    if (intPart > (limit - frac) / 100u) {
        out = std::numeric_limits<uint32_t>::max();
    } else {
        out = static_cast<uint32_t>(intPart * 100u + frac);
    }
    return true;
}

// "k=v" token 迭代；无 '=' 的 token 跳过。返回 false 表示 token 耗尽。
struct KeyValue {
    std::string_view key;
    std::string_view value;
};

bool nextKeyValue(std::string_view& s, KeyValue& kv) {
    const std::string_view tok = nextToken(s, s);
    if (tok.empty()) {
        return false;
    }
    const size_t eq = tok.find('=');
    if (eq == std::string_view::npos) {
        return true;  // 跳过无 '=' 的 token，继续
    }
    kv.key = tok.substr(0, eq);
    kv.value = tok.substr(eq + 1);
    return true;
}

OledControllerCode parseController(std::string_view name) {
    if (name == "SSD1306") {
        return OledControllerCode::kSsd1306;
    }
    if (name == "SH1106") {
        return OledControllerCode::kSh1106;
    }
    if (name == "AUTO") {
        return OledControllerCode::kAuto;
    }
    return OledControllerCode::kUnknown;
}

void parseOled(std::string_view rest, PhysicalStatus& out) {
    KeyValue kv;
    while (nextKeyValue(rest, kv)) {
        if (kv.key == "a") {
            uint64_t v = 0;
            if (parseHexClamp(kv.value, v)) {
                out.oledAddress = clampToU<uint8_t>(v);
            }
        } else if (kv.key == "c") {
            out.oledController = parseController(kv.value);
        } else if (kv.key == "err") {
            uint64_t v = 0;
            if (parseU64Clamp(kv.value, v)) {
                out.oledErrCount = v;
            }
        } else if (kv.key == "ok") {
            uint64_t v = 0;
            if (parseU64Clamp(kv.value, v)) {
                out.oledOk = (v != 0u);
            }
        }
        // 未知键：静默忽略（容错）
    }
    out.oledValid = true;
}

void parseTrx(std::string_view rest, PhysicalStatus& out) {
    KeyValue kv;
    while (nextKeyValue(rest, kv)) {
        if (kv.key == "rssi") {
            int64_t v = 0;
            if (parseI64Clamp(kv.value, v)) {
                out.rssiDbm = clampToI<int8_t>(v);
            }
        } else if (kv.key == "ch") {
            uint64_t v = 0;
            if (parseU64Clamp(kv.value, v)) {
                out.channel = clampToU<uint8_t>(v);
            }
        }
        // tr/st/sw/rc/tx/rx 属 Transport 诊断，由 WorkerStats 覆盖；
        // 物理快照只保留 rssi/ch（字段清单外的不重复保存）。
    }
    out.transportValid = true;
}

void parseMem(std::string_view rest, PhysicalStatus& out) {
    KeyValue kv;
    while (nextKeyValue(rest, kv)) {
        uint64_t v = 0;
        if (kv.key == "h") {
            if (parseU64Clamp(kv.value, v)) {
                out.heapFree = v;
            }
        } else if (kv.key == "lg") {
            if (parseU64Clamp(kv.value, v)) {
                out.heapLargest = v;
            }
        } else if (kv.key == "mn") {
            if (parseU64Clamp(kv.value, v)) {
                out.heapMinFree = v;
            }
        }
    }
    out.memValid = true;
}

void parseDisp(std::string_view rest, PhysicalStatus& out) {
    KeyValue kv;
    while (nextKeyValue(rest, kv)) {
        uint64_t v = 0;
        if (kv.key == "id") {
            if (parseU64Clamp(kv.value, v)) {
                out.lastFrameId = clampToU<uint16_t>(v);
            }
        } else if (kv.key == "t") {
            if (parseU64Clamp(kv.value, v)) {
                out.lastFrameType = clampToU<uint8_t>(v);
            }
        } else if (kv.key == "r") {
            if (parseU64Clamp(kv.value, v)) {
                out.lastRectCount = clampToU<uint32_t>(v);
            }
        } else if (kv.key == "b") {
            if (parseU64Clamp(kv.value, v)) {
                out.lastFrameBytes = clampToU<uint32_t>(v);
            }
        } else if (kv.key == "e") {
            if (parseU64Clamp(kv.value, v)) {
                out.lastFrameElapsedMs = v;
            }
        } else if (kv.key == "f") {
            uint32_t fps = 0;
            if (parseFpsHundredths(kv.value, fps)) {
                out.fpsHundredths = fps;
            }
        } else if (kv.key == "d") {
            if (parseU64Clamp(kv.value, v)) {
                out.framesDropped = v;
            }
        } else if (kv.key == "q") {
            if (parseU64Clamp(kv.value, v)) {
                out.queueFullEvents = v;
            }
        }
    }
    out.displayValid = true;
}

void parseSess(std::string_view rest, PhysicalStatus& out) {
    KeyValue kv;
    bool haveHello = false;
    bool havePing = false;
    uint64_t txHello = 0;
    uint64_t rxHello = 0;
    uint64_t txPing = 0;
    uint64_t rxPing = 0;
    while (nextKeyValue(rest, kv)) {
        if (kv.key == "st") {
            uint64_t v = 0;
            if (parseU64Clamp(kv.value, v)) {
                out.sessionState = clampToU<uint8_t>(v);
            }
        } else if (kv.key == "h") {
            const size_t slash = kv.value.find('/');
            if (slash != std::string_view::npos &&
                parseU64Clamp(kv.value.substr(0, slash), txHello) &&
                parseU64Clamp(kv.value.substr(slash + 1), rxHello)) {
                haveHello = true;
            }
        } else if (kv.key == "p") {
            const size_t slash = kv.value.find('/');
            if (slash != std::string_view::npos &&
                parseU64Clamp(kv.value.substr(0, slash), txPing) &&
                parseU64Clamp(kv.value.substr(slash + 1), rxPing)) {
                havePing = true;
            }
        }
    }
    if (haveHello) {
        out.helloOk = (txHello > 0u && rxHello > 0u);
    }
    if (havePing) {
        out.pingOk = (rxPing > 0u);
    }
    out.sessionValid = true;
}

void parseMod(std::string_view rest, PhysicalStatus& out) {
    KeyValue kv;
    while (nextKeyValue(rest, kv)) {
        if (kv.key == "sw") {
            uint64_t v = 0;
            if (parseU64Clamp(kv.value, v)) {
                out.mode = (v > 3u) ? 3u : static_cast<uint8_t>(v);  // 0..3
            }
        } else if (kv.key == "st") {
            int64_t v = 0;
            if (parseI64Clamp(kv.value, v)) {
                out.routerState = (v < 0 || v > 3) ? 0xFFu : static_cast<uint8_t>(v);
            }
        } else if (kv.key == "scene") {
            int64_t v = 0;
            if (parseI64Clamp(kv.value, v)) {
                out.physicalScene = (v == 0 || v == 1) ? static_cast<uint8_t>(v) : 0xFFu;
            }
        }
    }
    out.modeValid = true;
}

}  // namespace

const char* controllerCodeName(OledControllerCode c) {
    switch (c) {
        case OledControllerCode::kAuto:
            return "AUTO";
        case OledControllerCode::kSsd1306:
            return "SSD1306";
        case OledControllerCode::kSh1106:
            return "SH1106";
        case OledControllerCode::kUnknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool parsePhysicalStatusLine(std::string_view line, PhysicalStatus& out) {
    std::string_view rest;
    const std::string_view kind = nextToken(line, rest);
    if (kind == "oled") {
        parseOled(rest, out);
        return true;
    }
    if (kind == "trx") {
        parseTrx(rest, out);
        return true;
    }
    if (kind == "mem") {
        parseMem(rest, out);
        return true;
    }
    if (kind == "disp") {
        parseDisp(rest, out);
        return true;
    }
    if (kind == "sess") {
        parseSess(rest, out);
        return true;
    }
    if (kind == "mod") {
        parseMod(rest, out);
        return true;
    }
    return false;  // 不匹配：out 保持不变
}

void mergePhysicalStatus(const PhysicalStatus& src, PhysicalStatus& dst) {
    if (src.oledValid) {
        dst.oledAddress = src.oledAddress;
        dst.oledController = src.oledController;
        dst.oledErrCount = src.oledErrCount;
        dst.oledOk = src.oledOk;
        dst.oledValid = true;
    }
    if (src.transportValid) {
        dst.rssiDbm = src.rssiDbm;
        dst.channel = src.channel;
        dst.transportValid = true;
    }
    if (src.memValid) {
        dst.heapFree = src.heapFree;
        dst.heapLargest = src.heapLargest;
        dst.heapMinFree = src.heapMinFree;
        dst.memValid = true;
    }
    if (src.displayValid) {
        dst.lastFrameId = src.lastFrameId;
        dst.lastFrameType = src.lastFrameType;
        dst.lastRectCount = src.lastRectCount;
        dst.lastFrameBytes = src.lastFrameBytes;
        dst.lastFrameElapsedMs = src.lastFrameElapsedMs;
        dst.fpsHundredths = src.fpsHundredths;
        dst.framesDropped = src.framesDropped;
        dst.queueFullEvents = src.queueFullEvents;
        dst.displayValid = true;
    }
    if (src.sessionValid) {
        dst.sessionState = src.sessionState;
        dst.helloOk = src.helloOk;
        dst.pingOk = src.pingOk;
        dst.sessionValid = true;
    }
    if (src.modeValid) {
        dst.mode = src.mode;
        dst.routerState = src.routerState;
        dst.physicalScene = src.physicalScene;
        dst.modeValid = true;
    }
}

}  // namespace display
}  // namespace espview
