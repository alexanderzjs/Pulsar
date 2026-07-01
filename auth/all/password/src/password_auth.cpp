#include "password_auth.h"

namespace pulsar::auth::password {

PasswordAuth::PasswordAuth(std::string username, std::string password)
    : username_(std::move(username)), password_(std::move(password))
{
}

pulsar::core::AuthChallenge PasswordAuth::challenge() const {
    return {
        "password",
        { {"username", "Username"}, {"password", "Password"} }
    };
}

bool PasswordAuth::authenticate(const pulsar::core::AuthToken& token) {
    if (token.scheme != "password") return false;
    const auto u = token.data.find("username");
    const auto p = token.data.find("password");
    return u != token.data.end() && p != token.data.end()
        && u->second == username_ && p->second == password_;
}

} // namespace pulsar::auth::password
