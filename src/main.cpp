#include "cosy/debugger.hpp"

#include <iostream>
#include <stdexcept>

int main(int argc, char* argv[]) {
    bool batch = false;
    int index = 1;
    if (index < argc && std::string_view(argv[index]) == "--batch") {
        batch = true;
        ++index;
    }
    if (index < argc && std::string_view(argv[index]) == "--") {
        ++index;
    }
    if (index >= argc) {
        std::cerr << "usage: cosy [--batch] -- PROGRAM [ARG ...]\n";
        return 2;
    }
    std::vector<std::string> target;
    for (; index < argc; ++index) {
        target.emplace_back(argv[index]);
    }
    try {
        cosy::Debugger debugger(std::move(target), batch);
        return debugger.run();
    } catch (const std::exception& error) {
        std::cerr << "cosy: " << error.what() << '\n';
        return 1;
    }
}
