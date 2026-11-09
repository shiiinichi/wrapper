#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace native_poc {

struct ProgramHeader {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct Symbol {
    uint32_t name;
    uint8_t info, other;
    uint16_t section;
    uint64_t value, size;
};

struct Issue {
    std::string library;
    std::string kind;
    std::string detail;
};

struct ImageReport {
    std::string library;
    size_t mapped_bytes = 0;
    size_t relocations_applied = 0;
    size_t weak_unresolved = 0;
    size_t mixed_write_execute_pages = 0;
    size_t constructors_called = 0;
    std::vector<std::string> needed;
    std::set<std::string> guarded_symbols;
    std::set<std::string> missing_symbols;
};

class Image {
public:
    explicit Image(const std::filesystem::path& path);
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    ImageReport report;
    void* exported(const std::string& name) const;
    bool executableAddress(uintptr_t address) const;

private:
    friend class Loader;
    std::vector<uint8_t> file_;
    std::vector<ProgramHeader> segments_;
    std::map<int64_t, std::vector<uint64_t>> dynamic_;
    std::map<std::string, uintptr_t> exports_;
    std::vector<Symbol> symbols_;
    std::vector<int> page_permissions_;
    uint8_t* mapping_ = nullptr;
    size_t mapped_size_ = 0;
    size_t page_size_ = 0;
    uint64_t lowest_vaddr_ = 0;
    uintptr_t bias_ = 0;
    bool has_tls_ = false;

    const uint8_t* at(uint64_t vaddr, size_t count) const;
    std::string stringAt(uint64_t offset) const;
    uint64_t tag(int64_t key, uint64_t fallback = 0) const;
    void readDynamic(const ProgramHeader& segment);
    void readSymbols();
    void protect();
};

class Loader {
public:
    explicit Loader(std::filesystem::path assets);
    Image& load(const std::string& name);
    void relocate();
    void prepareExecution();
    void initialize();
    void* symbol(const std::string& library, const std::string& name) const;
    const std::vector<std::unique_ptr<Image>>& images() const { return images_; }
    const std::vector<Issue>& issues() const { return issues_; }
    const std::set<std::string>& systemShims() const { return system_shims_; }
    bool complete() const;
    bool executionPrepared() const { return executable_; }
    bool initialized() const { return initialized_; }

private:
    std::filesystem::path assets_;
    std::vector<std::unique_ptr<Image>> images_;
    std::map<std::string, Image*> by_name_;
    std::set<std::string> system_shims_;
    std::vector<Issue> issues_;
    size_t total_mapped_ = 0;
    bool relocated_ = false;
    bool executable_ = false;
    bool initialized_ = false;

    uintptr_t resolve(Image& image, const Symbol& symbol, const std::string& name);
    void relocateTable(Image& image, uint64_t address, uint64_t size);
    void initializeImage(Image& image, std::set<Image*>& visited);
};

std::string jsonString(const std::string& value);
std::string loaderReport(const Loader& loader);

} // namespace native_poc
