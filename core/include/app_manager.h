#pragma once
#include <string>
#include <vector>

namespace pulsar::core {

struct AppEntry {
    std::string id;
    std::string name;
    std::string executable;
    std::string args;
    std::string working_dir;
    std::string cover_image;
};

class IAppManager {
public:
    virtual ~IAppManager() = default;
    virtual std::vector<AppEntry> list_apps() const = 0;
    virtual bool launch(const std::string& app_id) = 0;
    virtual bool terminate(const std::string& app_id) = 0;
    virtual bool is_running(const std::string& app_id) const = 0;
};

} // namespace pulsar::core
