#pragma once
#include <string>
#include <vector>
namespace native_poc {
struct LoginInput {
    std::string user, password;
    LoginInput() = default;
    ~LoginInput();
    LoginInput(const LoginInput&) = delete;
    LoginInput& operator=(const LoginInput&) = delete;
};
bool promptLogin(const std::string& script, LoginInput& input);
bool promptVerification(const std::string& script, std::string& code);
int promptDialog(const std::string& script, const std::string& title, const std::string& message, const std::vector<std::string>& buttons);
std::string profileIdentity(const std::string& directory);
bool validVerification(const std::string& code);
void eraseSecret(std::string& text);
void eraseSecretBytes(void* data, size_t size);
}
