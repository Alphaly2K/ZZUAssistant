#include "cli/session_store.h"

#include "auth/environment.h"
#include "model/app.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace zzu_assistant::cli {
    namespace {
        using boost::property_tree::ptree;

        std::string key(const std::string_view username) {
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned size = 0;
            if (EVP_Digest(username.data(), username.size(), digest.data(), &size,
                           EVP_sha256(), nullptr) != 1)
                throw std::runtime_error("Cannot derive session key");
            std::ostringstream output;
            output << std::hex << std::setfill('0');
            for (unsigned index = 0; index < size; ++index)
                output << std::setw(2) << static_cast<unsigned>(digest[index]);
            return output.str();
        }

        std::filesystem::path file(const std::string_view user,
                                   const std::string_view type) {
            return auth::state_directory() / "sessions" /
                   (key(user) + "." + std::string(type) + ".json");
        }

        std::filesystem::path current_file(const std::string_view scope) {
            return auth::state_directory() / (std::string(scope) + ".current");
        }

        std::optional<ptree> read(const std::filesystem::path &path) {
            std::ifstream input(path);
            if (!input) return std::nullopt;
            ptree tree;
            boost::property_tree::read_json(input, tree);
            return tree;
        }

        void write(const std::filesystem::path &path, const ptree &tree) {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error) throw std::runtime_error("Cannot create session directory");
            const auto temporary = std::filesystem::path(path.string() + ".tmp");
            {
                std::ofstream output(temporary, std::ios::trunc);
                if (!output) throw std::runtime_error("Cannot write session");
                boost::property_tree::write_json(output, tree, false);
            }
            std::filesystem::rename(temporary, path, error);
            if (error) {
                std::filesystem::remove(path, error);
                error.clear();
                std::filesystem::rename(temporary, path, error);
            }
            if (error) throw std::runtime_error("Cannot replace session");
        }

        void put_path(ptree &tree, const ecard::LocationPath &path) {
            tree.put("big_area", path.big_area); tree.put("area", path.area);
            tree.put("building", path.building); tree.put("unit", path.unit);
            tree.put("level", path.level); tree.put("room", path.room);
            tree.put("sub_area", path.sub_area);
        }

        ecard::LocationPath get_path(const ptree &tree) {
            return {tree.get("big_area", ""), tree.get("area", ""),
                    tree.get("building", ""), tree.get("unit", ""),
                    tree.get("level", ""), tree.get("room", ""),
                    tree.get("sub_area", "")};
        }

        void remove_file(const std::filesystem::path &path) {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }

    NetworkOptions SessionStore::network_options() {
        return {.ca_file = auth::environment("SSL_CERT_FILE").value_or("")};
    }

    app::ClientOptions SessionStore::app_options() {
        return {{auth::environment("SSL_CERT_FILE").value_or("")},
                auth::environment(model::app::SUPER_APP_DEVICE_ID_ENVIRONMENT)
                    .value_or("")};
    }

    std::optional<std::string> SessionStore::current_user(
        const std::string_view scope) const {
        std::ifstream input(current_file(scope));
        std::string username;
        return input && std::getline(input, username) && !username.empty()
                   ? std::optional<std::string>(username) : std::nullopt;
    }

    void SessionStore::set_current_user(const std::string_view scope,
                                        const std::string_view username) const {
        const auto path = current_file(scope);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream output(path, std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot save current user");
        output << username << '\n';
    }

    void SessionStore::clear_current_user(const std::string_view scope,
                                          const std::string_view username) const {
        if (current_user(scope) == std::optional<std::string>(username))
            remove_file(current_file(scope));
    }

    std::optional<app::Session> SessionStore::load_app(const std::string_view user) const {
        if (const auto token = auth::environment(auth::APP_TOKEN_ENV)) {
            auth::validate_jwt(*token, auth::APP_TOKEN_ENV);
            return app::Session{std::string(user),
                                auth::environment(model::app::SUPER_APP_DEVICE_ID_ENVIRONMENT)
                                    .value_or(""), *token, {}};
        }
        const auto tree = read(file(user, "app")); if (!tree) return std::nullopt;
        return app::Session{
            std::string(user),
            auth::environment(model::app::SUPER_APP_DEVICE_ID_ENVIRONMENT)
                .value_or(tree->get("device_id", "")),
            tree->get("id_token", ""), tree->get("refresh_token", "")};
    }
    void SessionStore::save(const app::Session &s) const {
        if (auth::environment(auth::APP_TOKEN_ENV)) return;
        ptree t; t.put("username", s.username); t.put("device_id", s.device_id);
        t.put("id_token", s.id_token); t.put("refresh_token", s.refresh_token);
        write(file(s.username, "app"), t); set_current_user("app", s.username);
    }
    void SessionStore::remove_app(const std::string_view u) const {
        remove_file(file(u, "app")); clear_current_user("app", u);
    }

    std::optional<sso::Session> SessionStore::load_sso(const std::string_view user) const {
        if (const auto token = auth::environment(auth::SSO_TOKEN_ENV)) {
            auth::validate_header_value(*token, auth::SSO_TOKEN_ENV);
            const std::string cookie = token->find('=') == std::string::npos
                                           ? "TGC=" + *token : *token;
            return sso::Session{std::string(user), {}, 0, {}, cookie};
        }
        const auto t = read(file(user, "sso")); if (!t) return std::nullopt;
        return sso::Session{std::string(user), t->get("final_url", ""),
                            t->get<std::int64_t>("updated_at", 0),
                            t->get("cookies", ""), t->get("cas_cookie", "")};
    }
    void SessionStore::save(const sso::Session &s) const {
        if (auth::environment(auth::SSO_TOKEN_ENV)) return;
        ptree t; t.put("username", s.username); t.put("final_url", s.final_url);
        t.put("updated_at", s.updated_at); t.put("cookies", s.cookies);
        t.put("cas_cookie", s.cas_cookie); write(file(s.username, "sso"), t);
        set_current_user("sso", s.username);
    }
    void SessionStore::remove_sso(const std::string_view u) const {
        remove_file(file(u, "sso")); clear_current_user("sso", u);
    }

    std::optional<ecard::Session> SessionStore::load_ecard(const std::string_view user) const {
        if (const auto token = auth::environment(auth::ECARD_TOKEN_ENV)) {
            auth::validate_header_value(*token, auth::ECARD_TOKEN_ENV);
            return ecard::Session{std::string(user), *token,
                                  auth::environment(auth::ECARD_REFRESH_TOKEN_ENV)
                                      .value_or(""), {}, {}};
        }
        const auto t = read(file(user, "ecard")); if (!t) return std::nullopt;
        ecard::Session s; s.username = user;
        s.access_token = t->get("access_token", "");
        s.refresh_token = t->get("refresh_token", "");
        s.access_token_expire = t->get("access_token_expire", "");
        if (const auto p = t->get_child_optional("lighting")) s.profiles.lighting = get_path(*p);
        if (const auto p = t->get_child_optional("air")) s.profiles.air_conditioning = get_path(*p);
        return s;
    }
    void SessionStore::save(const ecard::Session &s) const {
        if (auth::environment(auth::ECARD_TOKEN_ENV) ||
            auth::environment(auth::APP_TOKEN_ENV)) return;
        ptree t, lighting, air; t.put("username", s.username);
        t.put("access_token", s.access_token); t.put("refresh_token", s.refresh_token);
        t.put("access_token_expire", s.access_token_expire);
        put_path(lighting, s.profiles.lighting); put_path(air, s.profiles.air_conditioning);
        t.put_child("lighting", lighting); t.put_child("air", air);
        write(file(s.username, "ecard"), t);
    }

    std::optional<portal::Session> SessionStore::load_portal(
        const std::string_view user) const {
        const auto t = read(file(user, "portal")); if (!t) return std::nullopt;
        return portal::Session{std::string(user), t->get("server_url", ""),
                                     t->get("user_ip", ""), t->get("isp_suffix", "")};
    }
    void SessionStore::save(const portal::Session &s) const {
        ptree t; t.put("username", s.username); t.put("server_url", s.server_url);
        t.put("user_ip", s.user_ip); t.put("isp_suffix", s.isp_suffix);
        write(file(s.username, "portal"), t); set_current_user("portal", s.username);
    }
    void SessionStore::remove_portal(const std::string_view u) const {
        remove_file(file(u, "portal")); clear_current_user("portal", u);
    }
} // namespace zzu_assistant::cli
