#include "CpuAffinity.h"

#include <cstddef>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include <unistd.h>

namespace iouring_runtime::detail {

namespace {

std::vector<int> ExpandCpuToken(const std::string& token) {
    const auto dash = token.find('-');
    if (dash == std::string::npos) {
        return {std::stoi(token)};
    }

    const int first = std::stoi(token.substr(0, dash));
    const int last = std::stoi(token.substr(dash + 1));
    std::vector<int> cpus;
    cpus.reserve(static_cast<std::size_t>(last - first + 1));
    for (int cpu = first; cpu <= last; ++cpu) {
        cpus.push_back(cpu);
    }
    return cpus;
}

std::vector<int> ParseCpuList(const std::string& text) {
    std::vector<int> cpus;
    std::stringstream ss(text);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        auto expanded = ExpandCpuToken(token);
        cpus.insert(cpus.end(), expanded.begin(), expanded.end());
    }
    return cpus;
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace

std::vector<int> OrderedOnlineCpus() {
    auto cpus = ParseCpuList(ReadTextFile("/sys/devices/system/cpu/online"));
    if (!cpus.empty()) {
        return cpus;
    }

    const long cpu_count = ::sysconf(_SC_NPROCESSORS_ONLN);
    cpus.reserve(cpu_count > 0 ? static_cast<std::size_t>(cpu_count) : 0U);
    for (int cpu = 0; cpu < cpu_count; ++cpu) {
        cpus.push_back(cpu);
    }
    return cpus;
}

std::vector<int> OrderedPhysicalFirstCpus() {
    std::vector<int> ordered;
    std::set<int> seen;
    for (int cpu : OrderedOnlineCpus()) {
        const std::string path =
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
            "/topology/thread_siblings_list";
        auto siblings = ParseCpuList(ReadTextFile(path));
        if (siblings.empty()) {
            siblings.push_back(cpu);
        }
        const int primary = siblings.front();
        if (seen.insert(primary).second) {
            ordered.push_back(primary);
        }
    }

    for (int cpu : OrderedOnlineCpus()) {
        if (seen.insert(cpu).second) {
            ordered.push_back(cpu);
        }
    }
    return ordered;
}

} // namespace iouring_runtime::detail
