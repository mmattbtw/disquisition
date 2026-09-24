#pragma once

#include <spdlog/spdlog.h>

#include <string>

#define LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERR(...) spdlog::error(__VA_ARGS__)

namespace chat {

void logToConsole(const std::string& program);
bool logToFile(const std::string& program, const std::string& path, std::string& error);

} // namespace chat
