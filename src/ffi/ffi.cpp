#include "zzuassistant/ffi.h"

#include "zzuassistant/client.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using boost::property_tree::ptree;
namespace zzu = zzu_assistant;

struct zzu_app_client {
    explicit zzu_app_client(zzu::app::ClientOptions options)
        : value(std::move(options)) {}
    zzu::app::AppClient value;
};

struct zzu_sso_client {
    explicit zzu_sso_client(zzu::NetworkOptions options)
        : value(std::move(options)) {}
    zzu::sso::SsoClient value;
};

struct zzu_portal_client {
    explicit zzu_portal_client(zzu::NetworkOptions options)
        : value(std::move(options)) {}
    zzu::portal::PortalClient value;
};

struct zzu_ecard_client {
    explicit zzu_ecard_client(zzu::NetworkOptions options)
        : value(std::move(options)) {}
    zzu::ecard::EcardClient value;
};

struct zzu_userinfo_client {
    zzu::userinfo::UserInfoClient value;
};

namespace {
    thread_local std::string last_error;

    std::string text(const char *value) {
        return value == nullptr ? std::string{} : std::string(value);
    }

    std::string_view view(const char *value) {
        return value == nullptr ? std::string_view{} : std::string_view(value);
    }

    void require(const void *value, const std::string_view name) {
        if (value == nullptr)
            throw std::invalid_argument(std::string(name) + " is null");
    }

    char *copy_string(const std::string_view value) {
        auto *result = static_cast<char *>(std::malloc(value.size() + 1));
        if (result == nullptr) throw std::bad_alloc();
        std::memcpy(result, value.data(), value.size());
        result[value.size()] = '\0';
        return result;
    }

    template<typename Function>
    char *json_call(Function &&function) noexcept {
        try {
            last_error.clear();
            return copy_string(std::forward<Function>(function)());
        } catch (const std::exception &error) {
            last_error = error.what();
        } catch (...) {
            last_error = "Unknown C++ exception";
        }
        return nullptr;
    }

    template<typename Function>
    int status_call(Function &&function) noexcept {
        try {
            last_error.clear();
            std::forward<Function>(function)();
            return 0;
        } catch (const std::exception &error) {
            last_error = error.what();
        } catch (...) {
            last_error = "Unknown C++ exception";
        }
        return -1;
    }

