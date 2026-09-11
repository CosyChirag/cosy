#include "cosy/debugger.hpp"

#include "cosy/architecture.hpp"
#include "cosy/common.hpp"
#include "cosy/elf_dwarf.hpp"

#include <readline/history.h>
#include <readline/readline.h>

#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cosy {
namespace {

volatile sig_atomic_t interrupted_pid = 0;

extern "C" void on_interrupt(int) {
    if (interrupted_pid > 0) {
        static_cast<void>(kill(interrupted_pid, SIGSTOP));
    }
}

struct Breakpoint {
    int id = 0;
    Address address = 0;
    std::string specification;
    std::vector<std::uint8_t> saved;
    bool enabled = true;
    bool temporary = false;
    bool pending = false;
};

struct Watchpoint {
    int id = 0;
    unsigned slot = 0;
    Address address = 0;
    WatchAccess access = WatchAccess::write;
    unsigned bytes = 8;
};

struct ThreadState {
    pid_t tid = 0;
    bool stopped = false;
    int signal = 0;
};

bool is_alias(std::string_view command, std::initializer_list<std::string_view> names) {
    for (const auto name : names) {
        if (command == name) {
            return true;
        }
    }
    return false;
}

std::string signal_name(int signal) {
    const char* text = strsignal(signal);
    return text == nullptr ? std::to_string(signal) : text;
}

}  // namespace

class Debugger::Impl {
public:
    Impl(std::vector<std::string> target_argv, bool batch)
        : target_argv_(std::move(target_argv)),
          batch_(batch),
          architecture_(make_host_architecture()),
          modules_(target_argv_.front()) {}

    ~Impl() { detach(); }

    int run() {
        launch();
        print_banner();
        print_stop(selected_tid_);
        command_loop();
        return 0;
    }

private:
    void launch() {
        child_ = fork();
        if (child_ < 0) {
            throw SystemError("fork");
        }
        if (child_ == 0) {
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1) {
                _exit(127);
            }
            std::vector<char*> arguments;
            arguments.reserve(target_argv_.size() + 1U);
            for (auto& value : target_argv_) {
                arguments.push_back(value.data());
            }
            arguments.push_back(nullptr);
            execvp(arguments.front(), arguments.data());
            _exit(127);
        }

