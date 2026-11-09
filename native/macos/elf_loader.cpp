#include "elf_loader.h"
#include "shims.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace native_poc {
namespace {

struct Header {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Dynamic { int64_t tag; uint64_t value; };
struct Rela { uint64_t offset, info; int64_t addend; };
static_assert(sizeof(Header) == 64 && sizeof(ProgramHeader) == 56);
static_assert(sizeof(Symbol) == 24 && sizeof(Rela) == 24 && sizeof(Dynamic) == 16);

constexpr uint32_t kLoad = 1, kDynamic = 2, kTls = 7;
constexpr int64_t kNeeded = 1, kPltRelSize = 2, kHash = 4, kStringTable = 5;
constexpr int64_t kSymbolTable = 6, kRela = 7, kRelaSize = 8, kRelaEnt = 9;
constexpr int64_t kStringSize = 10, kSymbolEnt = 11, kInit = 12;
constexpr int64_t kRel = 17, kRelSize = 18, kPltRel = 20, kJumpRel = 23;
constexpr int64_t kInitArray = 25, kInitArraySize = 27, kGnuHash = 0x6ffffef5;
constexpr uint64_t kMaximumImage = 256ULL * 1024 * 1024;
constexpr uint64_t kMaximumTotal = 512ULL * 1024 * 1024;
constexpr size_t kMaximumSymbols = 1000000;

uint64_t add(uint64_t a, uint64_t b) {
    if (b > std::numeric_limits<uint64_t>::max() - a)
        throw std::runtime_error("ELF integer overflow");
    return a + b;
}

uint64_t alignUp(uint64_t value, size_t alignment) {
    return add(value, alignment - 1) & ~(uint64_t(alignment) - 1);
}

template <typename T> T read(const uint8_t* source) {
    T value;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

bool safeName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
        name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
        name.find('\0') == std::string::npos;
}

const std::set<std::string> kShimLibraries = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libstdc++.so"
};

template <typename Range> void stringArray(std::ostream& out, const Range& values) {
    out << '[';
    bool first = true;
    for (const auto& value : values) {
        if (!first) out << ',';
        out << jsonString(value);
        first = false;
    }
    out << ']';
}

} // namespace

