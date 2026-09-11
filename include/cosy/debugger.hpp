#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cosy {

class Debugger {
public:
    Debugger(std::vector<std::string> target_argv, bool batch);
    ~Debugger();
    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;

    int run();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cosy
