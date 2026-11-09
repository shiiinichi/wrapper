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
    size_t reserved_bytes = 0;
    size_t relocations_applied = 0;
    size_t weak_unresolved = 0;
    size_t mixed_write_execute_pages = 0;
    size_t original_mixed_write_execute_pages = 0;
    size_t load_bias_offset = 0;
    size_t constructors_called = 0;
    size_t mixed_pages_read_execute = 0;
    size_t tls_read_rewrites = 0;
    size_t tls_adapter_bytes = 0;
    size_t data_only_relocations_skipped = 0;
    bool data_only = false;
    std::vector<std::string> needed;
    std::set<std::string> guarded_symbols;
    std::set<std::string> missing_symbols;
};

class Image {
public:
    explicit Image(const std::filesystem::path& path, bool reserve_tls_adapters = false);
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    ImageReport report;
    void* exported(const std::string& name) const;
    bool executableAddress(uintptr_t address) const;
    uintptr_t mappingBegin() const { return reinterpret_cast<uintptr_t>(mapping_); }
    uintptr_t mappingEnd() const { return mappingBegin() + mapped_size_; }
    uintptr_t loadBias() const { return bias_; }

private:
    friend class Loader;
    std::vector<uint8_t> file_;
    std::vector<ProgramHeader> segments_;
    std::map<int64_t, std::vector<uint64_t>> dynamic_;
    std::map<std::string, uintptr_t> exports_;
    std::vector<Symbol> symbols_;
    std::vector<int> page_permissions_;
    uint8_t* mapping_ = nullptr;
    void* reservation_ = nullptr;
    size_t reservation_size_ = 0;
    size_t mapped_size_ = 0;
    size_t allocation_size_ = 0;
    size_t thunk_used_ = 0;
    size_t page_size_ = 0;
    uintptr_t bias_ = 0;
    bool has_tls_ = false;

    const uint8_t* at(uint64_t vaddr, size_t count) const;
    std::string stringAt(uint64_t offset) const;
    uint64_t tag(int64_t key, uint64_t fallback = 0) const;
    void readDynamic(const ProgramHeader& segment);
    void readSymbols();
    void protect();
    void adaptThreadPointerReads();
};

class Loader {
public:
    explicit Loader(std::filesystem::path assets, bool apple_profile = false);
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
    bool appleProfile() const { return apple_profile_; }
    std::set<std::string> nativeHostModules() const;

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
    bool apple_profile_ = false;
    std::map<std::string, void*> host_modules_;

    uintptr_t resolve(Image& image, const Symbol& symbol, const std::string& name);
    void relocateTable(Image& image, uint64_t address, uint64_t size);
    void initializeImage(Image& image, std::set<Image*>& visited);
};

std::string jsonString(const std::string& value);
std::string loaderReport(const Loader& loader);

} // namespace native_poc
