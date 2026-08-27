#pragma once

#include "client/options.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

// Super App authentication and account client.

namespace zzu_assistant::app {
    struct ClientOptions : NetworkOptions {
        std::string device_id;
    };

    struct LoginOptions {
        std::string_view username;
        std::string_view password;
        std::function<void(std::string_view)> notify;
        std::function<std::string(std::string_view)> prompt;
    };

    struct LoginResult {
        bool success{false};
        std::string message;
    };

    struct Session {
        std::string username;
        std::string device_id;
        std::string id_token;
        std::string refresh_token;
    };

    class AppClient final {
    public:
        explicit AppClient(ClientOptions options = {});

        ~AppClient();

        AppClient(AppClient &&) noexcept;

        AppClient &operator=(AppClient &&) noexcept;

        AppClient(const AppClient &) = delete;

        AppClient &operator=(const AppClient &) = delete;

        [[nodiscard]] LoginResult login(const LoginOptions &options);

        [[nodiscard]] LoginResult logout();

        [[nodiscard]] std::string id_token() const;

        [[nodiscard]] double card_balance() const;

        [[nodiscard]] Session session() const;

        [[nodiscard]] LoginResult login(const Session &session);

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace zzu_assistant::app
