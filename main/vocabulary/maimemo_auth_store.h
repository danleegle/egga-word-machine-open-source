#ifndef MAIMEMO_AUTH_STORE_H
#define MAIMEMO_AUTH_STORE_H

#include <string>

struct MaimemoAuthConfig {
    bool configured = false;
    bool credential_saved = false;
    std::string token;
    std::string open_api_token;
    std::string account_masked;
    std::string account;
    std::string password;
    std::string obtained_at;
    bool open_api_configured = false;
};

class MaimemoAuthStore {
public:
    static MaimemoAuthStore& GetInstance();

    MaimemoAuthConfig Load() const;
    bool Save(const MaimemoAuthConfig& config);
    bool Clear();

private:
    MaimemoAuthStore() = default;
};

#endif  // MAIMEMO_AUTH_STORE_H
