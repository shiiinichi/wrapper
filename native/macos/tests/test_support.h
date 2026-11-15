#pragma once
#include "../kit/runtime.h"
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
extern char **environ;
namespace validation {
using wrapper::Bytes;
using wrapper::Json;
namespace fs = std::filesystem;
inline void require(bool pass, const std::string &description) {
    if (!pass)
        throw std::runtime_error(description);
}
struct Temp {
    fs::path path;
    Temp() {
        std::string p = "/private/tmp/native-wrapper-test-XXXXXX";
        if (!mkdtemp(p.data()))
            throw std::runtime_error("temporary folder");
        path = p;
    }
    ~Temp() {
        std::error_code e;
        fs::remove_all(path, e);
    }
};
inline std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
inline void write(const fs::path &p, const std::string &contents) {
    std::ofstream f(p, std::ios::binary);
    f.write(contents.data(), contents.size());
}
struct Process {
    pid_t pid = -1;
    fs::path out, err;
    Process(const std::vector<std::string> &args, const fs::path &dir) {
        out = dir / "stdout";
        err = dir / "stderr";
        std::vector<char *> av;
        for (auto &s : args)
            av.push_back(const_cast<char *>(s.c_str()));
        av.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, out.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, err.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        int rc = posix_spawn(&pid, args[0].c_str(), &fa, nullptr, av.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc)
            throw std::runtime_error("spawn failed: " + args[0]);
    }
    int wait(int seconds = 20) {
        auto end = wrapper::Clock::now() + std::chrono::seconds(seconds);
        int status = 0;
        while (wrapper::Clock::now() < end) {
            auto r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                pid = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            if (r < 0 && errno != EINTR)
                break;
            usleep(5000);
        }
        throw std::runtime_error("child deadline exceeded");
    }
    ~Process() {
        if (pid > 0) {
            kill(pid, SIGKILL);
            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            };
        }
    }
};
struct Report {
    size_t passed = 0, failed = 0;
    template <class Test> void run(const std::string &name, Test fn) {
        try {
            fn();
            ++passed;
            std::cout << "PASS " << name << "\n";
        } catch (const wrapper::Failure &e) {
            ++failed;
            std::cerr << "FAIL " << name << " (" << e.code << "): " << e.message << "\n";
        } catch (const std::exception &e) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << e.what() << "\n";
        } catch (...) {
            ++failed;
            std::cerr << "FAIL " << name << "\n";
        }
    }
};
} // namespace validation
