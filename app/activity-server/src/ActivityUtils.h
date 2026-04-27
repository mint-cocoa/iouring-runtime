#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace activity_server {

std::string EnvString(const char* name, std::string fallback);
std::uint16_t EnvPort(const char* name, std::uint16_t fallback);

std::string JsonEscape(std::string_view text);
std::string Lower(std::string_view text);
std::string Trim(std::string_view text);
std::string QueryParam(std::string_view query, std::string_view name);
std::optional<std::string> JsonString(std::string_view json, std::string_view key);
std::optional<double> JsonNumber(std::string_view json, std::string_view key);
std::string RandomId(std::string_view prefix);

std::string WebSocketAccept(std::string_view key);
std::string ReadFile(const std::filesystem::path& path);

std::string ShellQuote(std::string_view value);
int RunCommand(const std::string& command);
bool IsHttpUrl(std::string_view url);

struct CommandResult {
    int exit_code = -1;
    std::string output;
};

CommandResult RunCommandCapture(const std::string& command);
CommandResult CurlGet(std::string_view url, std::string_view extra_args = {});

} // namespace activity_server