        const auto status = wait_for(child_);
        if (!WIFSTOPPED(status)) {
            throw std::runtime_error("target did not stop after exec");
        }
        threads_.emplace(child_, ThreadState{child_, true, WSTOPSIG(status)});
        selected_tid_ = child_;
        set_options(child_);
        refresh_modules();
        resolve_pending_breakpoints();
        interrupted_pid = child_;
        std::signal(SIGINT, on_interrupt);
    }

    void set_options(pid_t tid) {
        constexpr long options = PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT |
                                 PTRACE_O_EXITKILL;
        check_syscall(ptrace(PTRACE_SETOPTIONS, tid, nullptr, reinterpret_cast<void*>(options)),
                      "PTRACE_SETOPTIONS");
    }

    int wait_for(pid_t tid) {
        int status = 0;
        while (waitpid(tid, &status, __WALL) == -1) {
            if (errno != EINTR) {
                throw SystemError("waitpid");
            }
        }
        return status;
    }

    std::pair<pid_t, int> wait_for_event() {
        int status = 0;
        pid_t tid = 0;
        while ((tid = waitpid(-1, &status, __WALL)) == -1) {
            if (errno != EINTR) {
                throw SystemError("waitpid");
            }
        }
        return {tid, status};
    }

    Address read_word(Address address) const {
        errno = 0;
        const auto value = ptrace(PTRACE_PEEKDATA, selected_tid_, reinterpret_cast<void*>(address), nullptr);
        if (value == -1 && errno != 0) {
            throw SystemError("PTRACE_PEEKDATA");
        }
        return static_cast<Address>(value);
    }

    std::vector<std::uint8_t> read_bytes(Address address, std::size_t size) const {
        std::vector<std::uint8_t> result(size);
        for (std::size_t index = 0; index < size; ++index) {
            const auto location = address + index;
            const auto word = read_word(location & ~Address{7});
            const auto shift = static_cast<unsigned>((location & 7U) * 8U);
            result[index] = static_cast<std::uint8_t>((word >> shift) & 0xffU);
        }
        return result;
    }

    void write_bytes(Address address, const std::vector<std::uint8_t>& bytes) const {
        std::map<Address, Address> words;
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const auto location = address + index;
            const auto aligned = location & ~Address{7};
            if (!words.contains(aligned)) {
                words.emplace(aligned, read_word(aligned));
            }
            const auto shift = static_cast<unsigned>((location & 7U) * 8U);
            auto& word = words.at(aligned);
            word = (word & ~(Address{0xff} << shift)) | (static_cast<Address>(bytes[index]) << shift);
        }
        for (const auto& [aligned, word] : words) {
            check_syscall(ptrace(PTRACE_POKEDATA, selected_tid_, reinterpret_cast<void*>(aligned),
                                 reinterpret_cast<void*>(word)),
                          "PTRACE_POKEDATA");
        }
    }

    void enable(Breakpoint& breakpoint) {
        if (breakpoint.pending || breakpoint.enabled && !breakpoint.saved.empty()) {
            return;
        }
        breakpoint.saved = read_bytes(breakpoint.address, architecture_->breakpoint_size());
        write_bytes(breakpoint.address, architecture_->breakpoint_instruction());
        breakpoint.enabled = true;
    }

    void disable(Breakpoint& breakpoint) {
        if (breakpoint.pending || !breakpoint.enabled || breakpoint.saved.empty()) {
            return;
        }
        write_bytes(breakpoint.address, breakpoint.saved);
        breakpoint.enabled = false;
    }

    Breakpoint* breakpoint_at(Address address) {
        const auto found = breakpoints_.find(address);
        return found == breakpoints_.end() ? nullptr : &found->second;
    }

    int create_breakpoint(Address address, std::string specification, bool temporary = false) {
        if (auto* existing = breakpoint_at(address)) {
            return existing->id;
        }
        Breakpoint breakpoint;
        breakpoint.id = next_breakpoint_id_++;
        breakpoint.address = address;
        breakpoint.specification = std::move(specification);
        breakpoint.temporary = temporary;
        breakpoint.enabled = false;
        auto [position, inserted] = breakpoints_.emplace(address, std::move(breakpoint));
        if (inserted) {
            enable(position->second);
        }
        return position->second.id;
    }

    int create_pending_breakpoint(std::string specification) {
        Breakpoint breakpoint;
        breakpoint.id = next_breakpoint_id_++;
        breakpoint.specification = std::move(specification);
        breakpoint.pending = true;
        breakpoint.enabled = false;
        pending_breakpoints_.push_back(std::move(breakpoint));
        return pending_breakpoints_.back().id;
    }

    void remove_breakpoint(int id) {
        for (auto iterator = breakpoints_.begin(); iterator != breakpoints_.end(); ++iterator) {
            if (iterator->second.id == id) {
                disable(iterator->second);
                breakpoints_.erase(iterator);
                return;
            }
        }
        pending_breakpoints_.erase(
            std::remove_if(pending_breakpoints_.begin(), pending_breakpoints_.end(),
                           [id](const Breakpoint& breakpoint) { return breakpoint.id == id; }),
            pending_breakpoints_.end());
    }

    void refresh_modules() {
        modules_.refresh(child_);
    }

    std::vector<Address> resolve_specification(const std::string& specification) const {
        if (specification.starts_with('*')) {
            if (const auto address = parse_address(specification.substr(1))) return {*address};
            return {};
        }
        const auto exclamation = specification.find('!');
        if (exclamation != std::string::npos) {
            const auto address = modules_.resolve_symbol(specification.substr(exclamation + 1),
                                                         specification.substr(0, exclamation));
            return address ? std::vector<Address>{*address} : std::vector<Address>{};
        }
        const auto colon = specification.rfind(':');
        if (colon != std::string::npos) {
            try {
                return modules_.resolve_source_line(specification.substr(0, colon),
                                                    std::stoi(specification.substr(colon + 1)));
            } catch (const std::exception&) {
                return {};
            }
        }
        const auto address = modules_.resolve_symbol(specification);
        return address ? std::vector<Address>{*address} : std::vector<Address>{};
    }

    void resolve_pending_breakpoints() {
        for (auto iterator = pending_breakpoints_.begin(); iterator != pending_breakpoints_.end();) {
            const auto resolved = resolve_specification(iterator->specification);
            if (resolved.empty()) {
                ++iterator;
                continue;
            }
            for (const auto address : resolved) {
                const auto id = create_breakpoint(address, iterator->specification);
                std::cout << "resolved pending breakpoint " << id << " at " << hex(address) << '\n';
            }
            iterator = pending_breakpoints_.erase(iterator);
        }
    }

    void set_breakpoint(const std::string& specification) {
        const auto resolved = resolve_specification(specification);
        if (resolved.empty()) {
            const auto id = create_pending_breakpoint(specification);
            std::cout << "breakpoint " << id << " pending: " << specification << '\n';
            return;
        }
        for (const auto address : resolved) {
            const auto id = create_breakpoint(address, specification);
            std::cout << "breakpoint " << id << " at " << hex(address) << '\n';
        }
    }

    void configure_thread_watchpoints(pid_t tid) {
        for (const auto& watchpoint : watchpoints_) {
            architecture_->configure_watchpoint(tid, watchpoint.slot, watchpoint.address,
                                                watchpoint.access, watchpoint.bytes);
        }
    }

    void set_watchpoint(const std::vector<std::string>& arguments) {
        if (arguments.size() < 2U) {
            std::cout << "usage: watch ADDRESS [r|w|rw] [1|2|4|8]\n";
            return;
        }
        const auto address = parse_address(arguments[1]);
        if (!address) {
            std::cout << "invalid watchpoint address\n";
            return;
        }
        WatchAccess access = WatchAccess::write;
        if (arguments.size() >= 3U) {
            if (arguments[2] == "r") access = WatchAccess::read;
            else if (arguments[2] == "rw") access = WatchAccess::access;
            else if (arguments[2] != "w") { std::cout << "access must be r, w, or rw\n"; return; }
        }
        unsigned bytes = 8U;
        if (arguments.size() >= 4U) {
            const auto parsed = parse_address(arguments[3]);
            if (!parsed) { std::cout << "invalid watchpoint size\n"; return; }
            bytes = static_cast<unsigned>(*parsed);
        }
        std::set<unsigned> occupied;
        for (const auto& watchpoint : watchpoints_) occupied.insert(watchpoint.slot);
        unsigned slot = 0;
        while (occupied.contains(slot)) ++slot;
        if (slot >= 4U) {
            std::cout << "no free hardware watchpoint slot\n";
            return;
        }
        try {
            for (const auto& [tid, state] : threads_) {
                if (state.stopped) architecture_->configure_watchpoint(tid, slot, *address, access, bytes);
            }
            watchpoints_.push_back({next_watchpoint_id_++, slot, *address, access, bytes});
            std::cout << "watchpoint " << watchpoints_.back().id << " at " << hex(*address) << '\n';
        } catch (const std::exception& error) {
            std::cout << "watchpoint failed: " << error.what() << '\n';
        }
    }

    void step_over_breakpoint() {
        const auto pc = architecture_->pc(selected_tid_);
        auto* breakpoint = breakpoint_at(pc);
        if (breakpoint == nullptr || !breakpoint->enabled) {
            return;
        }
        disable(*breakpoint);
        check_syscall(ptrace(PTRACE_SINGLESTEP, selected_tid_, nullptr, nullptr), "PTRACE_SINGLESTEP");
        threads_.at(selected_tid_).stopped = false;
        const auto [tid, status] = wait_for_event();
        threads_[tid] = ThreadState{tid, WIFSTOPPED(status), WIFSTOPPED(status) ? WSTOPSIG(status) : 0};
        if (WIFSTOPPED(status)) {
            if (tid == selected_tid_) {
                enable(*breakpoint);
            } else {
                enable(*breakpoint);
                selected_tid_ = tid;
                print_stop(tid);
            }
        }
    }

    void resume_thread(pid_t tid, enum __ptrace_request request, int signal = 0) {
        auto position = threads_.find(tid);
        if (position == threads_.end() || !position->second.stopped) {
            return;
        }
        check_syscall(ptrace(request, tid, nullptr, reinterpret_cast<void*>(static_cast<intptr_t>(signal))),
                      request == PTRACE_CONT ? "PTRACE_CONT" : "PTRACE_SINGLESTEP");
        position->second.stopped = false;
        position->second.signal = 0;
    }

    void continue_execution() {
        if (!alive_) {
            std::cout << "target has exited\n";
            return;
        }
        step_over_breakpoint();
        for (const auto& [tid, state] : threads_) {
            if (state.stopped) {
                resume_thread(tid, PTRACE_CONT);
            }
        }
        wait_and_report();
    }

    void instruction_step(bool report = true) {
        if (!alive_) {
            return;
        }
        step_over_breakpoint();
        resume_thread(selected_tid_, PTRACE_SINGLESTEP);
        wait_and_report(report);
    }

    void source_step() {
        const auto before = modules_.location(architecture_->pc(selected_tid_));
        if (!before.valid()) {
            instruction_step();
            return;
        }
        for (unsigned count = 0; count < 100000U; ++count) {
            instruction_step(false);
            if (!alive_) return;
            const auto after = modules_.location(architecture_->pc(selected_tid_));
            if (!after.valid() || after.file != before.file || after.line != before.line) {
                print_stop(selected_tid_);
                return;
            }
        }
        std::cout << "source step limit reached\n";
    }

    void step_out() {
        try {
            const auto return_address = architecture_->return_address(selected_tid_);
            const auto id = create_breakpoint(return_address, "<finish>", true);
            std::cout << "temporary breakpoint " << id << " at " << hex(return_address) << '\n';
            continue_execution();
        } catch (const std::exception& error) {
            std::cout << "cannot finish: " << error.what() << '\n';
        }
    }

    void wait_and_report(bool report = true) {
        const auto [tid, status] = wait_for_event();
        selected_tid_ = tid;
        if (WIFEXITED(status)) {
            threads_.erase(tid);
            if (tid == child_) alive_ = false;
            if (report) std::cout << "thread " << tid << " exited with status " << WEXITSTATUS(status) << '\n';
            return;
        }
        if (WIFSIGNALED(status)) {
            threads_.erase(tid);
            if (tid == child_) alive_ = false;
            if (report) std::cout << "thread " << tid << " terminated by " << signal_name(WTERMSIG(status)) << '\n';
            return;
        }
        if (!WIFSTOPPED(status)) {
            return;
        }
        threads_[tid] = ThreadState{tid, true, WSTOPSIG(status)};
        const unsigned event = static_cast<unsigned>(status) >> 16U;
        if (event == PTRACE_EVENT_CLONE) {
            unsigned long new_tid = 0;
            check_syscall(ptrace(PTRACE_GETEVENTMSG, tid, nullptr, &new_tid), "PTRACE_GETEVENTMSG");
            threads_.emplace(static_cast<pid_t>(new_tid), ThreadState{static_cast<pid_t>(new_tid), false, 0});
        }
        if (event == PTRACE_EVENT_EXEC) {
            refresh_modules();
            resolve_pending_breakpoints();
        }
        if (WSTOPSIG(status) == SIGSTOP) {
            try {
                set_options(tid);
                configure_thread_watchpoints(tid);
            } catch (const std::exception&) {
                // A dying clone can disappear before its initial stop is processed.
            }
        }
        refresh_modules();
        resolve_pending_breakpoints();
        if (report) print_stop(tid);
    }

    void print_source(const SourceLocation& location) const {
        if (!location.valid()) {
            return;
        }
        std::ifstream source(location.file);
        if (!source) {
            return;
        }
        const int first = std::max(1, location.line - 2);
        const int last = location.line + 2;
        std::string line;
        int number = 1;
        while (std::getline(source, line)) {
            if (number >= first && number <= last) {
                std::cout << (number == location.line ? "=>" : "  ") << std::setw(4) << number
                          << "  " << line << '\n';
            }
            if (number++ > last) break;
        }
    }

    void print_stop(pid_t tid) {
        if (!alive_ || !threads_.contains(tid) || !threads_.at(tid).stopped) {
            return;
        }
        const auto signal = threads_.at(tid).signal;
        if (signal == SIGTRAP) {
            try {
                auto pc = architecture_->pc(tid);
                const auto candidate = architecture_->name() == "x86-64"
                                           ? pc - architecture_->breakpoint_size() : pc;
                if (auto* breakpoint = breakpoint_at(candidate)) {
                    architecture_->set_pc(tid, candidate);
                    pc = candidate;
                    std::cout << "hit breakpoint " << breakpoint->id << " at " << hex(pc) << '\n';
                    if (breakpoint->temporary) {
                        const int id = breakpoint->id;
                        disable(*breakpoint);
                        breakpoints_.erase(candidate);
                        std::cout << "temporary breakpoint " << id << " removed\n";
                    }
                }
                for (const auto& watchpoint : watchpoints_) {
                    siginfo_t info{};
                    if (ptrace(PTRACE_GETSIGINFO, tid, nullptr, &info) == 0 && info.si_code == TRAP_HWBKPT) {
                        std::cout << "hit watchpoint " << watchpoint.id << " at " << hex(watchpoint.address) << '\n';
                        break;
                    }
                }
            } catch (const std::exception& error) {
                std::cout << "trap inspection failed: " << error.what() << '\n';
            }
        } else if (signal == SIGSTOP) {
            std::cout << "thread " << tid << " stopped\n";
        } else {
            std::cout << "thread " << tid << " stopped by " << signal_name(signal) << '\n';
        }
        try {
            const auto location = modules_.location(architecture_->pc(tid));
            const auto function = modules_.function(architecture_->pc(tid));
            if (location.valid()) {
                std::cout << "at " << (function.empty() ? "<unknown>" : function)
                          << " " << location.file << ':' << location.line << '\n';
                print_source(location);
            } else if (!function.empty()) {
                std::cout << "at " << function << " (" << hex(architecture_->pc(tid)) << ")\n";
            }
        } catch (const std::exception&) {
        }
    }

    void print_breakpoints() const {
        std::cout << "Num  Type          Address             Status  Location\n";
        for (const auto& [address, breakpoint] : breakpoints_) {
            std::cout << std::setw(3) << breakpoint.id << "  breakpoint    " << std::setw(18)
                      << hex(address) << "  " << (breakpoint.enabled ? "enabled " : "disabled")
                      << "  " << breakpoint.specification << '\n';
        }
        for (const auto& breakpoint : pending_breakpoints_) {
            std::cout << std::setw(3) << breakpoint.id << "  breakpoint    " << std::setw(18)
                      << "<pending>" << "  pending   " << breakpoint.specification << '\n';
        }
    }

    void print_threads() const {
        for (const auto& [tid, state] : threads_) {
            std::cout << (tid == selected_tid_ ? "* " : "  ") << tid << "  "
                      << (state.stopped ? "stopped" : "running");
            if (state.stopped && state.signal != 0) std::cout << " (" << signal_name(state.signal) << ')';
            std::cout << '\n';
        }
    }

    void print_modules() const {
        for (const auto& module : modules_.modules()) {
            std::cout << hex(module.base) << '-' << hex(module.end) << "  " << module.path << '\n';
        }
    }

    void print_registers() const {
        for (const auto& register_value : architecture_->registers(selected_tid_)) {
            std::cout << std::left << std::setw(10) << register_value.name
                      << std::right << hex(register_value.value) << '\n';
        }
    }

    std::vector<std::pair<Address, Address>> backtrace() const {
        std::vector<std::pair<Address, Address>> frames;
        Address pc = architecture_->pc(selected_tid_);
        Address frame = architecture_->frame_pointer(selected_tid_);
        std::set<Address> seen;
        for (unsigned depth = 0; depth < 64U; ++depth) {
            frames.push_back({pc, frame});
            if (frame == 0 || !seen.insert(frame).second) break;
            try {
                pc = read_word(frame + 8U);
                const auto next = read_word(frame);
                if (next <= frame) break;
                frame = next;
            } catch (const std::exception&) {
                break;
            }
        }
        return frames;
    }

    void print_backtrace() {
        const auto frames = backtrace();
        for (std::size_t index = 0; index < frames.size(); ++index) {
            const auto& [pc, frame] = frames[index];
            const auto location = modules_.location(pc);
            const auto function = modules_.function(pc);
            std::cout << (selected_frame_ == index ? "=>" : "  ") << '#' << index << ' '
                      << hex(pc) << " in " << (function.empty() ? "<unknown>" : function);
            if (location.valid()) std::cout << " at " << location.file << ':' << location.line;
            std::cout << " (fp=" << hex(frame) << ")\n";
        }
    }

    void print_locals(std::optional<std::string> wanted = std::nullopt) {
        if (selected_frame_ != 0U) {
            std::cout << "locals are available only for the current frame\n";
            return;
        }
        const auto pc = architecture_->pc(selected_tid_);
        const auto values = modules_.locals(selected_tid_, pc,
                                            [this](Address address) { return read_word(address); },
                                            *architecture_);
        bool found = false;
        for (const auto& value : values) {
            if (!wanted || value.name == *wanted) {
                std::cout << value.name << " = " << value.value << '\n';
                found = true;
            }
        }
        if (!found) std::cout << (wanted ? "variable not found" : "no local variables available") << '\n';
    }

    void print_memory(const std::vector<std::string>& arguments) {
        if (arguments.size() < 2U) { std::cout << "usage: x ADDRESS [COUNT]\n"; return; }
        const auto address = parse_address(arguments[1]);
        if (!address) { std::cout << "invalid address\n"; return; }
        unsigned count = 1;
        if (arguments.size() > 2U) {
            const auto parsed = parse_address(arguments[2]);
            if (!parsed || *parsed > 1024U) { std::cout << "invalid count\n"; return; }
            count = static_cast<unsigned>(*parsed);
        }
        for (unsigned index = 0; index < count; ++index) {
            const auto current = *address + index * sizeof(Address);
            try {
                std::cout << hex(current) << ": " << hex(read_word(current)) << '\n';
            } catch (const std::exception& error) {
                std::cout << hex(current) << ": " << error.what() << '\n';
                return;
            }
        }
    }

    void write_memory(const std::vector<std::string>& arguments) {
        if (arguments.size() != 4U || arguments[1] != "memory") {
            std::cout << "usage: set memory ADDRESS VALUE\n";
            return;
        }
        const auto address = parse_address(arguments[2]);
        const auto value = parse_address(arguments[3]);
        if (!address || !value) { std::cout << "invalid address or value\n"; return; }
        check_syscall(ptrace(PTRACE_POKEDATA, selected_tid_, reinterpret_cast<void*>(*address),
                             reinterpret_cast<void*>(*value)), "PTRACE_POKEDATA");
    }

    void print_help() const {
        std::cout
            << "break LOCATION          LOCATION is *0xADDR, symbol, lib.so!symbol, or file:line\n"
            << "continue | c            resume every stopped thread\n"
            << "stepi | si              execute one instruction on the selected thread\n"
            << "step | s                source-step selected thread\n"
            << "next | n                source-step selected thread without resuming others\n"
            << "finish                  run until current function returns\n"
            << "delete ID, enable ID, disable ID, info break\n"
            << "watch ADDRESS [r|w|rw] [1|2|4|8]\n"
            << "backtrace | bt, frame NUMBER, info locals, print NAME\n"
            << "info registers, register read NAME, register write NAME VALUE\n"
            << "x ADDRESS [COUNT], set memory ADDRESS VALUE\n"
            << "info threads, thread TID, info sharedlibrary, quit\n";
    }

    bool dispatch(const std::string& line) {
        const auto arguments = words(line);
        if (arguments.empty()) return true;
        const auto& command = arguments.front();
        try {
            if (is_alias(command, {"help", "h"})) print_help();
            else if (is_alias(command, {"continue", "c"})) continue_execution();
            else if (is_alias(command, {"stepi", "si"})) instruction_step();
            else if (is_alias(command, {"step", "s", "next", "n"})) source_step();
            else if (command == "finish") step_out();
            else if (is_alias(command, {"break", "b"})) {
                if (arguments.size() != 2U) std::cout << "usage: break LOCATION\n";
                else set_breakpoint(arguments[1]);
            } else if (command == "delete") {
                if (arguments.size() != 2U || !parse_address(arguments[1])) std::cout << "usage: delete ID\n";
                else remove_breakpoint(static_cast<int>(*parse_address(arguments[1])));
            } else if (command == "enable" || command == "disable") {
                if (arguments.size() != 2U || !parse_address(arguments[1])) std::cout << "usage: " << command << " ID\n";
                else {
                    const int id = static_cast<int>(*parse_address(arguments[1]));
                    bool found = false;
                    for (auto& [address, breakpoint] : breakpoints_) {
                        if (breakpoint.id == id) {
                            command == "enable" ? enable(breakpoint) : disable(breakpoint);
                            found = true;
                        }
                    }
                    if (!found) std::cout << "unknown breakpoint\n";
                }
            } else if (command == "watch") set_watchpoint(arguments);
            else if (is_alias(command, {"backtrace", "bt"})) { selected_frame_ = 0; print_backtrace(); }
            else if (command == "frame") {
                if (arguments.size() != 2U || !parse_address(arguments[1])) std::cout << "usage: frame NUMBER\n";
                else { selected_frame_ = static_cast<std::size_t>(*parse_address(arguments[1])); print_backtrace(); }
            } else if (command == "print") {
                if (arguments.size() != 2U) std::cout << "usage: print NAME\n";
                else print_locals(arguments[1]);
            } else if (command == "x") print_memory(arguments);
            else if (command == "set") write_memory(arguments);
            else if (command == "register") {
                if (arguments.size() == 3U && arguments[1] == "read") {
                    const auto value = architecture_->register_value(selected_tid_, arguments[2]);
                    std::cout << (value ? hex(*value) : "unknown register") << '\n';
                } else if (arguments.size() == 4U && arguments[1] == "write") {
                    const auto value = parse_address(arguments[3]);
                    if (!value || !architecture_->set_register_value(selected_tid_, arguments[2], *value)) {
                        std::cout << "invalid register or value\n";
                    }
                } else std::cout << "usage: register read NAME | register write NAME VALUE\n";
            } else if (command == "thread") {
                if (arguments.size() != 2U || !parse_address(arguments[1])) std::cout << "usage: thread TID\n";
                else {
                    const auto tid = static_cast<pid_t>(*parse_address(arguments[1]));
                    if (!threads_.contains(tid) || !threads_.at(tid).stopped) std::cout << "unknown or running thread\n";
                    else { selected_tid_ = tid; selected_frame_ = 0; print_stop(tid); }
                }
            } else if (command == "info" && arguments.size() >= 2U) {
                if (arguments[1] == "break") print_breakpoints();
                else if (arguments[1] == "threads") print_threads();
                else if (arguments[1] == "sharedlibrary") print_modules();
                else if (arguments[1] == "registers") print_registers();
                else if (arguments[1] == "locals") print_locals();
                else std::cout << "unknown info topic\n";
            } else if (is_alias(command, {"quit", "q"})) return false;
            else std::cout << "unknown command; try help\n";
        } catch (const std::exception& error) {
            std::cout << "error: " << error.what() << '\n';
        }
        return true;
    }

    void command_loop() {
        while (true) {
            std::string line;
            if (batch_) {
                if (!std::getline(std::cin, line)) break;
            } else {
                char* raw = readline("cosy> ");
                if (raw == nullptr) break;
                line = raw;
                if (!line.empty()) add_history(raw);
                free(raw);
            }
            if (!dispatch(line)) break;
        }
    }

    void print_banner() const {
        std::cout << "Cosy " << architecture_->name() << " debugger; target pid " << child_ << '\n';
    }

    void detach() noexcept {
        if (detached_ || child_ <= 0) return;
        for (auto& [address, breakpoint] : breakpoints_) {
            try { disable(breakpoint); } catch (const std::exception&) {}
        }
        for (const auto& [tid, state] : threads_) {
            if (state.stopped) {
                static_cast<void>(ptrace(PTRACE_DETACH, tid, nullptr, nullptr));
            }
        }
        interrupted_pid = 0;
        detached_ = true;
    }

    std::vector<std::string> target_argv_;
    bool batch_ = false;
    pid_t child_ = -1;
    pid_t selected_tid_ = -1;
    bool alive_ = true;
    bool detached_ = false;
    std::unique_ptr<Architecture> architecture_;
    ModuleManager modules_;
    std::map<pid_t, ThreadState> threads_;
    std::map<Address, Breakpoint> breakpoints_;
    std::vector<Breakpoint> pending_breakpoints_;
    std::vector<Watchpoint> watchpoints_;
    int next_breakpoint_id_ = 1;
    int next_watchpoint_id_ = 1;
    std::size_t selected_frame_ = 0;
};

Debugger::Debugger(std::vector<std::string> target_argv, bool batch)
    : impl_(std::make_unique<Impl>(std::move(target_argv), batch)) {}
Debugger::~Debugger() = default;
int Debugger::run() { return impl_->run(); }

}  // namespace cosy
