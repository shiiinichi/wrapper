#include "../third_party/json/json.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <unistd.h>
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
const std::set<std::string> expected = {"libBlocksRuntime.so", "libCoreADI.so",           "libCoreFP.so",
                                        "libCoreLSKD.so",      "libOpenSLES.so",          "libandroidappmusic.so",
                                        "libc++_shared.so",    "libdaapkit.so",           "libmedialibrarycore.so",
                                        "libmediaplatform.so", "libstoreservicescore.so", "libwilhelm.so"};
std::string read(const fs::path &path) {
    if (!fs::is_regular_file(fs::symlink_status(path)) || fs::file_size(path) > 64 * 1024 * 1024)
        throw std::runtime_error("Invalid asset: " + path.filename().string());
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Could not read asset");
    return {std::istreambuf_iterator<char>(f), {}};
}
std::string hash(const std::string &bytes) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest);
    std::string out;
    const char *alphabet = "0123456789abcdef";
    for (auto c : digest) {
        out += alphabet[c >> 4];
        out += alphabet[c & 15];
    }
    return out;
}
void publish(const fs::path &target, const std::string &bytes) {
    auto t = target.string() + ".tmp." + std::to_string(getpid());
    std::ofstream f(t, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), bytes.size());
    f.close();
    if (f.fail()) {
        std::error_code e;
        fs::remove(t, e);
        throw std::runtime_error("Could not write staged asset");
    }
    fs::rename(t, target);
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 3 || argc > 4)
            throw std::runtime_error("Usage: native-asset-tool SOURCE DESTINATION [--list]");
        fs::path src = argv[1], dest = argv[2];
        auto text = read(src / "manifest.json");
        auto manifest = Json::parse(text);
        std::set<std::string> found;
        std::vector<std::pair<std::string, std::string>> files;
        for (const auto &row : manifest.at("files")) {
            auto name = row.at("library").get<std::string>();
            if (!expected.count(name) || !found.insert(name).second)
                throw std::runtime_error("Unexpected or duplicate SDK library");
            auto data = read(src / name);
            if (data.size() < 64 ||
                data.compare(0, 6,
                             "\x7f"
                             "ELF\x02\x01",
                             6) != 0 ||
                static_cast<uint8_t>(data[18]) != 183 || data[19] != 0)
                throw std::runtime_error("Invalid ARM64 ELF: " + name);
            if (data.size() != row.at("bytes").get<size_t>() || hash(data) != row.at("sha256").get<std::string>())
                throw std::runtime_error("Asset checksum mismatch: " + name);
            files.emplace_back(name, std::move(data));
        }
        if (found != expected)
            throw std::runtime_error("Missing native SDK libraries");
        fs::create_directories(dest);
        Json previous;
        try {
            previous = Json::parse(read(dest / "manifest.json"));
        } catch (const std::exception &) {
        }
        for (const auto &item : files) {
            auto target = dest / item.first;
            if (!fs::is_regular_file(fs::symlink_status(target)) || hash(read(target)) != hash(item.second))
                publish(target, item.second);
        }
        if (previous.contains("files") && previous["files"].is_array())
            for (const auto &row : previous["files"]) {
                if (!row.is_object())
                    continue;
                auto name = row.value("library", std::string{});
                if (name.empty() || found.count(name) || fs::path(name).filename() != name ||
                    fs::path(name).extension() != ".so")
                    continue;
                auto path = dest / name;
                if (fs::is_regular_file(fs::symlink_status(path)) &&
                    hash(read(path)) == row.value("sha256", std::string{}))
                    fs::remove(path);
            }
        publish(dest / "manifest.json", text);
        if (argc == 4 && std::string(argv[3]) == "--list") {
            for (const auto &item : manifest["files"])
                std::cout << item["library"].get<std::string>() << "\n";
        } else
            std::cout << "Verified " << files.size() << " ARM64 SDK libraries.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
