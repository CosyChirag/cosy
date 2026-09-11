#pragma once

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cosy {

using Address = std::uint64_t;

class SystemError final : public std::runtime_error {
public:
    explicit SystemError(const std::string& operation)
        : std::runtime_error(operation + ": " + std::strerror(errno)) {}
};

inline void check_syscall(long result, const std::string& operation) {
    if (result == -1) {
        throw SystemError(operation);
    }
}

inline std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline std::vector<std::string> words(const std::string& value) {
    std::vector<std::string> result;
    std::string current;
    bool quoted = false;
    char quote = 0;
    for (const char ch : value) {
        if ((ch == '\'' || ch == '"') && (!quoted || ch == quote)) {
            if (quoted) {
                quoted = false;
            } else {
                quoted = true;
                quote = ch;
            }
        } else if (std::isspace(static_cast<unsigned char>(ch)) && !quoted) {
            if (!current.empty()) {
                result.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        result.push_back(current);
    }
    return result;
}

inline std::optional<Address> parse_address(std::string_view text) {
    Address value = 0;
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

inline std::string hex(Address value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result{"0x"};
    bool began = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const auto nibble = static_cast<unsigned>((value >> static_cast<unsigned>(shift)) & 0xfU);
        if (nibble != 0U || began || shift == 0) {
            result += digits[nibble];
            began = true;
        }
    }
    return result;
}

}  // namespace cosy
