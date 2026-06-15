#pragma once

#include <vector>

namespace iouring::detail {

std::vector<int> OrderedOnlineCpus();
std::vector<int> OrderedPhysicalFirstCpus();

} // namespace iouring::detail
