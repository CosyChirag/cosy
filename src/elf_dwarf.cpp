#include "cosy/elf_dwarf.hpp"

#include <dwarf.h>
#include <elf.h>
#include <fcntl.h>
#include <gelf.h>
#include <libdw.h>
#include <libelf.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace cosy {
namespace {

struct Symbol {
    std::string name;
    Address value = 0;
    Address size = 0;
};

struct Mapping {
    Address start = 0;
    Address end = 0;
    Address offset = 0;
    std::string permissions;
    std::string path;
};

std::vector<Mapping> read_maps(pid_t pid) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    if (!maps) {
        throw std::runtime_error("cannot read target memory maps");
    }
    std::vector<Mapping> result;
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream input(line);
        std::string range;
        Mapping mapping;
        std::string device;
        std::string inode;
        if (!(input >> range >> mapping.permissions >> std::hex >> mapping.offset >> device >> inode)) {
            continue;
        }
        std::getline(input, mapping.path);
        mapping.path = trim(mapping.path);
        const auto dash = range.find('-');
        if (dash == std::string::npos) {
            continue;
        }
        const auto start = parse_address("0x" + range.substr(0, dash));
        const auto end = parse_address("0x" + range.substr(dash + 1));
        if (!start || !end || mapping.path.empty() || mapping.path.front() == '[') {
            continue;
        }
        mapping.start = *start;
        mapping.end = *end;
        result.push_back(std::move(mapping));
    }
    return result;
}

