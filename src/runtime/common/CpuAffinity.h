#pragma once

#include <vector>

namespace iouring_runtime::detail {

std::vector<int> OrderedOnlineCpus();
std::vector<int> OrderedPhysicalFirstCpus();

} // namespace iouring_runtime::detail
