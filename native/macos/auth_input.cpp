#include "auth_input.h"
#include "elf_loader.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <CoreFoundation/CoreFoundation.h>
extern char** environ;
namespace native_poc {
void eraseSecretBytes(void* data, size_t size) { volatile unsigned char* bytes = static_cast<volatile unsigned char*>(data); while (size--) *bytes++ = 0; }
void eraseSecret(std::string& text) { eraseSecretBytes(text.data(), text.size()); text.clear(); }
LoginInput::~LoginInput() { eraseSecret(user); eraseSecret(password); }
namespace {
bool prompt(const std::string& script, const char* kind, std::string& answer, const std::string& payload = "") {
    if (script == "@private-pipe") {
        // Embedded clients render their own sign-in UI. Credentials and codes
        // travel only through inherited pipes, never argv, files, or logs.
        if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO)) throw std::runtime_error("sign-in requires private pipes");
        auto request = "{\"prompt\":" + jsonString(kind) + ",\"plist\":" + jsonString(payload) + "}";
        if (request.size() > 65536) throw std::runtime_error("sign-in prompt is too large");
        auto transfer = [](int fd, void* pointer, size_t size, bool writing) {
            auto* bytes = static_cast<unsigned char*>(pointer);
            while (size) {
                ssize_t count = writing ? write(fd, bytes, size) : read(fd, bytes, size);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) return false;
                bytes += count; size -= static_cast<size_t>(count);
            }
            return true;
        };
        std::array<unsigned char, 4> prefix{};
        auto size = static_cast<uint32_t>(request.size());
        for (unsigned n = 0; n < 4; ++n) prefix[n] = static_cast<unsigned char>(size >> (24 - n * 8));
        if (!transfer(STDOUT_FILENO, prefix.data(), prefix.size(), true) ||
            !transfer(STDOUT_FILENO, request.data(), request.size(), true) ||
            !transfer(STDIN_FILENO, prefix.data(), prefix.size(), false)) return false;
        size = 0; for (auto byte : prefix) size = (size << 8) | byte;
        if (!size) return false;
        if (size > 8192) throw std::runtime_error("native sign-in input is too long");
        answer.resize(size);
        if (!transfer(STDIN_FILENO, answer.data(), answer.size(), false)) { eraseSecret(answer); return false; }
        return true;
    }
    answer.reserve(8192);
    int pipefd[2]; if (pipe(pipefd)) throw std::runtime_error("cannot open native prompt pipe");
    int inputfd[2]; if (pipe(inputfd)) { close(pipefd[0]); close(pipefd[1]); throw std::runtime_error("cannot open native prompt input"); }
    fcntl(inputfd[1], F_SETNOSIGPIPE, 1);
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC); fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    fcntl(inputfd[0], F_SETFD, FD_CLOEXEC); fcntl(inputfd[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, inputfd[0], STDIN_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    posix_spawn_file_actions_addclose(&actions, inputfd[0]);
    posix_spawn_file_actions_addclose(&actions, inputfd[1]);
    char executable[] = "/usr/bin/osascript";
    char* arguments[] = {executable, const_cast<char*>(script.c_str()), const_cast<char*>(kind), nullptr};
    pid_t child = 0; int error = posix_spawn(&child, executable, &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions); close(pipefd[1]);
    close(inputfd[0]);
    if (error) { close(pipefd[0]); close(inputfd[1]); throw std::runtime_error("cannot launch native sign-in prompt"); }
    size_t sent = 0;
    while (sent < payload.size()) { ssize_t n = write(inputfd[1], payload.data() + sent, payload.size() - sent); if (n < 0 && errno == EINTR) continue; if (n <= 0) break; sent += n; }
    close(inputfd[1]);
    std::array<char, 1024> buffer{};
    for (;;) {
        ssize_t count = read(pipefd[0], buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        if (answer.size() + count > 8192) { kill(child, SIGTERM); close(pipefd[0]); waitpid(child, nullptr, 0); eraseSecret(answer); throw std::runtime_error("native sign-in input is too long"); }
        answer.append(buffer.data(), count); eraseSecretBytes(buffer.data(), buffer.size());
    }
    close(pipefd[0]); int status = 0; while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { eraseSecret(answer); return false; }
    if (!answer.empty() && answer.back() == '\n') answer.pop_back();
    return true;
}
bool hexIdentity(const std::string& text) { return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
}
bool promptLogin(const std::string& script, LoginInput& input) {
    std::string reply;
    if (!prompt(script, "credentials", reply)) return false;
    auto split = reply.find('\n');
    if (split == std::string::npos || split == 0 || split + 1 == reply.size() || reply.find('\n', split + 1) != std::string::npos || reply.find('\r') != std::string::npos || reply.find('\0') != std::string::npos) {
        eraseSecret(reply); throw std::runtime_error("native sign-in requires a single-line account and password");
    }
    eraseSecret(input.user); eraseSecret(input.password);
    input.user.assign(reply, 0, split); input.password.assign(reply, split + 1, std::string::npos);
    eraseSecret(reply); return true;
}
bool validVerification(const std::string& code) { return code.size() == 6 && std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; }); }
bool promptVerification(const std::string& script, std::string& code) {
    if (!prompt(script, "code", code)) return false;
    if (!validVerification(code)) { eraseSecret(code); throw std::runtime_error("verification code must contain six digits"); }
    return true;
}
std::string probeAuthInputPipe() {
    LoginInput input;
    if (!promptLogin("@private-pipe", input)) return "{\"cancelled\":true,\"credentials_submitted\":false}";
    std::string code;
    if (!promptVerification("@private-pipe", code)) return "{\"cancelled\":true,\"credentials_submitted\":false}";
    eraseSecret(code);
    int choice = promptDialog("@private-pipe", "Private pipe fixture", "No credentials are submitted by this probe.", {"Continue", "Cancel"});
    return "{\"credentials_valid\":true,\"verification_valid\":true,\"dialog_choice\":" + std::to_string(choice) + ",\"credentials_submitted\":false}";
}
int promptDialog(const std::string& script, const std::string& title, const std::string& message, const std::vector<std::string>& buttons) {
    if (buttons.empty() || buttons.size() > 32) return -1;
    auto string = [](const std::string& text) { return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(text.data()), text.size(), kCFStringEncodingUTF8, false); };
    auto title_string = string(title), message_string = string(message);
    if (!title_string || !message_string) { if (title_string) CFRelease(title_string); if (message_string) CFRelease(message_string); throw std::runtime_error("invalid dialog text encoding"); }
    auto choices = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    for (size_t i = 0; i < buttons.size(); ++i) { auto choice = string(std::to_string(i + 1) + ". " + buttons[i]); CFArrayAppendValue(choices, choice); CFRelease(choice); }
    const void* keys[] = {CFSTR("title"), CFSTR("message"), CFSTR("buttons")};
    const void* values[] = {title_string, message_string, choices};
    auto dictionary = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto data = CFPropertyListCreateData(nullptr, dictionary, kCFPropertyListXMLFormat_v1_0, 0, nullptr);
    CFRelease(title_string); CFRelease(message_string); CFRelease(choices); CFRelease(dictionary);
    if (!data) throw std::runtime_error("cannot prepare native protocol dialog");
    std::string payload(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), CFDataGetLength(data)); CFRelease(data);
    std::string answer;
    if (!prompt(script, "dialog", answer, payload)) return -1;
    char* end = nullptr; long value = std::strtol(answer.c_str(), &end, 10);
    return end && !*end && value > 0 && value <= static_cast<long>(buttons.size()) ? static_cast<int>(value - 1) : -1;
}
std::string profileIdentity(const std::string& directory) {
    if (directory.empty()) throw std::runtime_error("authentication requires --data-dir");
    std::filesystem::create_directories(directory);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    auto path = std::filesystem::path(directory) / "native-device-id";
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        std::array<char, 18> buffer{}; ssize_t count = read(fd, buffer.data(), buffer.size()); close(fd);
        std::string identity(buffer.data(), count > 0 ? static_cast<size_t>(count) : 0);
        if (!hexIdentity(identity)) throw std::runtime_error("native profile identity is invalid");
        return identity;
    }
    if (errno != ENOENT) throw std::runtime_error("native profile identity cannot be read");
    std::array<unsigned char, 8> random{}; arc4random_buf(random.data(), random.size());
    std::string identity; for (auto byte : random) { identity += "0123456789abcdef"[byte >> 4]; identity += "0123456789abcdef"[byte & 15]; }
    fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("native profile identity cannot be created");
    ssize_t count = write(fd, identity.data(), identity.size()); int closed = close(fd);
    if (count != static_cast<ssize_t>(identity.size()) || closed) throw std::runtime_error("native profile identity could not be saved");
    return identity;
}
}