Image::Image(const std::filesystem::path& path) {
    report.library = path.filename().string();
    try {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) throw std::runtime_error("cannot read ELF: " + path.string());
        auto length = stream.tellg();
        if (length < static_cast<std::streamoff>(sizeof(Header)) ||
            length > static_cast<std::streamoff>(kMaximumImage))
            throw std::runtime_error("ELF file size is invalid");
        file_.resize(static_cast<size_t>(length));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(file_.data()), length))
            throw std::runtime_error("truncated ELF file");
        Header header = read<Header>(file_.data());
        if (std::memcmp(header.ident, "\177ELF", 4) || header.ident[4] != 2 ||
            header.ident[5] != 1 || header.ident[6] != 1 || header.type != 3 ||
            header.machine != 183 || header.version != 1)
            throw std::runtime_error("expected a little-endian AArch64 ELF64 shared library");
        if (header.ehsize != sizeof(Header) || header.phentsize != sizeof(ProgramHeader) ||
            header.phnum == 0 || header.phnum > 1024 ||
            add(header.phoff, uint64_t(header.phnum) * header.phentsize) > file_.size())
            throw std::runtime_error("invalid ELF program-header table");
        page_size_ = static_cast<size_t>(getpagesize());
        if (!page_size_ || (page_size_ & (page_size_ - 1)))
            throw std::runtime_error("invalid host page size");
        uint64_t low = std::numeric_limits<uint64_t>::max(), high = 0;
        for (size_t i = 0; i < header.phnum; ++i) {
            auto ph = read<ProgramHeader>(file_.data() + header.phoff + i * header.phentsize);
            if (ph.type == kLoad) {
                if (ph.filesz > ph.memsz || add(ph.offset, ph.filesz) > file_.size())
                    throw std::runtime_error("invalid PT_LOAD file range");
                if (ph.align > 1 && ((ph.align & (ph.align - 1)) ||
                    ph.offset % ph.align != ph.vaddr % ph.align))
                    throw std::runtime_error("invalid PT_LOAD alignment");
                if ((ph.flags & ~uint32_t(7)) || ((ph.flags & 3) == 3))
                    throw std::runtime_error("ELF requests an unsupported writable executable segment");
                if (ph.memsz) {
                    low = std::min(low, ph.vaddr & ~(uint64_t(page_size_) - 1));
                    high = std::max(high, alignUp(add(ph.vaddr, ph.memsz), page_size_));
                }
            }
            if (ph.type == kTls && ph.memsz) has_tls_ = true;
            segments_.push_back(ph);
        }
        if (low >= high || high - low > kMaximumImage)
            throw std::runtime_error("ELF load span exceeds the 256 MiB limit");
        // Real segment ranges may not overlap; host-page overlap is handled below.
        for (size_t i = 0; i < segments_.size(); ++i) {
            const auto& a = segments_[i];
            if (a.type != kLoad || !a.memsz) continue;
            for (size_t j = 0; j < i; ++j) {
                const auto& b = segments_[j];
                if (b.type == kLoad && b.memsz && a.vaddr < add(b.vaddr, b.memsz) &&
                    b.vaddr < add(a.vaddr, a.memsz))
                    throw std::runtime_error("overlapping ELF load segments");
            }
        }
        lowest_vaddr_ = low;
        mapped_size_ = static_cast<size_t>(high - low);
        void* memory = mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
        if (memory == MAP_FAILED) throw std::runtime_error("mmap failed: " + std::string(std::strerror(errno)));
        mapping_ = static_cast<uint8_t*>(memory);
        bias_ = reinterpret_cast<uintptr_t>(mapping_) - low;
        report.mapped_bytes = mapped_size_;
        page_permissions_.resize(mapped_size_ / page_size_, PROT_NONE);
        for (const auto& ph : segments_) {
            if (ph.type != kLoad || !ph.memsz) continue;
            if (ph.filesz) std::memcpy(mapping_ + ph.vaddr - low, file_.data() + ph.offset, ph.filesz);
            int permissions = ((ph.flags & 4) ? PROT_READ : 0) |
                ((ph.flags & 2) ? PROT_WRITE : 0) | ((ph.flags & 1) ? PROT_EXEC : 0);
            size_t first = static_cast<size_t>((ph.vaddr - low) / page_size_);
            size_t end = static_cast<size_t>((alignUp(add(ph.vaddr, ph.memsz), page_size_) - low) / page_size_);
            for (size_t page = first; page < end; ++page) page_permissions_[page] |= permissions;
        }
        for (int permissions : page_permissions_)
            if ((permissions & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
                ++report.mixed_write_execute_pages;
        size_t dynamic_count = 0;
        for (const auto& ph : segments_) {
            if (ph.type == kDynamic) { readDynamic(ph); ++dynamic_count; }
        }
        if (dynamic_count != 1) throw std::runtime_error("expected exactly one PT_DYNAMIC segment");
        readSymbols();
        for (uint64_t offset : dynamic_[kNeeded]) {
            auto name = stringAt(offset);
            if (!safeName(name)) throw std::runtime_error("unsafe DT_NEEDED library name");
            report.needed.push_back(name);
        }
    } catch (...) {
        if (mapping_) munmap(mapping_, mapped_size_);
        mapping_ = nullptr;
        throw;
    }
}

Image::~Image() {
    if (mapping_) munmap(mapping_, mapped_size_);
}

const uint8_t* Image::at(uint64_t vaddr, size_t count) const {
    uint64_t end = add(vaddr, count);
    for (const auto& ph : segments_) {
        if (ph.type == kLoad && vaddr >= ph.vaddr && end <= add(ph.vaddr, ph.memsz))
            return mapping_ + vaddr - lowest_vaddr_;
    }
    throw std::runtime_error(report.library + ": dynamic address outside PT_LOAD: " + hex(vaddr));
}

