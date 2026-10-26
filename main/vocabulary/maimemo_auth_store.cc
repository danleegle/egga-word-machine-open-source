#include "maimemo_auth_store.h"

#include "settings.h"

namespace {

constexpr char kNamespace[] = "maimemo";
constexpr char kTokenKey[] = "token";
constexpr char kOpenApiTokenKey[] = "open_token";
constexpr char kAccountMaskedKey[] = "acct_mask";
constexpr char kAccountKey[] = "account";
constexpr char kPasswordKey[] = "password";
constexpr char kObtainedAtKey[] = "obtained_at";
constexpr char kCredentialSavedKey[] = "cred_saved";

}  // namespace

MaimemoAuthStore& MaimemoAuthStore::GetInstance() {
    static MaimemoAuthStore store;
    return store;
}

MaimemoAuthConfig MaimemoAuthStore::Load() const {
    Settings settings(kNamespace);
    MaimemoAuthConfig config;
    config.token = settings.GetString(kTokenKey);
    config.open_api_token = settings.GetString(kOpenApiTokenKey);
    config.account_masked = settings.GetString(kAccountMaskedKey);
    config.obtained_at = settings.GetString(kObtainedAtKey);
    config.credential_saved = settings.GetBool(kCredentialSavedKey, false);
    if (config.credential_saved) {
        config.account = settings.GetString(kAccountKey);
        config.password = settings.GetString(kPasswordKey);
    }
    config.configured = !config.token.empty() ||
        (config.credential_saved && !config.account.empty() && !config.password.empty());
    config.open_api_configured = !config.open_api_token.empty();
    return config;
}

bool MaimemoAuthStore::Save(const MaimemoAuthConfig& config) {
    Settings settings(kNamespace, true);
    settings.SetString(kTokenKey, config.token);
    settings.SetString(kOpenApiTokenKey, config.open_api_token);
    settings.SetString(kAccountMaskedKey, config.account_masked);
    settings.SetString(kObtainedAtKey, config.obtained_at);
    settings.SetBool(kCredentialSavedKey, config.credential_saved);
    if (config.credential_saved) {
        settings.SetString(kAccountKey, config.account);
        settings.SetString(kPasswordKey, config.password);
    } else {
        settings.EraseKey(kAccountKey);
        settings.EraseKey(kPasswordKey);
    }
    return true;
}

bool MaimemoAuthStore::Clear() {
    Settings settings(kNamespace, true);
    settings.EraseAll();
    return true;
}