bool module_matches(const std::string& path, std::string_view requested) {
    if (requested.empty()) {
        return true;
    }
    const auto filename = std::filesystem::path(path).filename().string();
    return path == requested || filename == requested || path.find(requested) != std::string::npos;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

class DebugFile {
public:
    DebugFile(std::string path, Address load_bias, Address end)
        : info_{std::move(path), load_bias, end} {
        if (elf_version(EV_CURRENT) == EV_NONE) {
            throw std::runtime_error("libelf initialization failed");
        }
        fd_ = open(info_.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) {
            throw SystemError("open ELF file");
        }
        elf_ = elf_begin(fd_, ELF_C_READ, nullptr);
        if (elf_ == nullptr) {
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("cannot parse ELF file " + info_.path);
        }
        GElf_Ehdr header{};
        if (gelf_getehdr(elf_, &header) == nullptr) {
            throw std::runtime_error("cannot read ELF header " + info_.path);
        }
        dynamic_ = header.e_type == ET_DYN;
        dwarf_ = dwarf_begin_elf(elf_, DWARF_C_READ, nullptr);
        read_symbols();
    }

    ~DebugFile() {
        if (dwarf_ != nullptr) {
            dwarf_end(dwarf_);
        }
        if (elf_ != nullptr) {
            elf_end(elf_);
        }
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    DebugFile(const DebugFile&) = delete;
    DebugFile& operator=(const DebugFile&) = delete;

    [[nodiscard]] const ModuleInfo& info() const { return info_; }
    [[nodiscard]] bool contains(Address address) const {
        return address >= runtime_base() && address < info_.end;
    }

    [[nodiscard]] Address runtime_base() const { return dynamic_ ? info_.base : 0; }
    [[nodiscard]] Address to_runtime(Address address) const { return runtime_base() + address; }
    [[nodiscard]] Address to_dwarf(Address address) const { return address - runtime_base(); }

    [[nodiscard]] std::optional<Address> symbol(std::string_view name) const {
        for (const auto& item : symbols_) {
            if (item.name == name) {
                return to_runtime(item.value);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string function(Address address) const {
        const auto relative = to_dwarf(address);
        const Symbol* closest = nullptr;
        for (const auto& symbol : symbols_) {
            if (symbol.value <= relative &&
                (symbol.size == 0 || relative < symbol.value + symbol.size) &&
                (closest == nullptr || symbol.value > closest->value)) {
                closest = &symbol;
            }
        }
        if (closest == nullptr) {
            return {};
        }
        return closest->name;
    }

    [[nodiscard]] SourceLocation location(Address address) const {
        SourceLocation result;
        result.function = function(address);
        if (dwarf_ == nullptr) {
            return result;
        }
        Dwarf_Die cu{};
        if (dwarf_addrdie(dwarf_, to_dwarf(address), &cu) == nullptr) {
            return result;
        }
        Dwarf_Line* line = dwarf_getsrc_die(&cu, to_dwarf(address));
        if (line == nullptr) {
            return result;
        }
        const char* path = dwarf_linesrc(line, nullptr, nullptr);
        int line_number = 0;
        int column = 0;
        if (path != nullptr && dwarf_lineno(line, &line_number) == 0) {
            result.file = path;
            result.line = line_number;
            static_cast<void>(dwarf_linecol(line, &column));
            result.column = column;
        }
        return result;
    }

    [[nodiscard]] std::vector<Address> source_line(std::string_view file, int wanted_line) const {
        std::vector<Address> result;
        if (dwarf_ == nullptr) {
            return result;
        }
        Dwarf_Off offset = 0;
        Dwarf_Off next = 0;
        size_t header_size = 0;
        Dwarf_Off abbrev = 0;
        std::uint8_t address_size = 0;
        std::uint8_t offset_size = 0;
        while (dwarf_nextcu(dwarf_, offset, &next, &header_size, &abbrev, &address_size, &offset_size) == 0) {
            Dwarf_Die cu{};
            if (dwarf_offdie(dwarf_, offset + header_size, &cu) != nullptr) {
                Dwarf_Lines* lines = nullptr;
                size_t count = 0;
                if (dwarf_getsrclines(&cu, &lines, &count) == 0) {
                    for (size_t index = 0; index < count; ++index) {
                        Dwarf_Line* entry = dwarf_onesrcline(lines, index);
                        const char* source = dwarf_linesrc(entry, nullptr, nullptr);
                        int line = 0;
                        bool statement = false;
                        Dwarf_Addr entry_address = 0;
                        if (source != nullptr && dwarf_lineno(entry, &line) == 0 &&
                            dwarf_linebeginstatement(entry, &statement) == 0 &&
                            dwarf_lineaddr(entry, &entry_address) == 0 &&
                            line == wanted_line &&
                            (file == source || ends_with(source, file)) && statement) {
                            result.push_back(to_runtime(entry_address));
                        }
                    }
                }
            }
            offset = next;
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    [[nodiscard]] std::vector<VariableValue> locals(
        pid_t tid, Address runtime_address, const std::function<Address(Address)>& read_word,
        const Architecture& architecture) const {
        std::vector<VariableValue> result;
        if (dwarf_ == nullptr) {
            return result;
        }
        Dwarf_Die cu{};
        const auto dwarf_address = to_dwarf(runtime_address);
        if (dwarf_addrdie(dwarf_, dwarf_address, &cu) == nullptr) {
            return result;
        }
        Dwarf_Die* scopes = nullptr;
        const int count = dwarf_getscopes(&cu, dwarf_address, &scopes);
        if (count <= 0 || scopes == nullptr) {
            return result;
        }
        std::set<std::string> names;
        for (int scope_index = 0; scope_index < count; ++scope_index) {
            Dwarf_Die child{};
            if (dwarf_child(&scopes[scope_index], &child) != 0) {
                continue;
            }
            while (true) {
                const int tag = dwarf_tag(&child);
                const char* name = dwarf_diename(&child);
                if ((tag == DW_TAG_variable || tag == DW_TAG_formal_parameter) &&
                    name != nullptr && names.insert(name).second) {
                    VariableValue variable;
                    variable.name = name;
                    Dwarf_Attribute location_attribute{};
                    if (dwarf_attr(&child, DW_AT_location, &location_attribute) != nullptr) {
                        Dwarf_Op* operations = nullptr;
                        size_t operation_count = 0;
                        if (dwarf_getlocation(&location_attribute, &operations, &operation_count) == 0) {
                            variable = evaluate_location(variable, operations, operation_count, tid,
                                                         read_word, architecture);
                        } else {
                            variable.value = "<optimized out>";
                        }
                    } else {
                        variable.value = "<no location>";
                    }
                    result.push_back(std::move(variable));
                }
                Dwarf_Die sibling{};
                if (dwarf_siblingof(&child, &sibling) != 0) {
                    break;
                }
                child = sibling;
            }
        }
        std::free(scopes);
        return result;
    }

private:
    static VariableValue evaluate_location(
        VariableValue variable, const Dwarf_Op* operations, size_t count, pid_t tid,
        const std::function<Address(Address)>& read_word, const Architecture& architecture) {
        std::vector<Address> stack;
        bool stack_value = false;
        bool register_value = false;
        for (size_t index = 0; index < count; ++index) {
            const auto& operation = operations[index];
            const auto atom = operation.atom;
            if (atom == DW_OP_addr) {
                stack.push_back(operation.number);
            } else if (atom >= DW_OP_reg0 && atom <= DW_OP_reg31) {
                const auto value = architecture.dwarf_register(tid, atom - DW_OP_reg0);
                if (!value) { variable.value = "<unknown register>"; return variable; }
                stack.push_back(*value);
                register_value = true;
            } else if (atom == DW_OP_regx) {
                const auto value = architecture.dwarf_register(tid, static_cast<unsigned>(operation.number));
                if (!value) { variable.value = "<unknown register>"; return variable; }
                stack.push_back(*value);
                register_value = true;
            } else if (atom >= DW_OP_breg0 && atom <= DW_OP_breg31) {
                const auto value = architecture.dwarf_register(tid, atom - DW_OP_breg0);
                if (!value) { variable.value = "<unknown register>"; return variable; }
                stack.push_back(*value + static_cast<Address>(operation.number2));
            } else if (atom == DW_OP_bregx) {
                const auto value = architecture.dwarf_register(tid, static_cast<unsigned>(operation.number));
                if (!value) { variable.value = "<unknown register>"; return variable; }
                stack.push_back(*value + static_cast<Address>(operation.number2));
            } else if (atom == DW_OP_fbreg) {
                stack.push_back(architecture.frame_pointer(tid) + 16U +
                                static_cast<Address>(operation.number));
            } else if (atom == DW_OP_deref) {
                if (stack.empty()) { variable.value = "<bad DWARF expression>"; return variable; }
                stack.back() = read_word(stack.back());
                register_value = true;
            } else if (atom == DW_OP_plus_uconst) {
                if (stack.empty()) { variable.value = "<bad DWARF expression>"; return variable; }
                stack.back() += operation.number;
            } else if (atom == DW_OP_stack_value) {
                stack_value = true;
            } else {
                variable.value = "<unsupported DWARF expression>";
                return variable;
            }
        }
        if (stack.empty()) {
            variable.value = "<unavailable>";
            return variable;
        }
        Address value = stack.back();
        if (!stack_value && !register_value) {
            value = read_word(value);
        }
        variable.value = hex(value);
        variable.available = true;
        return variable;
    }

    void read_symbols() {
        Elf_Scn* section = nullptr;
        while ((section = elf_nextscn(elf_, section)) != nullptr) {
            GElf_Shdr header{};
            if (gelf_getshdr(section, &header) == nullptr ||
                (header.sh_type != SHT_SYMTAB && header.sh_type != SHT_DYNSYM) ||
                header.sh_entsize == 0) {
                continue;
            }
            Elf_Data* data = elf_getdata(section, nullptr);
            if (data == nullptr) {
                continue;
            }
            const auto count = header.sh_size / header.sh_entsize;
            for (std::size_t index = 0; index < count; ++index) {
                GElf_Sym symbol{};
                if (gelf_getsym(data, static_cast<int>(index), &symbol) == nullptr ||
                    GELF_ST_TYPE(symbol.st_info) != STT_FUNC || symbol.st_value == 0) {
                    continue;
                }
                const char* name = elf_strptr(elf_, header.sh_link, symbol.st_name);
                if (name != nullptr && *name != '\0') {
                    symbols_.push_back({name, symbol.st_value, symbol.st_size});
                }
            }
        }
    }

    ModuleInfo info_;
    int fd_ = -1;
    Elf* elf_ = nullptr;
    Dwarf* dwarf_ = nullptr;
    bool dynamic_ = false;
    std::vector<Symbol> symbols_;
};

}  // namespace

struct ModuleManager::Impl {
    explicit Impl(std::string program) : main_program(std::move(program)) {}
    std::string main_program;
    std::vector<std::unique_ptr<DebugFile>> debug_files;
    std::vector<ModuleInfo> module_infos;
};

ModuleManager::ModuleManager(std::string main_program) : impl_(std::make_unique<Impl>(std::move(main_program))) {}
ModuleManager::~ModuleManager() = default;
ModuleManager::ModuleManager(ModuleManager&&) noexcept = default;
ModuleManager& ModuleManager::operator=(ModuleManager&&) noexcept = default;

void ModuleManager::refresh(pid_t pid) {
    const auto mappings = read_maps(pid);
    std::unordered_map<std::string, std::vector<Mapping>> grouped;
    for (const auto& mapping : mappings) {
        grouped[mapping.path].push_back(mapping);
    }
    std::vector<std::unique_ptr<DebugFile>> next;
    std::vector<ModuleInfo> infos;
    for (const auto& [path, group] : grouped) {
        Address bias = UINT64_MAX;
        Address end = 0;
        for (const auto& map : group) {
            bias = std::min(bias, map.start - map.offset);
            end = std::max(end, map.end);
        }
        try {
            auto file = std::make_unique<DebugFile>(path, bias, end);
            infos.push_back(file->info());
            next.push_back(std::move(file));
        } catch (const std::exception&) {
            // Mappings such as deleted files or vdso-like pseudo files need not be symbolized.
        }
    }
    impl_->debug_files = std::move(next);
    impl_->module_infos = std::move(infos);
}

const std::vector<ModuleInfo>& ModuleManager::modules() const { return impl_->module_infos; }

std::optional<Address> ModuleManager::resolve_symbol(std::string_view name, std::string_view module) const {
    for (const auto& file : impl_->debug_files) {
        if (module_matches(file->info().path, module)) {
            if (const auto address = file->symbol(name)) return address;
        }
    }
    return std::nullopt;
}

std::vector<Address> ModuleManager::resolve_source_line(std::string_view file, int line) const {
    std::vector<Address> result;
    for (const auto& debug_file : impl_->debug_files) {
        auto addresses = debug_file->source_line(file, line);
        result.insert(result.end(), addresses.begin(), addresses.end());
    }
    return result;
}

SourceLocation ModuleManager::location(Address runtime_address) const {
    for (const auto& file : impl_->debug_files) {
        if (file->contains(runtime_address)) return file->location(runtime_address);
    }
    return {};
}

std::string ModuleManager::function(Address runtime_address) const {
    for (const auto& file : impl_->debug_files) {
        if (file->contains(runtime_address)) return file->function(runtime_address);
    }
    return {};
}

std::vector<VariableValue> ModuleManager::locals(
    pid_t tid, Address runtime_address, const std::function<Address(Address)>& read_word,
    const Architecture& architecture) const {
    for (const auto& file : impl_->debug_files) {
        if (file->contains(runtime_address)) return file->locals(tid, runtime_address, read_word, architecture);
    }
    return {};
}

}  // namespace cosy