uint64_t Image::tag(int64_t key, uint64_t fallback) const {
    auto found = dynamic_.find(key);
    return found == dynamic_.end() || found->second.empty() ? fallback : found->second.front();
}

void Image::readDynamic(const ProgramHeader& segment) {
    if (!segment.memsz || segment.memsz % sizeof(Dynamic) || segment.memsz > kMaximumImage)
        throw std::runtime_error("invalid PT_DYNAMIC size");
    at(segment.vaddr, segment.memsz);
    for (uint64_t offset = 0; offset < segment.memsz; offset += sizeof(Dynamic)) {
        auto entry = read<Dynamic>(at(add(segment.vaddr, offset), sizeof(Dynamic)));
        if (entry.tag == 0) return;
        if (entry.tag != kNeeded && dynamic_.count(entry.tag))
            throw std::runtime_error("duplicate singleton dynamic tag");
        dynamic_[entry.tag].push_back(entry.value);
    }
    throw std::runtime_error("unterminated PT_DYNAMIC table");
}

std::string Image::stringAt(uint64_t offset) const {
    uint64_t size = tag(kStringSize);
    if (offset >= size) throw std::runtime_error("ELF string offset out of bounds");
    auto* start = at(add(tag(kStringTable), offset), static_cast<size_t>(size - offset));
    auto* end = static_cast<const uint8_t*>(std::memchr(start, 0, static_cast<size_t>(size - offset)));
    if (!end) throw std::runtime_error("unterminated ELF string");
    return std::string(reinterpret_cast<const char*>(start), static_cast<size_t>(end - start));
}

void Image::readSymbols() {
    if (!tag(kStringTable) || !tag(kStringSize) || !tag(kSymbolTable) || tag(kSymbolEnt) != sizeof(Symbol))
        throw std::runtime_error("missing or invalid ELF dynamic symbol table");
    at(tag(kStringTable), tag(kStringSize));
    size_t count = 0;
    if (tag(kHash)) {
        auto* hash = at(tag(kHash), 8);
        auto buckets = read<uint32_t>(hash);
        count = read<uint32_t>(hash + 4);
        if (buckets > kMaximumSymbols || count > kMaximumSymbols)
            throw std::runtime_error("oversized SysV hash table");
        at(tag(kHash), add(8, uint64_t(buckets + count) * 4));
    } else if (tag(kGnuHash)) {
        auto* hash = at(tag(kGnuHash), 16);
        uint32_t buckets = read<uint32_t>(hash), first = read<uint32_t>(hash + 4);
        uint32_t bloom = read<uint32_t>(hash + 8);
        if (!bloom || buckets > kMaximumSymbols || bloom > kMaximumSymbols || first > kMaximumSymbols)
            throw std::runtime_error("invalid GNU hash table");
        uint64_t bucket_address = add(tag(kGnuHash), add(16, uint64_t(bloom) * 8));
        at(bucket_address, uint64_t(buckets) * 4);
        count = first;
        uint64_t chain = add(bucket_address, uint64_t(buckets) * 4);
        for (uint32_t i = 0; i < buckets; ++i) {
            uint32_t index = read<uint32_t>(at(add(bucket_address, uint64_t(i) * 4), 4));
            if (!index) continue;
            if (index < first) throw std::runtime_error("GNU hash bucket precedes symbol offset");
            for (;;) {
                if (index >= kMaximumSymbols) throw std::runtime_error("GNU hash chain exceeds limit");
                uint32_t value = read<uint32_t>(at(add(chain, uint64_t(index - first) * 4), 4));
                count = std::max(count, size_t(index) + 1);
                if (value & 1) break;
                ++index;
            }
        }
    } else throw std::runtime_error("ELF has neither SysV nor GNU symbol hash");
    if (!count || count > kMaximumSymbols) throw std::runtime_error("invalid ELF symbol count");
    at(tag(kSymbolTable), count * sizeof(Symbol));
    for (size_t i = 0; i < count; ++i) {
        auto symbol = read<Symbol>(at(add(tag(kSymbolTable), i * sizeof(Symbol)), sizeof(Symbol)));
        symbols_.push_back(symbol);
        auto name = stringAt(symbol.name);
        unsigned binding = symbol.info >> 4, visibility = symbol.other & 3, type = symbol.info & 15;
        if (symbol.section && !name.empty() && (binding == 1 || binding == 2) &&
            (visibility == 0 || visibility == 3) && type != 6 && type != 10) {
            uintptr_t address = symbol.section == 0xfff1 ? symbol.value : bias_ + symbol.value;
            // Linker end markers can point one byte past PT_LOAD. They are not
            // executable entry points and are never used without a relocation.
            exports_.emplace(name, address);
        }
    }
}

