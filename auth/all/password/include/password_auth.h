#pragma once

#include "auth.h"
#include <string>

namespace pulsar::auth::password {

// Simple username/password authenticator.
// Credentials are supplied at construction time (from config.json).
class PasswordAuth final : public pulsar::core::IAuthProvider {
public:
    PasswordAuth(std::string username, std::string password);

    pulsar::core::AuthChallenge challenge() const override;
    bool authenticate(const pulsar::core::AuthToken& token) override;

private:
    std::string username_;
    std::string password_;
};

} // namespace pulsar::auth::password