    std::string quote(const std::string_view value) {
        std::string result;
        result.reserve(value.size() + 2);
        result.push_back('"');
        constexpr char hex[] = "0123456789abcdef";
        for (const unsigned char ch: value) {
            switch (ch) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b"; break;
                case '\f': result += "\\f"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:
                    if (ch < 0x20) {
                        result += "\\u00";
                        result.push_back(hex[ch >> 4]);
                        result.push_back(hex[ch & 0x0f]);
                    } else {
                        result.push_back(static_cast<char>(ch));
                    }
            }
        }
        result.push_back('"');
        return result;
    }

    ptree parse_json(const char *source) {
        require(source, "JSON");
        std::istringstream input(source);
        ptree tree;
        boost::property_tree::read_json(input, tree);
        return tree;
    }

    std::string member(const std::string_view name,
                       const std::string_view value) {
        return quote(name) + ':' + quote(value);
    }

    std::string path_json(const zzu::ecard::LocationPath &path) {
        return '{' + member("big_area", path.big_area) + ',' +
               member("area", path.area) + ',' +
               member("building", path.building) + ',' +
               member("unit", path.unit) + ',' +
               member("level", path.level) + ',' +
               member("room", path.room) + ',' +
               member("sub_area", path.sub_area) + '}';
    }

    zzu::ecard::LocationPath path_from(const ptree &tree) {
        return {tree.get("big_area", ""), tree.get("area", ""),
                tree.get("building", ""), tree.get("unit", ""),
                tree.get("level", ""), tree.get("room", ""),
                tree.get("sub_area", "")};
    }

    std::string profiles_json(const zzu::ecard::ElectricityProfiles &profiles) {
        return "{" + quote("lighting") + ':' + path_json(profiles.lighting) +
               ',' + quote("air_conditioning") + ':' +
               path_json(profiles.air_conditioning) + '}';
    }

    zzu::ecard::ElectricityProfiles profiles_from(const ptree &tree) {
        zzu::ecard::ElectricityProfiles result;
        if (const auto node = tree.get_child_optional("lighting"))
            result.lighting = path_from(*node);
        if (const auto node = tree.get_child_optional("air_conditioning"))
            result.air_conditioning = path_from(*node);
        return result;
    }

    std::string app_result(const zzu::app::LoginResult &result) {
        return "{" + quote("success") + ':' +
               (result.success ? "true" : "false") + ',' +
               member("message", result.message) + '}';
    }

    std::string sso_status(const zzu::sso::LoginStatus status) {
        using enum zzu::sso::LoginStatus;
        switch (status) {
            case success: return "success";
            case no_session: return "no_session";
            case rejected: return "rejected";
            case captcha_required: return "captcha_required";
            case mfa_rejected: return "mfa_rejected";
            case mfa_expired: return "mfa_expired";
            case protocol_error: return "protocol_error";
            case network_error: return "network_error";
        }
        return "protocol_error";
    }

    std::string sso_result(const zzu::sso::LoginResult &result) {
        return "{" + member("status", sso_status(result.status)) + ',' +
               quote("success") + ':' +
               (result.succeeded() ? "true" : "false") + ',' +
               member("message", result.message) + ',' +
               member("final_url", result.final_url) + '}';
    }

    std::string portal_result(const zzu::portal::AuthResult &result) {
        std::string json = "{" + quote("success") + ':' +
                           (result.success ? "true" : "false") + ',' +
                           quote("result") + ':' +
                           std::to_string(result.result) + ',' +
                           quote("return_code") + ':';
        json += result.return_code ? std::to_string(*result.return_code) : "null";
        return json + ',' + member("message", result.message) + '}';
    }
}