void* Image::exported(const std::string& name) const {
    auto found = exports_.find(name);
    return found == exports_.end() ? nullptr : reinterpret_cast<void*>(found->second);
}

bool Image::executableAddress(uintptr_t address) const {
    if (address < bias_) return false;
    uint64_t vaddr = address - bias_;
    for (const auto& ph : segments_)
        if (ph.type == kLoad && (ph.flags & 1) && vaddr >= ph.vaddr && vaddr < add(ph.vaddr, ph.memsz))
            return true;
    return false;
}

void Image::protect() {
    if (report.mixed_write_execute_pages)
        throw std::runtime_error(report.library + ": 4 KiB ELF segments share a writable/executable host page");
    for (size_t first = 0; first < page_permissions_.size();) {
        size_t end = first + 1;
        while (end < page_permissions_.size() && page_permissions_[end] == page_permissions_[first]) ++end;
        if (mprotect(mapping_ + first * page_size_, (end - first) * page_size_, page_permissions_[first]))
            throw std::runtime_error("mprotect failed: " + std::string(std::strerror(errno)));
        first = end;
    }
    // A load image can contain PROT_NONE gaps. Invalidating the entire reserved
    // span faults on Apple Silicon; flush only the actual executable segments.
    for (const auto& ph : segments_) {
        if (ph.type == kLoad && (ph.flags & 1) && ph.memsz) {
            auto* start = reinterpret_cast<char*>(mapping_ + ph.vaddr - lowest_vaddr_);
            __builtin___clear_cache(start, start + ph.memsz);
        }
    }
}

Loader::Loader(std::filesystem::path assets) : assets_(std::move(assets)) {}

Image& Loader::load(const std::string& name) {
    if (relocated_) throw std::runtime_error("loading after relocation is unsupported");
    if (!safeName(name)) throw std::runtime_error("expected a library basename inside the assets directory");
    auto existing = by_name_.find(name);
    if (existing != by_name_.end()) return *existing->second;
    if (kShimLibraries.count(name)) throw std::runtime_error("cannot use a system shim as the root ELF");
    auto image = std::make_unique<Image>(assets_ / name);
    auto* result = image.get();
    if (add(total_mapped_, image->report.mapped_bytes) > kMaximumTotal)
        throw std::runtime_error("dependency graph exceeds the 512 MiB mapping limit");
    total_mapped_ += image->report.mapped_bytes;
    by_name_[name] = result;
    images_.push_back(std::move(image));
    if (result->has_tls_) issues_.push_back({name, "tls", "PT_TLS requires an Android TLS implementation"});
    if (result->report.mixed_write_execute_pages)
        issues_.push_back({name, "page_permissions", "code and writable data share " +
            std::to_string(result->report.mixed_write_execute_pages) + " host page(s)"});
    for (const auto& dependency : result->report.needed) {
        if (kShimLibraries.count(dependency)) { system_shims_.insert(dependency); continue; }
        try { load(dependency); }
        catch (const std::exception& error) { issues_.push_back({name, "dependency", dependency + ": " + error.what()}); }
    }
    return *result;
}

