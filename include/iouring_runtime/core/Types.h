#pragma once

#include <cstdint>
#include <string>

namespace iouring_runtime::core {

using SessionId = std::uint64_t;
using ContextId = std::uint16_t;

struct Address {
    std::string host = "0.0.0.0";
    std::uint16_t port = 0;
};

} // namespace iouring_runtime::core
