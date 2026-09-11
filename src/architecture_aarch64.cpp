#include "cosy/architecture.hpp"

#if defined(__aarch64__)

#include <elf.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <asm/ptrace.h>

namespace cosy {
namespace {

struct HardwareDebugState {
    std::uint32_t info;
    struct Register {
        std::uint64_t address;
        std::uint32_t control;
        std::uint32_t padding;
    } registers[16];
};

user_pt_regs get_regs(pid_t tid) {
    user_pt_regs regs{};
    iovec io{&regs, sizeof(regs)};
    check_syscall(ptrace(PTRACE_GETREGSET, tid, reinterpret_cast<void*>(NT_PRSTATUS), &io),
                  "PTRACE_GETREGSET");
    return regs;
}

void set_regs(pid_t tid, const user_pt_regs& regs) {
    iovec io{const_cast<user_pt_regs*>(&regs), sizeof(regs)};
    check_syscall(ptrace(PTRACE_SETREGSET, tid, reinterpret_cast<void*>(NT_PRSTATUS), &io),
                  "PTRACE_SETREGSET");
}

class AArch64Architecture final : public Architecture {
public:
    [[nodiscard]] std::string name() const override { return "AArch64"; }
    [[nodiscard]] std::size_t breakpoint_size() const override { return 4U; }
    [[nodiscard]] std::vector<std::uint8_t> breakpoint_instruction() const override {
        return {0x00U, 0x00U, 0x20U, 0xd4U};
    }
    [[nodiscard]] Address pc(pid_t tid) const override { return get_regs(tid).pc; }
    void set_pc(pid_t tid, Address value) const override {
        auto regs = get_regs(tid);
        regs.pc = value;
        set_regs(tid, regs);
    }
    [[nodiscard]] Address frame_pointer(pid_t tid) const override { return get_regs(tid).regs[29]; }
    [[nodiscard]] Address return_address(pid_t tid) const override {
        const auto frame = frame_pointer(tid);
        errno = 0;
        const auto value = ptrace(PTRACE_PEEKDATA, tid, reinterpret_cast<void*>(frame + 8U), nullptr);
        if (value == -1 && errno != 0) throw SystemError("PTRACE_PEEKDATA return address");
        return static_cast<Address>(value);
    }
    [[nodiscard]] std::vector<RegisterValue> registers(pid_t tid) const override {
        const auto regs = get_regs(tid);
        std::vector<RegisterValue> values;
        for (unsigned index = 0; index < 31U; ++index) {
            values.push_back({"x" + std::to_string(index), regs.regs[index]});
        }
        values.push_back({"sp", regs.sp});
        values.push_back({"pc", regs.pc});
        values.push_back({"pstate", regs.pstate});
        return values;
    }
    [[nodiscard]] std::optional<Address> register_value(pid_t tid, std::string_view name) const override {
        const auto regs = get_regs(tid);
        if (name == "sp") return regs.sp;
        if (name == "pc") return regs.pc;
        if (name == "fp") return regs.regs[29];
        if (name == "lr") return regs.regs[30];
        if (name.size() >= 2U && name[0] == 'x') {
            const auto parsed = parse_address(name.substr(1));
            if (parsed && *parsed < 31U) return regs.regs[*parsed];
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Address> dwarf_register(pid_t tid, unsigned number) const override {
        if (number <= 30U) return get_regs(tid).regs[number];
        if (number == 31U) return get_regs(tid).sp;
        return std::nullopt;
    }
    bool set_register_value(pid_t tid, std::string_view name, Address value) const override {
        auto regs = get_regs(tid);
        if (name == "sp") regs.sp = value;
        else if (name == "pc") regs.pc = value;
        else if (name == "fp") regs.regs[29] = value;
        else if (name == "lr") regs.regs[30] = value;
        else if (name.size() >= 2U && name[0] == 'x') {
            const auto parsed = parse_address(name.substr(1));
            if (!parsed || *parsed >= 31U) return false;
            regs.regs[*parsed] = value;
        } else return false;
        set_regs(tid, regs);
        return true;
    }
    void configure_watchpoint(pid_t tid, unsigned slot, Address address,
                              WatchAccess access, unsigned bytes) const override {
        if (slot >= 16U) throw std::runtime_error("AArch64 has no free hardware watchpoint slot");
        if (bytes != 1U && bytes != 2U && bytes != 4U && bytes != 8U) {
            throw std::runtime_error("watchpoint size must be 1, 2, 4, or 8");
        }
        HardwareDebugState state{};
        iovec io{&state, sizeof(state)};
        check_syscall(ptrace(PTRACE_GETREGSET, tid, reinterpret_cast<void*>(NT_ARM_HW_WATCH), &io),
                      "PTRACE_GETREGSET hardware watchpoint");
        std::uint32_t mask = (1U << bytes) - 1U;
        const std::uint32_t type = access == WatchAccess::write ? 2U :
                                   access == WatchAccess::read ? 1U : 3U;
        state.registers[slot].address = address;
        state.registers[slot].control = 1U | (2U << 1U) | (type << 3U) | (mask << 5U);
        check_syscall(ptrace(PTRACE_SETREGSET, tid, reinterpret_cast<void*>(NT_ARM_HW_WATCH), &io),
                      "PTRACE_SETREGSET hardware watchpoint");
    }
    void clear_watchpoint(pid_t tid, unsigned slot) const override {
        HardwareDebugState state{};
        iovec io{&state, sizeof(state)};
        check_syscall(ptrace(PTRACE_GETREGSET, tid, reinterpret_cast<void*>(NT_ARM_HW_WATCH), &io),
                      "PTRACE_GETREGSET hardware watchpoint");
        if (slot < 16U) {
            state.registers[slot] = {};
            check_syscall(ptrace(PTRACE_SETREGSET, tid, reinterpret_cast<void*>(NT_ARM_HW_WATCH), &io),
                          "PTRACE_SETREGSET hardware watchpoint");
        }
    }
};
}  // namespace

std::unique_ptr<Architecture> make_aarch64_architecture() {
    return std::make_unique<AArch64Architecture>();
}
}  // namespace cosy
#endif
