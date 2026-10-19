#include "../include/App.hpp"

#include <spdlog/spdlog.h>
#include <cstring>
#include <iostream>

namespace {

struct Args {
    std::string config{"config/bridge.yaml"};
    bool verbose{false};
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        if ((!std::strcmp(argv[i], "--config") || !std::strcmp(argv[i], "-c")) && i + 1 < argc) {
            args.config = argv[++i];
        } else if (!std::strcmp(argv[i], "-v") || !std::strcmp(argv[i], "--verbose")) {
            args.verbose = true;
        } else {
            std::cerr << "usage: " << argv[0] << " [--config path.yaml] [-v]\n";
            std::exit(2);
        }
    }
    return args;
}

}  // namespace

int main(int argc, char** argv) {
    //const auto args = parse_args(argc, argv);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    //spdlog::set_level(args.verbose ? spdlog::level::debug : spdlog::level::info);
    spdlog::set_level(true ? spdlog::level::debug : spdlog::level::info);
    try {
        //App app(load_config(args.config));
        App app(load_config("/home/semen/opcua_serial_bridge/config/bridge_config.yaml"));
        return app.run();
    } catch (const ConfigError& e) {
        spdlog::critical("Config: {}", e.what());
        return 2;
    } catch (const std::exception& e) {
        spdlog::critical("Startup: {}", e.what());
        return 1;
    }
}
