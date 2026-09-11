#include "cosy/architecture.hpp"

#if defined(__x86_64__)

#include <array>
#include <sys/ptrace.h>
#include <sys/user.h>

namespace cosy {
namespace {

user_regs_struct get_regs(pid_t tid) {
    user_regs_struct regs{};
    check_syscall(ptrace(PTRACE_GETREGS, tid, nullptr, &regs), "PTRACE_GETREGS");
    return regs;
}

void set_regs(pid_t tid, const user_regs_struct& regs) {
    check_syscall(ptrace(PTRACE_SETREGS, tid, nullptr, &regs), "PTRACE_SETREGS");
}

long debug_register(pid_t tid, unsigned index) {
    errno = 0;
    const auto value = ptrace(PTRACE_PEEKUSER, tid,
                              static_cast<void*>(reinterpret_cast<void*>(
                                  offsetof(user, u_debugreg) + index * sizeof(long))),
                              nullptr);
    if (value == -1 && errno != 0) {
        throw SystemError("PTRACE_PEEKUSER debug register");
    }
    return value;
}

void set_debug_register(pid_t tid, unsigned index, long value) {
    check_syscall(ptrace(PTRACE_POKEUSER, tid,
                         static_cast<void*>(reinterpret_cast<void*>(
                             offsetof(user, u_debugreg) + index * sizeof(long))),
                         static_cast<void*>(reinterpret_cast<void*>(value))),
                  "PTRACE_POKEUSER debug register");
}

std::optional<Address> find_register(const user_regs_struct& r, std::string_view name) {
    const std::array<std::pair<std::string_view, Address>, 27> values{{
        {"rax", r.rax}, {"rbx", r.rbx}, {"rcx", r.rcx}, {"rdx", r.rdx},
        {"rsi", r.rsi}, {"rdi", r.rdi}, {"rbp", r.rbp}, {"rsp", r.rsp},
        {"r8", r.r8}, {"r9", r.r9}, {"r10", r.r10}, {"r11", r.r11},
        {"r12", r.r12}, {"r13", r.r13}, {"r14", r.r14}, {"r15", r.r15},
        {"rip", r.rip}, {"eflags", r.eflags}, {"cs", r.cs}, {"ss", r.ss},
        {"ds", r.ds}, {"es", r.es}, {"fs", r.fs}, {"gs", r.gs},
        {"fs_base", r.fs_base}, {"gs_base", r.gs_base}, {"orig_rax", r.orig_rax},
    }};
    for (const auto& [candidate, value] : values) {
        if (candidate == name) {
            return value;
        }
    }
    return std::nullopt;
}

class X86_64Architecture final : public Architecture {
public:
    [[nodiscard]] std::string name() const override { return "x86-64"; }
    [[nodiscard]] std::size_t breakpoint_size() const override { return 1; }
    [[nodiscard]] std::vector<std::uint8_t> breakpoint_instruction() const override { return {0xccU}; }

    [[nodiscard]] Address pc(pid_t tid) const override { return get_regs(tid).rip; }
    void set_pc(pid_t tid, Address value) const override {
        auto regs = get_regs(tid);
        regs.rip = value;
        set_regs(tid, regs);
    }
    [[nodiscard]] Address frame_pointer(pid_t tid) const override { return get_regs(tid).rbp; }
    [[nodiscard]] Address return_address(pid_t tid) const override {
        const auto frame = frame_pointer(tid);
        errno = 0;
        const auto result = ptrace(PTRACE_PEEKDATA, tid,
                                   static_cast<void*>(reinterpret_cast<void*>(frame + 8U)), nullptr);
        if (result == -1 && errno != 0) {
            throw SystemError("PTRACE_PEEKDATA return address");
        }
        return static_cast<Address>(result);
    }

