#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace iouring_runtime::core {

template<typename T>
concept ProtobufMessage = requires(T t, const void* data, void* out, int size) {
    { t.ParseFromArray(data, size) } -> std::same_as<bool>;
    { t.SerializeToArray(out, size) } -> std::same_as<bool>;
    { t.ByteSizeLong() } -> std::convertible_to<std::size_t>;
};

template<typename T>
concept Callable = std::invocable<T>;

} // namespace iouring_runtime::core
