#pragma once
#ifndef OPCSERIALBRIDGE_LOGGER_HPP
#define OPCSERIALBRIDGE_LOGGER_HPP

#ifdef SPDLOG_ACTIVE_LEVEL
#  undef SPDLOG_ACTIVE_LEVEL
#endif
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG



#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <iostream>

namespace Logger {
    bool init() {
        try {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

            console_sink->set_color(spdlog::level::trace,    "\033[37m");
            console_sink->set_color(spdlog::level::debug,    "\033[36m");
            console_sink->set_color(spdlog::level::info,     "\033[37m");
            console_sink->set_color(spdlog::level::warn,     "\033[33m");
            console_sink->set_color(spdlog::level::err,      "\033[31m");
            console_sink->set_color(spdlog::level::critical, "\033[1;31m");

            auto logger = std::make_shared<spdlog::logger>("global_logger", console_sink);

            logger->set_pattern("\033[32m[%Y-%m-%d %H:%M:%S.%e]\033[m%^%v%$");

            logger->set_level(spdlog::level::trace);
            spdlog::set_default_logger(logger);
            return true;
        }
        catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Log initialization failed: " << ex.what() << std::endl;
            return false;
        }
    };
}
#endif //OPCSERIALBRIDGE_LOGGER_HPP
