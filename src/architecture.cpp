#include "cosy/architecture.hpp"

#include <stdexcept>

namespace cosy {

std::unique_ptr<Architecture> make_x86_64_architecture();
std::unique_ptr<Architecture> make_aarch64_architecture();

std::unique_ptr<Architecture> make_host_architecture() {
#if defined(__x86_64__)
    return make_x86_64_architecture();
#elif defined(__aarch64__)
    return make_aarch64_architecture();
#else
    throw std::runtime_error("Cosy supports only x86-64 and AArch64 Linux hosts");
#endif
}

}  // namespace cosy