uintptr_t Loader::resolve(Image& image, const Symbol& symbol, const std::string& name) {
    unsigned type = symbol.info & 15;
    if (type == 6 || type == 10) {
        image.report.missing_symbols.insert(name);
        issues_.push_back({image.report.library, "symbol_type", name + ": TLS/IFUNC is unsupported"});
        return 0;
    }
    if (symbol.section) return symbol.section == 0xfff1 ? symbol.value : image.bias_ + symbol.value;
    for (const auto& candidate : images_)
        if (void* address = candidate->exported(name)) return reinterpret_cast<uintptr_t>(address);
    auto bridge = bridgeSymbol(name);
    if (bridge.address && (type != 1 || bridge.data)) {
        if (bridge.guarded) image.report.guarded_symbols.insert(name);
        return reinterpret_cast<uintptr_t>(bridge.address);
    }
    if ((symbol.info >> 4) == 2) { ++image.report.weak_unresolved; return 0; }
    image.report.missing_symbols.insert(name);
    return 0;
}

void Loader::relocateTable(Image& image, uint64_t address, uint64_t size) {
    if (!size) return;
    if (!address || size % sizeof(Rela)) throw std::runtime_error("invalid RELA table size");
    image.at(address, size);
    for (uint64_t offset = 0; offset < size; offset += sizeof(Rela)) {
        auto relocation = read<Rela>(image.at(add(address, offset), sizeof(Rela)));
        uint32_t type = uint32_t(relocation.info), index = uint32_t(relocation.info >> 32);
        if (type == 0) continue;
        if (type != 257 && type != 1025 && type != 1026 && type != 1027) {
            issues_.push_back({image.report.library, "relocation", "unsupported AArch64 relocation " + std::to_string(type)});
            continue;
        }
        auto* target = const_cast<uint8_t*>(image.at(relocation.offset, sizeof(uint64_t)));
        // Do not rewrite code, headers, or read-only data via a crafted relocation.
        bool writable = false;
        for (const auto& ph : image.segments_)
            if (ph.type == kLoad && (ph.flags & 2) && relocation.offset >= ph.vaddr &&
                add(relocation.offset, sizeof(uint64_t)) <= add(ph.vaddr, ph.memsz)) writable = true;
        if (!writable) throw std::runtime_error("relocation target is not in a writable PT_LOAD segment");
        uint64_t value = 0;
        if (type == 1027) {
            if (index) throw std::runtime_error("RELATIVE relocation has a symbol index");
            value = image.bias_ + uint64_t(relocation.addend);
        } else {
            if (index >= image.symbols_.size()) throw std::runtime_error("relocation symbol index out of bounds");
            const auto& symbol = image.symbols_[index];
            auto name = image.stringAt(symbol.name);
            uintptr_t resolved = resolve(image, symbol, name);
            if (!resolved && (symbol.info >> 4) != 2 && symbol.section == 0) continue;
            value = resolved + uint64_t(relocation.addend);
        }
        std::memcpy(target, &value, sizeof(value));
        ++image.report.relocations_applied;
    }
}

void Loader::relocate() {
    if (relocated_) throw std::runtime_error("ELF images have already been relocated");
    for (auto& image : images_) {
        try {
            if (image->tag(kRelSize) || image->tag(kRel) || image->tag(0x60000010) ||
                image->tag(0x60000012) || image->tag(35))
                throw std::runtime_error("REL, packed Android relocations, and RELR are unsupported");
            if (image->tag(kRelaSize) && image->tag(kRelaEnt) != sizeof(Rela))
                throw std::runtime_error("invalid DT_RELAENT");
            relocateTable(*image, image->tag(kRela), image->tag(kRelaSize));
            if (image->tag(kPltRelSize) && image->tag(kPltRel) != uint64_t(kRela))
                throw std::runtime_error("PLT relocations are not RELA");
            relocateTable(*image, image->tag(kJumpRel), image->tag(kPltRelSize));
        } catch (const std::exception& error) {
            issues_.push_back({image->report.library, "relocation", error.what()});
        }
    }
    relocated_ = true;
}

bool Loader::complete() const {
    if (!relocated_ || !issues_.empty()) return false;
    for (const auto& image : images_) if (!image->report.missing_symbols.empty()) return false;
    return true;
}

