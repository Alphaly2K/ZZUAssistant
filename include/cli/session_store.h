#pragma once

#include "client/app_client.h"
#include "client/ecard_client.h"
#include "client/portal_client.h"
#include "client/sso_client.h"

#include <optional>
#include <string>
#include <string_view>

namespace zzu_assistant::cli {
    class SessionStore final {
    public:
        [[nodiscard]] static NetworkOptions network_options();
        [[nodiscard]] static app::ClientOptions app_options();

        [[nodiscard]] std::optional<std::string> current_user(
            std::string_view scope) const;
        void set_current_user(std::string_view scope, std::string_view username) const;
        void clear_current_user(std::string_view scope, std::string_view username) const;

        [[nodiscard]] std::optional<app::Session> load_app(std::string_view username) const;
        void save(const app::Session &session) const;
        void remove_app(std::string_view username) const;

        [[nodiscard]] std::optional<sso::Session> load_sso(std::string_view username) const;
        void save(const sso::Session &session) const;
        void remove_sso(std::string_view username) const;

        [[nodiscard]] std::optional<ecard::Session> load_ecard(std::string_view username) const;
        void save(const ecard::Session &session) const;

        [[nodiscard]] std::optional<portal::Session> load_portal(
            std::string_view username) const;
        void save(const portal::Session &session) const;
        void remove_portal(std::string_view username) const;
    };
} // namespace zzu_assistant::cli
