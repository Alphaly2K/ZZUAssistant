#pragma once

#include "client/options.h"

#include <optional>
#include <memory>
#include <string>
#include <string_view>

// Client for WLAN portal authentications.

namespace zzu_assistant::portal {
    struct PortalInfo {
        std::string auth_url;
        std::string server_url;
        std::string user_ip;
    };

    struct AuthOptions {
        std::string_view server_url;
        std::string_view user_ip;
        std::string_view username;
        std::string_view password;
        std::string_view isp_suffix;
        bool encrypt_parameters{false};
    };

    struct AuthResult {
        bool success{false};
        int result{};
        std::optional<int> return_code;
        std::string message;
    };

    struct Session {
        std::string username;
        std::string server_url;
        std::string user_ip;
        std::string isp_suffix;
    };

    class PortalClient final {
    public:
        explicit PortalClient(NetworkOptions options = {});

        ~PortalClient();

        PortalClient(PortalClient &&) noexcept;

        PortalClient &operator=(PortalClient &&) noexcept;

        PortalClient(const PortalClient &) = delete;

        PortalClient &operator=(const PortalClient &) = delete;

        [[nodiscard]] PortalInfo discover();

        [[nodiscard]] AuthResult login(const AuthOptions &options);

        [[nodiscard]] AuthResult logout();

        [[nodiscard]] std::optional<Session> session() const;

        void login(const Session &session);

        [[nodiscard]] static std::string local_ipv4();

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
        std::optional<Session> session_;
    };
} // namespace zzu_assistant::portal
