#include "util/log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>

namespace chat {
namespace {

constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v";

void install(const std::shared_ptr<spdlog::logger>& logger, spdlog::level::level_enum level) {
    logger->set_pattern(kPattern);
    logger->set_level(level);
    logger->flush_on(spdlog::level::trace);
    spdlog::set_default_logger(logger);
}

} // namespace

void logToConsole(const std::string& program) {
    install(spdlog::stdout_color_mt(program), spdlog::level::info);
}

bool logToFile(const std::string& program, const std::string& path, std::string& error) {
    if (path.empty()) {
        install(spdlog::null_logger_mt(program), spdlog::level::off);
        return true;
    }
    try {
        install(spdlog::basic_logger_mt(program, path), spdlog::level::debug);
        return true;
    }
    catch (const spdlog::spdlog_ex& failure) {
        error = failure.what();
        install(spdlog::null_logger_mt(program), spdlog::level::off);
        return false;
    }
}

} // namespace chat