void Loader::prepareExecution() {
    if (!complete()) throw std::runtime_error("dependency, relocation, or page-layout blockers prevent execution");
    for (auto& image : images_) image->protect();
    executable_ = true;
}

void Loader::initializeImage(Image& image, std::set<Image*>& visited) {
    if (!visited.insert(&image).second) return;
    for (const auto& name : image.report.needed) {
        auto found = by_name_.find(name);
        if (found != by_name_.end()) initializeImage(*found->second, visited);
    }
    auto invoke = [&](uintptr_t address) {
        if (!address || address == std::numeric_limits<uintptr_t>::max()) return;
        if (!image.executableAddress(address)) throw std::runtime_error("constructor is outside its executable PT_LOAD");
        char process_name[] = "wrapper-native-poc";
        char* argv[] = {process_name, nullptr};
        char* envp[] = {nullptr};
        reinterpret_cast<void (*)(int, char**, char**)>(address)(1, argv, envp);
        ++image.report.constructors_called;
    };
    if (image.tag(kInit)) invoke(image.bias_ + image.tag(kInit));
    uint64_t size = image.tag(kInitArraySize);
    if (size % sizeof(uintptr_t)) throw std::runtime_error("invalid init-array size");
    if (size) image.at(image.tag(kInitArray), size);
    for (uint64_t offset = 0; offset < size; offset += sizeof(uintptr_t))
        invoke(read<uintptr_t>(image.at(add(image.tag(kInitArray), offset), sizeof(uintptr_t))));
}

void Loader::initialize() {
    if (!executable_ || initialized_) throw std::runtime_error("invalid constructor initialization state");
    std::set<Image*> visited;
    for (auto& image : images_) initializeImage(*image, visited);
    initialized_ = true;
}

void* Loader::symbol(const std::string& library, const std::string& name) const {
    auto found = by_name_.find(library);
    if (found == by_name_.end()) throw std::runtime_error("ELF image is not loaded: " + library);
    void* address = found->second->exported(name);
    if (!address) throw std::runtime_error("ELF symbol is not exported: " + name);
    return address;
}

std::string jsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 32 || c >= 127) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
                else out << char(c);
        }
    }
    out << '"';
    return out.str();
}

std::string loaderReport(const Loader& loader) {
    std::ostringstream out;
    out << "{\"host_architecture\":\"arm64\",\"host_page_size\":" << getpagesize()
        << ",\"relocated_without_blockers\":" << (loader.complete() ? "true" : "false")
        << ",\"execution_prepared\":" << (loader.executionPrepared() ? "true" : "false")
        << ",\"constructors_initialized\":" << (loader.initialized() ? "true" : "false")
        << ",\"guarded_imports_are_traps\":true"
        << ",\"system_shim_libraries\":";
    stringArray(out, loader.systemShims());
    out << ",\"issues\":[";
    bool first = true;
    for (const auto& issue : loader.issues()) {
        if (!first) out << ',';
        out << "{\"library\":" << jsonString(issue.library) << ",\"kind\":" << jsonString(issue.kind)
            << ",\"detail\":" << jsonString(issue.detail) << '}';
        first = false;
    }
    out << "],\"images\":[";
    first = true;
    for (const auto& image : loader.images()) {
        const auto& r = image->report;
        if (!first) out << ',';
        out << "{\"library\":" << jsonString(r.library) << ",\"mapped_bytes\":" << r.mapped_bytes
            << ",\"relocations_applied\":" << r.relocations_applied << ",\"weak_unresolved\":" << r.weak_unresolved
            << ",\"mixed_write_execute_pages\":" << r.mixed_write_execute_pages
            << ",\"constructors_called\":" << r.constructors_called << ",\"needed\":";
        stringArray(out, r.needed);
        out << ",\"guarded_symbols\":";
        stringArray(out, r.guarded_symbols);
        out << ",\"missing_symbols\":";
        stringArray(out, r.missing_symbols);
        out << '}';
        first = false;
    }
    out << "]}";
    return out.str();
}

} // namespace native_poc
