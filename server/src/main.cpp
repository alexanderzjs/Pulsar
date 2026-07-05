#include "config_parser.h"
#include "factory.h"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string config_path;
    bool verify = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--verify") {
            verify = true;
        } else {
            config_path = arg;
        }
    }

    pulsar::server::ServerConfig cfg;
    if (!config_path.empty()) {
        std::string err;
        auto loaded = pulsar::server::load_config(config_path, err);
        if (!loaded) {
            std::cerr << "[error] config: " << err << '\n';
            return 1;
        }
        cfg = *loaded;
    } else {
        // Prefer workspace config.json when present so protocol settings
        // (for example enabling RDP/VNC) apply without extra CLI args.
        std::string err;
        auto loaded = pulsar::server::load_config("config.json", err);
        if (loaded) {
            cfg = *loaded;
        } else {
            cfg = pulsar::server::default_config();
            std::cerr << "[warn] config: " << err
                      << " ; falling back to built-in defaults\n";
        }
    }

    if (verify) {
        return pulsar::server::run_verify(cfg);
    }
    return pulsar::server::run_server(cfg);
}
