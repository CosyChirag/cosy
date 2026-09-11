#pragma once

#include "cosy/architecture.hpp"

#include <memory>

namespace cosy {

struct SourceLocation {
    std::string file;
    int line = 0;
    int column = 0;
    std::string function;

    [[nodiscard]] bool valid() const { return !file.empty() && line > 0; }
};

struct ModuleInfo {
    std::string path;
    Address base = 0;
    Address end = 0;
};

struct VariableValue {
    std::string name;
    std::string value;
    bool available = false;
};

class ModuleManager {
public:
    explicit ModuleManager(std::string main_program);
    ~ModuleManager();
    ModuleManager(ModuleManager&&) noexcept;
    ModuleManager& operator=(ModuleManager&&) noexcept;
    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;

    void refresh(pid_t pid);
    [[nodiscard]] const std::vector<ModuleInfo>& modules() const;
    [[nodiscard]] std::optional<Address> resolve_symbol(std::string_view name,
                                                         std::string_view module = {}) const;
    [[nodiscard]] std::vector<Address> resolve_source_line(std::string_view file, int line) const;
    [[nodiscard]] SourceLocation location(Address runtime_address) const;
    [[nodiscard]] std::string function(Address runtime_address) const;
    [[nodiscard]] std::vector<VariableValue> locals(
        pid_t tid, Address runtime_address,
        const std::function<Address(Address)>& read_word,
        const Architecture& architecture) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cosy
