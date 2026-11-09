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
#include <atomic>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>

namespace native_poc {
namespace {
std::atomic<bool> logging_enabled{true};
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
    if (!logging_enabled) return 0;
    std::fprintf(stderr, "android[%s]: %s\n", tag ? tag : "", message ? message : ""); return message ? static_cast<int>(std::strlen(message)) : 0;
}
int logVprint(int priority, const char* tag, const char* source, const Arguments* arguments) {
    if (!logging_enabled) return 0;
    auto text = format(source, *arguments); return logWrite(priority, tag, text.c_str());
}
std::vector<uint64_t> cfArguments(CFStringRef source, Arguments args) {
    CFIndex length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(source), kCFStringEncodingUTF8);
    if (length < 0 || length > 1024 * 1024) unsupported("CoreFoundation format length");
    std::vector<char> utf8(static_cast<size_t>(length) + 1);
    if (!CFStringGetCString(source, utf8.data(), utf8.size(), kCFStringEncodingUTF8)) unsupported("CoreFoundation format encoding");
    // Derive argument classes by position, then consume AAPCS64 GP/FP areas in
    // call order. Darwin va_list is a pointer to consecutive 8-byte stack slots.
    std::vector<char> kinds;
    size_t next = 0;
    auto assign = [&](size_t index, char kind) {
        if (index >= 64) unsupported("CoreFoundation argument count");
        if (kinds.size() <= index) kinds.resize(index + 1, 0);
        if (kinds[index] && kinds[index] != kind) unsupported("CoreFoundation positional argument type");
        kinds[index] = kind;
    };
    auto position = [&](const char*& p) -> size_t {
        const char* saved = p; size_t value = 0;
        while (std::isdigit(static_cast<unsigned char>(*p))) { value = value * 10 + (*p++ - '0'); if (value > 1000000) unsupported("CoreFoundation format index"); }
        if (*p == '$') { ++p; if (!value || value > 64) unsupported("CoreFoundation format index"); return value - 1; }
        p = saved; return SIZE_MAX;
    };
    for (const char* p = utf8.data(); *p;) {
        if (*p++ != '%') continue;
        if (*p == '%') { ++p; continue; }
        size_t index = position(p);
        while (*p && std::strchr("-+ #0'", *p)) ++p;
        if (*p == '*') { ++p; auto width = position(p); assign(width == SIZE_MAX ? next++ : width, 'g'); }
        else while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
        if (*p == '.') {
            ++p;
            if (*p == '*') { ++p; auto precision = position(p); assign(precision == SIZE_MAX ? next++ : precision, 'g'); }
            else while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
        }
        if (*p == 'L') unsupported("CoreFoundation binary128 argument");
        if (*p && std::strchr("hljztq", *p)) { char first = *p++; if ((*p == 'h' || *p == 'l') && *p == first) ++p; }
        char kind = *p && std::strchr("fFeEgGaA", *p) ? 'f' : 'g';
        if (!*p || !std::strchr("@diDuUoOxXcCsSpfFeEgGaA", *p)) unsupported("CoreFoundation format specifier");
        assign(index == SIZE_MAX ? next++ : index, kind); ++p;
    }
    std::vector<uint64_t> slots;
    for (auto kind : kinds) {
        if (!kind) unsupported("CoreFoundation positional argument gap");
        if (kind == 'f') { double value = args.floating(); uint64_t bits; std::memcpy(&bits, &value, 8); slots.push_back(bits); }
        else slots.push_back(args.integer());
    }
    return slots;
}
CFStringRef cfCreate(CFAllocatorRef allocator, CFDictionaryRef options, CFStringRef source, const Arguments* args) {
    auto slots = cfArguments(source, *args);
    return CFStringCreateWithFormatAndArguments(allocator, options, source, reinterpret_cast<va_list>(slots.data()));
}
void cfAppend(CFMutableStringRef target, CFDictionaryRef options, CFStringRef source, const Arguments* args) {
    auto slots = cfArguments(source, *args);
    CFStringAppendFormatAndArguments(target, options, source, reinterpret_cast<va_list>(slots.data()));
}
} // namespace

extern "C" uintptr_t guestFormatDispatch(uint64_t* registers, const unsigned char* vectors, const unsigned char* stack, unsigned mode) {
    if ((mode == 1 || mode == 4) && !logging_enabled) return 0;
    unsigned named = mode == 4 ? 2 : 3;
    Arguments args{stack, reinterpret_cast<unsigned char*>(registers + 8), vectors + 128, -int((8 - named) * 8), -128};
    if (mode == 2) return reinterpret_cast<uintptr_t>(cfCreate(reinterpret_cast<CFAllocatorRef>(registers[0]), reinterpret_cast<CFDictionaryRef>(registers[1]), reinterpret_cast<CFStringRef>(registers[2]), &args));
    if (mode == 3) { cfAppend(reinterpret_cast<CFMutableStringRef>(registers[0]), reinterpret_cast<CFDictionaryRef>(registers[1]), reinterpret_cast<CFStringRef>(registers[2]), &args); return 0; }
    if (mode == 4) {
        auto source = reinterpret_cast<CFStringRef>(registers[1]);
        auto message = cfCreate(nullptr, nullptr, source, &args);
        auto log = reinterpret_cast<void (*)(int64_t, CFStringRef, ...)>(dlsym(RTLD_DEFAULT, "CFLog"));
        if (!log) unsupported("native CFLog unavailable");
        log(static_cast<int64_t>(registers[0]), CFSTR("%@"), message);
        if (message) CFRelease(message); return 0;
    }
    auto text = format(reinterpret_cast<const char*>(registers[2]), args);
    if (mode == 1) return logWrite(static_cast<int>(registers[0]), reinterpret_cast<const char*>(registers[1]), text.c_str());
    return copyFormat(reinterpret_cast<char*>(registers[0]), registers[1], text);
}
void setGuestLogging(bool enabled) { logging_enabled = enabled; }
extern "C" void guestSnprintf();
extern "C" void guestLogPrint();
extern "C" void guestCFCreateFormat();
extern "C" void guestCFAppendFormat();
extern "C" void guestCFLog();
Bridge formatBridge(const std::string& name) {
    if (name == "snprintf") return {reinterpret_cast<void*>(&guestSnprintf), false, false};
    if (name == "vsnprintf") return {reinterpret_cast<void*>(&vsnprintfBridge), false, false};
    if (name == "__android_log_print") return {reinterpret_cast<void*>(&guestLogPrint), false, false};
    if (name == "__android_log_write") return {reinterpret_cast<void*>(&logWrite), false, false};
    if (name == "__android_log_vprint") return {reinterpret_cast<void*>(&logVprint), false, false};
    if (name == "CFStringCreateWithFormat") return {reinterpret_cast<void*>(&guestCFCreateFormat), false, false};
    if (name == "CFStringAppendFormat") return {reinterpret_cast<void*>(&guestCFAppendFormat), false, false};
    if (name == "CFStringCreateWithFormatAndArguments") return {reinterpret_cast<void*>(&cfCreate), false, false};
    if (name == "CFStringAppendFormatAndArguments") return {reinterpret_cast<void*>(&cfAppend), false, false};
    if (name == "CFLog") return {reinterpret_cast<void*>(&guestCFLog), false, false};
    return {};
}
} // namespace native_poc