extern "C" {
    const char *zzu_ffi_version(void) { return "0.1.0"; }
    const char *zzu_last_error(void) { return last_error.c_str(); }
    void zzu_string_free(char *value) { std::free(value); }

    zzu_app_client *zzu_app_create(const char *ca_file, const char *device_id) {
        try {
            last_error.clear();
            return new zzu_app_client({{text(ca_file)}, text(device_id)});
        } catch (const std::exception &error) { last_error = error.what(); }
        catch (...) { last_error = "Unknown C++ exception"; }
        return nullptr;
    }
    void zzu_app_destroy(zzu_app_client *client) { delete client; }
    char *zzu_app_login(zzu_app_client *client, const char *username,
                        const char *password, zzu_notify_callback notify,
                        zzu_prompt_callback prompt, void *user_data) {
        return json_call([&] {
            require(client, "App client");
            return app_result(client->value.login({
                .username = view(username), .password = view(password),
                .notify = notify ? [=](const std::string_view message) {
                    const std::string copy(message); notify(copy.c_str(), user_data);
                } : std::function<void(std::string_view)>{},
                .prompt = prompt ? [=](const std::string_view message) {
                    const std::string copy(message);
                    return text(prompt(copy.c_str(), user_data));
                } : std::function<std::string(std::string_view)>{},
            }));
        });
    }
    int zzu_app_restore(zzu_app_client *client, const char *session_json) {
        return status_call([&] {
            require(client, "App client");
            const ptree tree = parse_json(session_json);
            const auto result = client->value.login({
                tree.get("username", ""), tree.get("device_id", ""),
                tree.get("id_token", ""), tree.get("refresh_token", "")});
            if (!result.success) throw std::invalid_argument(result.message);
        });
    }
    char *zzu_app_logout(zzu_app_client *client) {
        return json_call([&] { require(client, "App client");
            return app_result(client->value.logout()); });
    }
    char *zzu_app_id_token(zzu_app_client *client) {
        return json_call([&] { require(client, "App client");
            return quote(client->value.id_token()); });
    }
    char *zzu_app_card_balance(zzu_app_client *client) {
        return json_call([&] { require(client, "App client");
            return std::format("{{\"balance_yuan\":{}}}",
                               client->value.card_balance()); });
    }
    char *zzu_app_session(zzu_app_client *client) {
        return json_call([&] {
            require(client, "App client");
            const auto value = client->value.session();
            return "{" + member("username", value.username) + ',' +
                   member("device_id", value.device_id) + ',' +
                   member("id_token", value.id_token) + ',' +
                   member("refresh_token", value.refresh_token) + '}';
        });
    }

    zzu_sso_client *zzu_sso_create(const char *ca_file) {
        try { last_error.clear(); return new zzu_sso_client({text(ca_file)}); }
        catch (const std::exception &error) { last_error = error.what(); }
        catch (...) { last_error = "Unknown C++ exception"; }
        return nullptr;
    }
    void zzu_sso_destroy(zzu_sso_client *client) { delete client; }
    char *zzu_sso_probe(zzu_sso_client *client) {
        return json_call([&] { require(client, "SSO client");
            return sso_result(client->value.probe()); });
    }
    char *zzu_sso_login(zzu_sso_client *client, const char *username,
                        const char *password, const char *service_url,
                        const int mfa_method, const unsigned timeout,
                        zzu_notify_callback notify, zzu_prompt_callback prompt,
                        zzu_qr_callback display_qr, void *user_data) {
        return json_call([&] {
            require(client, "SSO client");
            if (mfa_method != 0 && mfa_method != 1)
                throw std::invalid_argument("mfa_method must be 0 or 1");
            zzu::sso::LoginOptions options{
                .username = view(username), .password = view(password),
                .service_url = view(service_url),
                .mfa_method = mfa_method == 0
                    ? zzu::sso::MfaMethod::secure_phone
                    : zzu::sso::MfaMethod::qr_code,
                .mfa_timeout = std::chrono::seconds(timeout == 0 ? 120 : timeout),
            };
            if (notify) options.notify = [=](const std::string_view message) {
                const std::string copy(message); notify(copy.c_str(), user_data);
            };
            if (prompt) options.prompt = [=](const std::string_view message) {
                const std::string copy(message);
                return text(prompt(copy.c_str(), user_data));
            };
            if (display_qr) options.display_qr = [=](const std::vector<unsigned char> &png) {
                display_qr(png.data(), png.size(), user_data);
            };
            return sso_result(client->value.login(options));
        });
    }
    int zzu_sso_restore(zzu_sso_client *client, const char *session_json) {
        return status_call([&] {
            require(client, "SSO client"); const ptree tree = parse_json(session_json);
            const auto result = client->value.login({
                tree.get("username", ""), tree.get("final_url", ""),
                tree.get<std::int64_t>("updated_at", 0), tree.get("cookies", ""),
                tree.get("cas_cookie", "")});
            if (!result.succeeded()) throw std::invalid_argument(result.message);
        });
    }
    char *zzu_sso_resume(zzu_sso_client *client, const char *service_url) {
        return json_call([&] { require(client, "SSO client");
            return sso_result(client->value.resume(view(service_url))); });
    }
    char *zzu_sso_logout(zzu_sso_client *client) {
        return json_call([&] { require(client, "SSO client");
            return sso_result(client->value.logout()); });
    }
    char *zzu_sso_session(zzu_sso_client *client) {
        return json_call([&] {
            require(client, "SSO client"); const auto value = client->value.session();
            return "{" + member("username", value.username) + ',' +
                   member("final_url", value.final_url) + ',' +
                   quote("updated_at") + ':' + std::to_string(value.updated_at) + ',' +
                   member("cookies", value.cookies) + ',' +
                   member("cas_cookie", value.cas_cookie) + '}';
        });
    }

    zzu_portal_client *zzu_portal_create(const char *ca_file) {
        try { last_error.clear(); return new zzu_portal_client({text(ca_file)}); }
        catch (const std::exception &error) { last_error = error.what(); }
        catch (...) { last_error = "Unknown C++ exception"; }
        return nullptr;
    }
    void zzu_portal_destroy(zzu_portal_client *client) { delete client; }
    char *zzu_portal_discover(zzu_portal_client *client) {
        return json_call([&] { require(client, "Portal client");
            const auto value = client->value.discover();
            return "{" + member("auth_url", value.auth_url) + ',' +
                   member("server_url", value.server_url) + ',' +
                   member("user_ip", value.user_ip) + '}'; });
    }
    char *zzu_portal_login(zzu_portal_client *client, const char *server_url,
                           const char *user_ip, const char *username,
                           const char *password, const char *isp_suffix,
                           const int encrypt_parameters) {
        return json_call([&] { require(client, "Portal client");
            return portal_result(client->value.login({
                view(server_url), view(user_ip), view(username), view(password),
                view(isp_suffix), encrypt_parameters != 0})); });
    }
    int zzu_portal_restore(zzu_portal_client *client, const char *session_json) {
        return status_call([&] { require(client, "Portal client");
            const ptree tree = parse_json(session_json);
            client->value.login(zzu::portal::Session{
                tree.get("username", ""), tree.get("server_url", ""),
                tree.get("user_ip", ""), tree.get("isp_suffix", "")}); });
    }
    char *zzu_portal_logout(zzu_portal_client *client) {
        return json_call([&] { require(client, "Portal client");
            return portal_result(client->value.logout()); });
    }
    char *zzu_portal_session(zzu_portal_client *client) {
        return json_call([&] { require(client, "Portal client");
            const auto value = client->value.session();
            if (!value) return std::string("null");
            return "{" + member("username", value->username) + ',' +
                   member("server_url", value->server_url) + ',' +
                   member("user_ip", value->user_ip) + ',' +
                   member("isp_suffix", value->isp_suffix) + '}'; });
    }
    char *zzu_portal_local_ipv4(void) {
        return json_call([] { return quote(zzu::portal::PortalClient::local_ipv4()); });
    }

    zzu_ecard_client *zzu_ecard_create(const char *ca_file) {
        try { last_error.clear(); return new zzu_ecard_client({text(ca_file)}); }
        catch (const std::exception &error) { last_error = error.what(); }
        catch (...) { last_error = "Unknown C++ exception"; }
        return nullptr;
    }
    void zzu_ecard_destroy(zzu_ecard_client *client) { delete client; }
    int zzu_ecard_login(zzu_ecard_client *client, const char *username,
                        const char *super_app_id_token) {
        return status_call([&] { require(client, "eCard client");
            client->value.login(view(username), view(super_app_id_token)); });
    }
    int zzu_ecard_restore(zzu_ecard_client *client, const char *session_json) {
        return status_call([&] { require(client, "eCard client");
            const ptree tree = parse_json(session_json);
            const auto profiles = tree.get_child_optional("profiles");
            client->value.login(zzu::ecard::Session{
                tree.get("username", ""), tree.get("access_token", ""),
                tree.get("refresh_token", ""),
                tree.get("access_token_expire", ""),
                profiles ? profiles_from(*profiles)
                         : zzu::ecard::ElectricityProfiles{}}); });
    }
    char *zzu_ecard_session(zzu_ecard_client *client) {
        return json_call([&] { require(client, "eCard client");
            const auto value = client->value.session();
            return "{" + member("username", value.username) + ',' +
                   member("access_token", value.access_token) + ',' +
                   member("refresh_token", value.refresh_token) + ',' +
                   member("access_token_expire", value.access_token_expire) + ',' +
                   quote("profiles") + ':' + profiles_json(value.profiles) + '}'; });
    }
    char *zzu_ecard_locations(zzu_ecard_client *client,
                              const char *location_type, const char *path) {
        return json_call([&] { require(client, "eCard client");
            const auto value = client->value.locations(view(location_type),
                                                        path_from(parse_json(path)));
            std::string json = "{" + member("location_type", value.location_type) +
                ',' + member("next_location_type", value.next_location_type) + ',' +
                quote("end") + ':' + (value.end ? "true" : "false") + ',' +
                quote("options") + ":[";
            for (std::size_t i = 0; i < value.options.size(); ++i) {
                if (i) json += ',';
                json += "{" + member("id", value.options[i].id) + ',' +
                        member("name", value.options[i].name) + '}';
            }
            return json + "]}";
        });
    }
    char *zzu_ecard_account(zzu_ecard_client *client, const char *path) {
        return json_call([&] { require(client, "eCard client");
            const auto value = client->value.account(path_from(parse_json(path)));
            return std::format("{{\"quantity_kwh\":{},\"price_yuan_per_kwh\":{}}}",
                               value.quantity_kwh, value.price_yuan_per_kwh); });
    }
    char *zzu_ecard_recharge(zzu_ecard_client *client, const char *path,
                             const char *password, const unsigned amount) {
        return json_call([&] { require(client, "eCard client");
            const auto value = client->value.recharge(path_from(parse_json(path)),
                                                       view(password), amount);
            return "{" + quote("success") + ':' +
                   (value.success ? "true" : "false") + ',' +
                   member("message", value.message) + '}'; });
    }
    char *zzu_ecard_recharge_config(zzu_ecard_client *client) {
        return json_call([&] { require(client, "eCard client");
            const auto value = client->value.campus_card_recharge_config();
            std::string json = std::format("{{\"balance_yuan\":{},\"amounts_yuan\":[",
                                           value.balance_yuan);
            for (std::size_t i = 0; i < value.amounts_yuan.size(); ++i) {
                if (i) json += ','; json += std::to_string(value.amounts_yuan[i]);
            }
            return json + "]}"; });
    }
    char *zzu_ecard_create_recharge_order(zzu_ecard_client *client,
                                           const unsigned amount) {
        return json_call([&] { require(client, "eCard client");
            const auto value = client->value.campus_card_recharge(amount);
            return "{" + quote("amount_yuan") + ':' +
                   std::to_string(value.amount_yuan) + ',' +
                   member("checkout_url", value.checkout_url) + '}'; });
    }
    char *zzu_ecard_profiles(zzu_ecard_client *client) {
        return json_call([&] { require(client, "eCard client");
            zzu::ecard::ElectricityProfiles value;
            if (!client->value.load_profiles(value)) return std::string("null");
            return profiles_json(value); });
    }
    int zzu_ecard_set_profiles(zzu_ecard_client *client,
                               const char *profiles) {
        return status_call([&] { require(client, "eCard client");
            client->value.save_profiles(profiles_from(parse_json(profiles))); });
    }

    zzu_userinfo_client *zzu_userinfo_create(void) {
        try { last_error.clear(); return new zzu_userinfo_client; }
        catch (const std::exception &error) { last_error = error.what(); }
        catch (...) { last_error = "Unknown C++ exception"; }
        return nullptr;
    }
    void zzu_userinfo_destroy(zzu_userinfo_client *client) { delete client; }
    char *zzu_userinfo_parse(zzu_userinfo_client *client, const char *final_url,
                             const char *expected_username) {
        return json_call([&] { require(client, "UserInfo client");
            const auto value = client->value.parse_sso_redirect(
                view(final_url), view(expected_username));
            return "{" + member("username", value.username) + ',' +
                   member("name", value.name) + ',' +
                   member("identity_type_code", value.identity_type_code) + ',' +
                   member("identity_type_name", value.identity_type_name) + ',' +
                   member("organization_code", value.organization_code) + ',' +
                   member("organization_name", value.organization_name) + ',' +
                   member("account_id", value.account_id) + ',' +
                   member("user_id", value.user_id) + ',' +
                   member("uid", value.uid) + ',' + quote("expires_at") + ':' +
                   std::to_string(value.expires_at) + '}'; });
    }
}
