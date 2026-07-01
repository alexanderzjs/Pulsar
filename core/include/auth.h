#pragma once

#include <string>
#include <unordered_map>

namespace pulsar::core {

struct AuthChallenge {
    std::string scheme;
    std::unordered_map<std::string, std::string> fields;
};

struct AuthToken {
    std::string scheme;
    std::unordered_map<std::string, std::string> data;
};

class IAuthProvider {
public:
    virtual ~IAuthProvider() = default;
    virtual AuthChallenge challenge() const = 0;
    virtual bool authenticate(const AuthToken& token) = 0;
};

} // namespace pulsar::core
