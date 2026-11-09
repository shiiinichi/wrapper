#include "format_bridge.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>

namespace native_poc {
namespace {
// Android AAPCS64 va_list. Darwin's va_list is a different type and is never
// passed a guest object. Register arguments are captured by guest_call.S.
struct Arguments {
    const unsigned char* stack;
    const unsigned char* general_top;
    const unsigned char* vector_top;
    int general_offset, vector_offset;
    uint64_t integer() {
        uint64_t value;
        if (general_offset < 0) { std::memcpy(&value, general_top + general_offset, 8); general_offset += 8; }
        else { std::memcpy(&value, stack, 8); stack += 8; }
        return value;
    }
    double floating() {
        double value;
        if (vector_offset < 0) { std::memcpy(&value, vector_top + vector_offset, 8); vector_offset += 16; }
        else { std::memcpy(&value, stack, 8); stack += 8; }
        return value;
    }
};
[[noreturn]] void unsupported(const char* format) {
    std::fprintf(stderr, "native-poc: unsupported Android format: %s\n", format); _exit(86);
}
template<class T> void append(std::string& text, const std::string& spec, T value) {
    int size = std::snprintf(nullptr, 0, spec.c_str(), value);
    if (size < 0 || size > 1024 * 1024 || text.size() + size > 1024 * 1024) unsupported(spec.c_str());
    std::vector<char> buffer(static_cast<size_t>(size) + 1);
    std::snprintf(buffer.data(), buffer.size(), spec.c_str(), value);
    text.append(buffer.data(), size);
}
std::string format(const char* source, Arguments args) {
    if (!source) unsupported("null");
    std::string text;
    for (const char* p = source; *p;) {
        if (*p != '%') { text += *p++; continue; }
        const char* start = p++;
        if (*p == '%') { text += *p++; continue; }
        std::string spec = "%";
        while (*p && std::strchr("-+ #0'", *p)) spec += *p++;
        if (*p == '*') {
            int width = static_cast<int>(args.integer());
            if (width < 0) { spec += '-'; width = width == INT32_MIN ? 1000001 : -width; }
            if (width > 1000000) unsupported(source);
            spec += std::to_string(width); ++p;
        } else while (std::isdigit(static_cast<unsigned char>(*p))) spec += *p++;
        if (*p == '.') {
            ++p;
            if (*p == '*') { int precision = static_cast<int>(args.integer()); if (precision >= 0) spec += "." + std::to_string(precision); ++p; }
            else { spec += '.'; while (std::isdigit(static_cast<unsigned char>(*p))) spec += *p++; }
        }
        std::string length;
        if (*p && std::strchr("hljztL", *p)) { length += *p++; if ((*p == 'h' || *p == 'l') && *p == length[0]) length += *p++; }
        char type = *p++;
        if (type == 'd' || type == 'i') {
            auto value = static_cast<int64_t>(args.integer());
            if (length.empty()) value = static_cast<int>(value);
            else if (length == "h") value = static_cast<int16_t>(value);
            else if (length == "hh") value = static_cast<int8_t>(value);
            else if (length != "l" && length != "ll" && length != "j" && length != "z" && length != "t") unsupported(source);
            append(text, spec + "ll" + type, static_cast<long long>(value));
        } else if (type && std::strchr("uoxX", type)) {
            auto value = args.integer();
            if (length.empty()) value = static_cast<unsigned>(value);
            else if (length == "h") value = static_cast<uint16_t>(value);
            else if (length == "hh") value = static_cast<uint8_t>(value);
            else if (length != "l" && length != "ll" && length != "j" && length != "z" && length != "t") unsupported(source);
            append(text, spec + "ll" + type, static_cast<unsigned long long>(value));
        } else if (type == 's' && length.empty()) append(text, spec + type, reinterpret_cast<const char*>(args.integer()));
        else if (type == 'p' && length.empty()) append(text, spec + type, reinterpret_cast<void*>(args.integer()));
        else if (type == 'c' && length.empty()) append(text, spec + type, static_cast<int>(args.integer()));
        else if (type && std::strchr("fFeEgGaA", type) && (length.empty() || length == "l")) append(text, spec + type, args.floating());
        else unsupported(start);
        if (text.size() > 1024 * 1024) unsupported(source);
    }
    return text;
}
int copyFormat(char* buffer, size_t capacity, const std::string& text) {
    if (capacity) { size_t size = std::min(capacity - 1, text.size()); std::memcpy(buffer, text.data(), size); buffer[size] = 0; }
    return static_cast<int>(text.size());
}
int vsnprintfBridge(char* buffer, size_t capacity, const char* source, const Arguments* arguments) {
    return copyFormat(buffer, capacity, format(source, *arguments));
}
int logWrite(int, const char* tag, const char* message) {
    std::fprintf(stderr, "android[%s]: %s\n", tag ? tag : "", message ? message : ""); return message ? static_cast<int>(std::strlen(message)) : 0;
}
int logVprint(int priority, const char* tag, const char* source, const Arguments* arguments) {
    auto text = format(source, *arguments); return logWrite(priority, tag, text.c_str());
}
} // namespace

extern "C" int guestFormatDispatch(uint64_t* registers, const unsigned char* vectors, const unsigned char* stack, unsigned mode) {
    constexpr unsigned named = 3;
    Arguments args{stack, reinterpret_cast<unsigned char*>(registers + 8), vectors + 128, -int((8 - named) * 8), -128};
    auto text = format(reinterpret_cast<const char*>(registers[2]), args);
    if (mode == 1) return logWrite(static_cast<int>(registers[0]), reinterpret_cast<const char*>(registers[1]), text.c_str());
    return copyFormat(reinterpret_cast<char*>(registers[0]), registers[1], text);
}
extern "C" void guestSnprintf();
extern "C" void guestLogPrint();
Bridge formatBridge(const std::string& name) {
    if (name == "snprintf") return {reinterpret_cast<void*>(&guestSnprintf), false, false};
    if (name == "vsnprintf") return {reinterpret_cast<void*>(&vsnprintfBridge), false, false};
    if (name == "__android_log_print") return {reinterpret_cast<void*>(&guestLogPrint), false, false};
    if (name == "__android_log_write") return {reinterpret_cast<void*>(&logWrite), false, false};
    if (name == "__android_log_vprint") return {reinterpret_cast<void*>(&logVprint), false, false};
    return {};
}
} // namespace native_poc
