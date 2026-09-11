#pragma once

#include "cosy/common.hpp"

#include <memory>
#include <sys/types.h>

namespace cosy {

enum class WatchAccess { read, write, access };

struct RegisterValue {
    std::string name;
    Address value;
};

class Architecture {
public:
    virtual ~Architecture() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::size_t breakpoint_size() const = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> breakpoint_instruction() const = 0;
    [[nodiscard]] virtual Address pc(pid_t tid) const = 0;
    virtual void set_pc(pid_t tid, Address value) const = 0;
    [[nodiscard]] virtual Address frame_pointer(pid_t tid) const = 0;
    [[nodiscard]] virtual Address return_address(pid_t tid) const = 0;
    [[nodiscard]] virtual std::vector<RegisterValue> registers(pid_t tid) const = 0;
    [[nodiscard]] virtual std::optional<Address> register_value(pid_t tid, std::string_view name) const = 0;
    [[nodiscard]] virtual std::optional<Address> dwarf_register(pid_t tid, unsigned number) const = 0;
    virtual bool set_register_value(pid_t tid, std::string_view name, Address value) const = 0;
    virtual void configure_watchpoint(pid_t tid, unsigned slot, Address address,
                                      WatchAccess access, unsigned bytes) const = 0;
    virtual void clear_watchpoint(pid_t tid, unsigned slot) const = 0;
};

std::unique_ptr<Architecture> make_host_architecture();

}  // namespace cosy