    [[nodiscard]] std::vector<RegisterValue> registers(pid_t tid) const override {
        const auto r = get_regs(tid);
        return {
            {"rax", r.rax}, {"rbx", r.rbx}, {"rcx", r.rcx}, {"rdx", r.rdx},
            {"rsi", r.rsi}, {"rdi", r.rdi}, {"rbp", r.rbp}, {"rsp", r.rsp},
            {"r8", r.r8}, {"r9", r.r9}, {"r10", r.r10}, {"r11", r.r11},
            {"r12", r.r12}, {"r13", r.r13}, {"r14", r.r14}, {"r15", r.r15},
            {"rip", r.rip}, {"eflags", r.eflags}, {"cs", r.cs}, {"ss", r.ss},
            {"fs_base", r.fs_base}, {"gs_base", r.gs_base},
        };
    }

    [[nodiscard]] std::optional<Address> register_value(pid_t tid, std::string_view name) const override {
        return find_register(get_regs(tid), name);
    }

    [[nodiscard]] std::optional<Address> dwarf_register(pid_t tid, unsigned number) const override {
        static constexpr std::array<std::string_view, 17> dwarf_names{
            "rax", "rdx", "rcx", "rbx", "rsi", "rdi", "rbp", "rsp",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip",
        };
        if (number >= dwarf_names.size()) {
            return std::nullopt;
        }
        return register_value(tid, dwarf_names[number]);
    }

    bool set_register_value(pid_t tid, std::string_view name, Address value) const override {
        auto r = get_regs(tid);
        if (name == "rax") r.rax = value; else if (name == "rbx") r.rbx = value;
        else if (name == "rcx") r.rcx = value; else if (name == "rdx") r.rdx = value;
        else if (name == "rsi") r.rsi = value; else if (name == "rdi") r.rdi = value;
        else if (name == "rbp") r.rbp = value; else if (name == "rsp") r.rsp = value;
        else if (name == "r8") r.r8 = value; else if (name == "r9") r.r9 = value;
        else if (name == "r10") r.r10 = value; else if (name == "r11") r.r11 = value;
        else if (name == "r12") r.r12 = value; else if (name == "r13") r.r13 = value;
        else if (name == "r14") r.r14 = value; else if (name == "r15") r.r15 = value;
        else if (name == "rip") r.rip = value; else if (name == "eflags") r.eflags = value;
        else return false;
        set_regs(tid, r);
        return true;
    }

    void configure_watchpoint(pid_t tid, unsigned slot, Address address,
                              WatchAccess access, unsigned bytes) const override {
        if (slot >= 4U) {
            throw std::runtime_error("x86-64 supports four hardware watchpoints");
        }
        unsigned length = 0;
        switch (bytes) {
        case 1U: length = 0U; break;
        case 2U: length = 1U; break;
        case 4U: length = 3U; break;
        case 8U: length = 2U; break;
        default: throw std::runtime_error("watchpoint size must be 1, 2, 4, or 8");
        }
        if ((address % bytes) != 0U) {
            throw std::runtime_error("x86-64 watchpoint address must be naturally aligned");
        }
        const unsigned access_bits = access == WatchAccess::write ? 1U : 3U;
        const auto shift = 16U + slot * 4U;
        auto control = static_cast<unsigned long>(debug_register(tid, 7U));
        control &= ~(0xfUL << shift);
        control &= ~(3UL << (slot * 2U));
        control |= 1UL << (slot * 2U);
        control |= static_cast<unsigned long>(access_bits | (length << 2U)) << shift;
        set_debug_register(tid, slot, static_cast<long>(address));
        set_debug_register(tid, 7U, static_cast<long>(control));
    }

    void clear_watchpoint(pid_t tid, unsigned slot) const override {
        if (slot >= 4U) {
            return;
        }
        const auto shift = 16U + slot * 4U;
        auto control = static_cast<unsigned long>(debug_register(tid, 7U));
        control &= ~(0xfUL << shift);
        control &= ~(3UL << (slot * 2U));
        set_debug_register(tid, slot, 0L);
        set_debug_register(tid, 7U, static_cast<long>(control));
    }
};

}  // namespace

std::unique_ptr<Architecture> make_x86_64_architecture() {
    return std::make_unique<X86_64Architecture>();
}

std::unique_ptr<Architecture> make_aarch64_architecture() {
    return nullptr;
}

}  // namespace cosy
#else
namespace cosy {
std::unique_ptr<Architecture> make_x86_64_architecture() { return nullptr; }
}
#endif
